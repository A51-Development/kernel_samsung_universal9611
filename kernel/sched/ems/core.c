/*
 * Core Exynos Mobile Scheduler
 *
 * Copyright (C) 2018 Samsung Electronics Co., Ltd
 * Park Bumgyu <bumgyu.park@samsung.com>
 */

#include <linux/ems.h>

#include "../sched.h"
#include "../tune.h"
#include "ems.h"

#define CREATE_TRACE_POINTS
#include <trace/events/ems.h>

struct lb_env {
	struct rq *src_rq;
	struct rq *dst_rq;
	struct task_struct *push_task;
	u64 flags;
};

static struct lb_env __percpu *lb_env;
static struct cpu_stop_work __percpu *lb_work;

static struct cpumask slowest_mask;
static struct cpumask fastest_mask;

#define BUSIEST_EQUAL	0
#define BUSIEST_SLOWER	1
#define BUSIEST_FASTER	2

#define TINY_TASK_RATIO_SHIFT	3	/* 12.5% */
#define BUSY_GROUP_RATIO_SHIFT	2	/* 25% */
#define NIB_AVG_IDLE_THRESHOLD	500000

static bool is_per_cpu_ktask(struct task_struct *p)
{
	if (!(p->flags & PF_KTHREAD))
		return false;

	if (p->nr_cpus_allowed != 1)
		return false;

	return true;
}

static inline bool can_migrate(struct task_struct *p, int dst_cpu)
{
	if (p->exit_state)
		return false;

	if (is_per_cpu_ktask(p))
		return false;

#ifdef CONFIG_SMP
	if (task_running(NULL, p))
		return false;
#endif

	if (!cpumask_test_cpu(dst_cpu, &p->cpus_allowed))
		return false;

	return cpu_active(dst_cpu);
}

void attach_task(struct rq *dst_rq, struct task_struct *p)
{
	activate_task(dst_rq, p, ENQUEUE_NOCLOCK);
	p->on_rq = TASK_ON_RQ_QUEUED;
	check_preempt_curr(dst_rq, p, 0);
}

void attach_one_task(struct rq *dst_rq, struct task_struct *p)
{
	struct rq_flags rf;

	rq_lock(dst_rq, &rf);
	update_rq_clock(dst_rq);
	attach_task(dst_rq, p);
	rq_unlock(dst_rq, &rf);
}

void detach_task(struct rq *src_rq, struct rq *dst_rq, struct task_struct *p,
		struct rq_flags *rf)
{
	p->on_rq = TASK_ON_RQ_MIGRATING;
	deactivate_task(src_rq, p, DEQUEUE_NOCLOCK);
	rq_unpin_lock(src_rq, rf);
	double_lock_balance(src_rq, dst_rq);
	set_task_cpu(p, dst_rq->cpu);
	double_unlock_balance(src_rq, dst_rq);
	rq_unpin_lock(src_rq, rf);
}

int detach_one_task(struct rq *src_rq, struct rq *dst_rq,
		struct task_struct *target, struct rq_flags *rf)
{
	struct task_struct *p, *n;

	list_for_each_entry_safe(p, n, &src_rq->cfs_tasks, se.group_node) {
		if (p != target)
			continue;

		if (!can_migrate(p, dst_rq->cpu))
			break;

		update_rq_clock(src_rq);
		detach_task(src_rq, dst_rq, p, rf);
		return 1;
	}

	return 0;
}

static bool determine_short_idle(u64 avg_idle)
{
	return avg_idle < NIB_AVG_IDLE_THRESHOLD;
}

static unsigned long cpu_util_total(int cpu)
{
	struct cfs_rq *cfs_rq = &cpu_rq(cpu)->cfs;
	struct rt_rq *rt_rq = &cpu_rq(cpu)->rt;
	unsigned long util = READ_ONCE(cfs_rq->avg.util_avg) + READ_ONCE(rt_rq->avg.util_avg);

	return util;
}

/*
 * This function called when src rq has only one task,
 * and need to check whether need an active balance or not
 */
bool lb_queue_need_active_mgt(struct rq *src, struct rq *dst)
{
	if (!src->misfit_task_load)
		return false;

	return true;
}

bool lb_busiest_queue_pre_condition(struct rq *rq, bool check_overutil)
{
	/* if there is no fair task, we can't balance */
	if (!rq->cfs.h_nr_running)
		return false;

	/* if rq is in the active balance, will be less busy */
	if (rq->active_balance)
		return false;

	if (check_overutil && !lbt_overutilized(cpu_of(rq), 0))
		return false;

	return true;
}

bool lb_group_is_busy(struct cpumask *cpus,
			int nr_task, unsigned long util)
{
	unsigned long capacity = capacity_orig_of(cpumask_first(cpus));
	int nr_cpus = cpumask_weight(cpus);
	/*
	 * if there is a available cpu in the group,
	 * this group is not busy
	 */
	if (nr_task <= nr_cpus)
		return false;

	if ((util + (util >> BUSY_GROUP_RATIO_SHIFT)) < (capacity * nr_cpus))
		return false;

	return true;
}

static void lb_find_busiest_faster_queue(int dst_cpu,
		struct cpumask *src_cpus, struct rq **busiest)
{
	int src_cpu, busiest_cpu = -1;
	struct cpumask candidate_mask;
	unsigned long util, busiest_util = 0;
	unsigned long util_sum = 0, nr_task_sum = 0;
	unsigned long tiny_task_util;

	tiny_task_util = capacity_orig_of(cpumask_first(src_cpus)) >> TINY_TASK_RATIO_SHIFT;
	cpumask_and(&candidate_mask, src_cpus, cpu_active_mask);
	if (cpumask_empty(&candidate_mask))
		return;

	for_each_cpu(src_cpu, &candidate_mask) {
		struct rq *src_rq = cpu_rq(src_cpu);

		trace_lb_cpu_util(src_cpu, "faster");

		util = cpu_util_total(src_cpu);
		util_sum += util;
		nr_task_sum += src_rq->cfs.h_nr_running;

		if (!lb_busiest_queue_pre_condition(src_rq, true))
			continue;

		if (src_rq->cfs.h_nr_running < 2)
			continue;

		if (src_rq->cfs.h_nr_running == 2 &&
			util < tiny_task_util)
			continue;

		if (util < busiest_util)
			continue;

		busiest_util = util;
		busiest_cpu = src_cpu;
	}

	/*
	 * Don't allow migrating to lower cluster unless
	 * this faster cluster is sufficiently loaded.
	 */
	if (!lb_group_is_busy(&candidate_mask, nr_task_sum, util_sum))
		return;

	if (busiest_cpu != -1)
		*busiest = cpu_rq(busiest_cpu);
}

static void lb_find_busiest_slower_queue(int dst_cpu,
		struct cpumask *src_cpus, struct rq **busiest)
{
	int src_cpu, busiest_cpu = -1;
	unsigned long util, busiest_util = 0;


	for_each_cpu_and(src_cpu, src_cpus, cpu_active_mask) {
		struct rq *src_rq = cpu_rq(src_cpu);

		trace_lb_cpu_util(src_cpu, "slower");

		if (!lb_busiest_queue_pre_condition(src_rq, true))
			continue;

		if (src_rq->cfs.h_nr_running == 1 &&
			!lb_queue_need_active_mgt(src_rq, cpu_rq(dst_cpu)))
			continue;

		util = cpu_util_total(src_cpu);
		if (util < busiest_util)
			continue;

		busiest_util = util;
		busiest_cpu = src_cpu;
	}

	if (busiest_cpu != -1)
		*busiest = cpu_rq(busiest_cpu);
}

static void lb_find_busiest_equivalent_queue(int dst_cpu,
		struct cpumask *src_cpus, struct rq **busiest)
{
	int src_cpu, busiest_cpu = -1;
	unsigned long util, busiest_util = 0;

	for_each_cpu_and(src_cpu, src_cpus, cpu_active_mask) {
		struct rq *src_rq = cpu_rq(src_cpu);

		trace_lb_cpu_util(src_cpu, "equal");

		if (src_cpu == dst_cpu)
			continue;

		if (!lb_busiest_queue_pre_condition(src_rq, false))
			continue;

		if (src_rq->nr_running < 2)
			continue;

		/* find highest util cpu */
		util = cpu_util_total(src_cpu);
		if (util < busiest_util)
			continue;

		busiest_util = util;
		busiest_cpu = src_cpu;
	}

	if (busiest_cpu != -1)
		*busiest = cpu_rq(busiest_cpu);
}

static void __lb_find_busiest_queue(int dst_cpu,
		struct cpumask *src_cpus, struct rq **busiest, int *type)
{
	unsigned long dst_capacity, src_capacity;

	src_capacity = capacity_orig_of(cpumask_first(src_cpus));
	dst_capacity = capacity_orig_of(dst_cpu);
	if (dst_capacity == src_capacity) {
		lb_find_busiest_equivalent_queue(dst_cpu, src_cpus, busiest);
		*type = BUSIEST_EQUAL;
	} else if (dst_capacity > src_capacity) {
		lb_find_busiest_slower_queue(dst_cpu, src_cpus, busiest);
		*type = BUSIEST_SLOWER;
	} else {
		lb_find_busiest_faster_queue(dst_cpu, src_cpus, busiest);
		*type = BUSIEST_FASTER;
	}
}

static int lb_active_migration_stop(void *data)
{
	struct lb_env *env = data;
	struct rq *src_rq = env->src_rq, *dst_rq = env->dst_rq;
	struct rq_flags rf;
	int ret;

	rq_lock_irq(src_rq, &rf);
	ret = detach_one_task(src_rq, dst_rq, env->push_task, &rf);
	src_rq->active_balance = 0;
	rq_unlock(src_rq, &rf);

	if (ret)
		attach_one_task(dst_rq, env->push_task);

	local_irq_enable();

	return 0;
}

static bool _lb_can_migrate_task(struct task_struct *p, int src_cpu, int dst_cpu)
{
	if (!ontime_can_migration(p, dst_cpu)) {
		trace_lb_can_migrate_task(p, dst_cpu, false, "ontime");
		return false;
	}

	if (!cpumask_test_cpu(dst_cpu, tsk_cpus_allowed(p))) {
		trace_lb_can_migrate_task(p, dst_cpu, false, "cpus-allowed");
		return false;
	}

	trace_lb_can_migrate_task(p, dst_cpu, true, "can-migrate");

	return true;
}

static bool lb_task_need_active_mgt(struct task_struct *p, int dst_cpu, int src_cpu)
{
	struct rq *src_rq = cpu_rq(src_cpu);

	if (capacity_orig_of(dst_cpu) <= capacity_orig_of(src_cpu))
		return false;

	if (!src_rq->misfit_task_load)
		return false;

	return true;
}

static int lb_idle_pull_tasks(int dst_cpu, int src_cpu, int type)
{
	struct lb_env *env = per_cpu_ptr(lb_env, src_cpu);
	struct rq *dst_rq = cpu_rq(dst_cpu);
	struct rq *src_rq = cpu_rq(src_cpu);
	struct task_struct *pulled_task = NULL, *p;
	bool active_balance = false;
	struct rq_flags rf;
	int util, min_util = INT_MAX, count = 0;

	rq_lock_irqsave(src_rq, &rf);
	list_for_each_entry_reverse(p, &src_rq->cfs_tasks, se.group_node) {
		if (!_lb_can_migrate_task(p, src_cpu, dst_cpu))
			continue;

		if (task_running(src_rq, p))
			if (!lb_task_need_active_mgt(p, dst_cpu, src_cpu))
				continue;

		if (!can_migrate(p, dst_cpu))
			continue;

		if (type != BUSIEST_FASTER) {
			pulled_task = p;
			break;
		}

		/* find min util cpu */
		util = task_util(p);
		if (util > min_util)
			continue;

		min_util = util;
		pulled_task = p;
		if (++count > 3)
			break;
	}

	if (!pulled_task) {
		rq_unlock_irqrestore(src_rq, &rf);
		return 0;
	}

	if (task_running(src_rq, pulled_task)) {
		active_balance = true;
	} else {
		update_rq_clock(src_rq);
		detach_task(src_rq, dst_rq, pulled_task, &rf);
	}

	if (active_balance) {
		if (src_rq->active_balance) {
			rq_unlock_irqrestore(src_rq, &rf);
			return 0;
		}

		src_rq->active_balance = true;
		env->src_rq = src_rq;
		env->dst_rq = dst_rq;
		env->push_task = pulled_task;

		/* lock must be dropped before waking the stopper */
		rq_unlock_irqrestore(src_rq, &rf);

		trace_lb_active_migration(p, src_cpu, dst_cpu, "idle");
		stop_one_cpu_nowait(src_cpu, lb_active_migration_stop,
				env, per_cpu_ptr(lb_work, src_cpu));

		/* we did not pull any task here */
		return 0;
	}

	raw_spin_unlock(&src_rq->lock);

	attach_one_task(dst_rq, pulled_task);

	local_irq_restore(rf.flags);

	return 1;
}

struct rq *lb_find_busiest_queue(struct rq *dst_rq, int *type)
{
	struct rq *busiest = NULL;
	int dst_cpu = dst_rq->cpu;

	/* if this rq has a task, stop idle-pull */
	if (dst_rq->nr_running > 0)
		goto out;

	if (cpumask_test_cpu(dst_cpu, &slowest_mask)) {
		__lb_find_busiest_queue(dst_cpu, &slowest_mask, &busiest, type);
		if (busiest)
			goto out;

		__lb_find_busiest_queue(dst_cpu, &fastest_mask, &busiest, type);
	} else {
		__lb_find_busiest_queue(dst_cpu, &fastest_mask, &busiest, type);
		if (busiest)
			goto out;

		__lb_find_busiest_queue(dst_cpu, &slowest_mask, &busiest, type);
	}
out:
	return busiest;
}

void lb_newidle_balance(struct rq *dst_rq, struct rq_flags *rf,
				int *pulled_task, struct sched_domain *sd)
{
	int dst_cpu = dst_rq->cpu;
	bool short_idle;
	struct rq *busiest = NULL;
	int type, src_cpu = -1;

	dst_rq->misfit_task_load = 0;
	short_idle = determine_short_idle(dst_rq->avg_idle);
	dst_rq->idle_stamp = rq_clock(dst_rq);

	/*
	 * Do not pull tasks towards !active CPUs...
	 */
	if (!cpu_active(dst_cpu))
		return;

	if (cpumask_empty(&slowest_mask) || cpumask_empty(&fastest_mask))
		return;

	rq_unpin_lock(dst_rq, rf);

	/* to check ttwu_pending */
	if (!llist_empty(&dst_rq->wake_list))
		goto out;

	if (dst_rq->nr_running)
		goto out;

	if (frt_idle_pull_tasks(dst_rq))
		goto out;

	/* sanity check again after drop rq lock during RT balance */
	if (dst_rq->nr_running)
		goto out;

	if (!READ_ONCE(dst_rq->rd->overload))
		goto out;

	if (atomic_read(&dst_rq->nr_iowait) && short_idle)
		goto out;

	raw_spin_unlock(&dst_rq->lock);

	busiest = lb_find_busiest_queue(dst_rq, &type);
	if (busiest) {
		src_cpu = cpu_of(busiest);
		if (dst_cpu != src_cpu)
			*pulled_task = lb_idle_pull_tasks(dst_cpu, src_cpu, type);
	}

	raw_spin_lock(&dst_rq->lock);
out:
	/*
	 * While browsing the domains, we released the rq lock, a task could
	 * have been enqueued in the meantime. Since we're not going idle,
	 * pretend we pulled a task.
	 */
	if (dst_rq->cfs.h_nr_running && !*pulled_task)
		*pulled_task = 1;

	/* Is there a task of a high priority class? */
	if (dst_rq->nr_running != dst_rq->cfs.h_nr_running)
		*pulled_task = -1;

	if (*pulled_task)
		dst_rq->idle_stamp = 0;

	rq_repin_lock(dst_rq, rf);

	trace_lb_newidle_balance(dst_cpu, src_cpu, *pulled_task, short_idle);
}

void update_last_waked_ns_task(struct task_struct *p)
{
	p->last_waked_ns = ktime_get_ns();
}

unsigned long cpu_util(int cpu)
{
	struct cfs_rq *cfs_rq;
	unsigned int util;

#ifdef CONFIG_SCHED_WALT
	if (likely(!walt_disabled && sysctl_sched_use_walt_cpu_util)) {
		u64 walt_cpu_util = cpu_rq(cpu)->cumulative_runnable_avg;

		walt_cpu_util <<= SCHED_CAPACITY_SHIFT;
		do_div(walt_cpu_util, walt_ravg_window);

		return min_t(unsigned long, walt_cpu_util,
			     capacity_orig_of(cpu));
	}
#endif

	cfs_rq = &cpu_rq(cpu)->cfs;
	util = READ_ONCE(cfs_rq->avg.util_avg);

	if (sched_feat(UTIL_EST))
		util = max(util, READ_ONCE(cfs_rq->avg.util_est.enqueued));

	return min_t(unsigned long, util, capacity_orig_of(cpu));
}

unsigned long task_util(struct task_struct *p)
{
	if (rt_task(p))
		return p->rt.avg.util_avg;
	else
		return p->se.avg.util_avg;
}

int cpu_util_wake(int cpu, struct task_struct *p)
{
	struct cfs_rq *cfs_rq;
	unsigned int util;

	/* Task has no contribution or is new */
	if (cpu != task_cpu(p) || !READ_ONCE(p->se.avg.last_update_time))
		return cpu_util(cpu);

	cfs_rq = &cpu_rq(cpu)->cfs;
	util = READ_ONCE(cfs_rq->avg.util_avg);

	/* Discount task's blocked util from CPU's util */
	util -= min_t(unsigned int, util, task_util_est(p));

	/*
	 * Covered cases:
	 *
	 * a) if *p is the only task sleeping on this CPU, then:
	 *      cpu_util (== task_util) > util_est (== 0)
	 *    and thus we return:
	 *      cpu_util_wake = (cpu_util - task_util) = 0
	 *
	 * b) if other tasks are SLEEPING on this CPU, which is now exiting
	 *    IDLE, then:
	 *      cpu_util >= task_util
	 *      cpu_util > util_est (== 0)
	 *    and thus we discount *p's blocked utilization to return:
	 *      cpu_util_wake = (cpu_util - task_util) >= 0
	 *
	 * c) if other tasks are RUNNABLE on that CPU and
	 *      util_est > cpu_util
	 *    then we use util_est since it returns a more restrictive
	 *    estimation of the spare capacity on that CPU, by just
	 *    considering the expected utilization of tasks already
	 *    runnable on that CPU.
	 *
	 * Cases a) and b) are covered by the above code, while case c) is
	 * covered by the following code when estimated utilization is
	 * enabled.
	 */
	if (sched_feat(UTIL_EST))
		util = max(util, READ_ONCE(cfs_rq->avg.util_est.enqueued));

	/*
	 * Utilization (estimated) can exceed the CPU capacity, thus let's
	 * clamp to the maximum CPU capacity to ensure consistency with
	 * the cpu_util call.
	 */
	return min_t(unsigned long, util, capacity_orig_of(cpu));
}

static inline int
check_cpu_capacity(struct rq *rq, struct sched_domain *sd)
{
	return ((rq->cpu_capacity * sd->imbalance_pct) <
				(rq->cpu_capacity_orig * 100));
}

#define lb_sd_parent(sd) \
	(sd->parent && sd->parent->groups != sd->parent->groups->next)

int exynos_need_active_balance(enum cpu_idle_type idle, struct sched_domain *sd,
					int src_cpu, int dst_cpu)
{
	unsigned int src_imb_pct = lb_sd_parent(sd) ? sd->imbalance_pct : 1;
	unsigned int dst_imb_pct = lb_sd_parent(sd) ? 100 : 1;
	unsigned long src_cap = capacity_of(src_cpu);
	unsigned long dst_cap = capacity_of(dst_cpu);
	int level = sd->level;

	/* dst_cpu is idle */
	if ((idle != CPU_NOT_IDLE) &&
	    (cpu_rq(src_cpu)->cfs.h_nr_running == 1)) {
		if ((check_cpu_capacity(cpu_rq(src_cpu), sd)) &&
		    (src_cap * sd->imbalance_pct < dst_cap * 100)) {
			return 1;
		}

		/* This domain is top and dst_cpu is bigger than src_cpu*/
		if (!lb_sd_parent(sd) && src_cap < dst_cap)
			if (lbt_overutilized(src_cpu, level) || global_boosted())
				return 1;
	}

	if ((src_cap * src_imb_pct < dst_cap * dst_imb_pct) &&
			cpu_rq(src_cpu)->cfs.h_nr_running == 1 &&
			lbt_overutilized(src_cpu, level) &&
			!lbt_overutilized(dst_cpu, level)) {
		return 1;
	}

	return unlikely(sd->nr_balance_failed > sd->cache_nice_tries + 2);
}

static int select_proper_cpu(struct task_struct *p, int prev_cpu)
{
	int cpu;
	unsigned long best_min_util = ULONG_MAX;
	int best_cpu = -1;

	for_each_cpu(cpu, cpu_active_mask) {
		int i;

		/* visit each coregroup only once */
		if (cpu != cpumask_first(cpu_coregroup_mask(cpu)))
			continue;

		/* skip if task cannot be assigned to coregroup */
		if (!cpumask_intersects(&p->cpus_allowed, cpu_coregroup_mask(cpu)))
			continue;

		for_each_cpu_and(i, tsk_cpus_allowed(p), cpu_coregroup_mask(cpu)) {
			unsigned long capacity_orig = capacity_orig_of(i);
			unsigned long wake_util, new_util;

			wake_util = cpu_util_wake(i, p);
			new_util = wake_util + task_util_est(p);
			new_util = max(new_util, boosted_task_util(p));

			/* skip over-capacity cpu */
			if (new_util > capacity_orig)
				continue;

			/*
			 * Best target) lowest utilization among lowest-cap cpu
			 *
			 * If the sequence reaches this function, the wakeup task
			 * does not require performance and the prev cpu is over-
			 * utilized, so it should do load balancing without
			 * considering energy side. Therefore, it selects cpu
			 * with smallest cpapacity and the least utilization among
			 * cpu that fits the task.
			 */
			if (best_min_util < new_util)
				continue;

			best_min_util = new_util;
			best_cpu = i;
		}

		/*
		 * if it fails to find the best cpu in this coregroup, visit next
		 * coregroup.
		 */
		if (cpu_selected(best_cpu))
			break;
	}

	trace_ems_select_proper_cpu(p, best_cpu, best_min_util);

	/*
	 * if it fails to find the vest cpu, choosing any cpu is meaningless.
	 * Return prev cpu.
	 */
	return cpu_selected(best_cpu) ? best_cpu : prev_cpu;
}

extern void sync_entity_load_avg(struct sched_entity *se);

int exynos_wakeup_balance(struct task_struct *p, int prev_cpu, int sd_flag, int sync)
{
	int target_cpu = -1;
	char state[30] = "fail";

	/*
	 * Since the utilization of a task is accumulated before sleep, it updates
	 * the utilization to determine which cpu the task will be assigned to.
	 * Exclude new task.
	 */
	if (!(sd_flag & SD_BALANCE_FORK)) {
		unsigned long old_util = task_util(p);

		sync_entity_load_avg(&p->se);
		/* update the band if a large amount of task util is decayed */
		update_band(p, old_util);
	}

	target_cpu = select_service_cpu(p);
	if (cpu_selected(target_cpu)) {
		strcpy(state, "service");
		goto out;
	}

	/*
	 * Priority 1 : ontime task
	 *
	 * If task which has more utilization than threshold wakes up, the task is
	 * classified as "ontime task" and assigned to performance cpu. Conversely,
	 * if heavy task that has been classified as ontime task sleeps for a long
	 * time and utilization becomes small, it is excluded from ontime task and
	 * is no longer guaranteed to operate on performance cpu.
	 *
	 * Ontime task is very sensitive to performance because it is usually the
	 * main task of application. Therefore, it has the highest priority.
	 */
	target_cpu = ontime_task_wakeup(p, sync);
	if (cpu_selected(target_cpu)) {
		strcpy(state, "ontime migration");
		goto out;
	}

	/*
	 * Priority 2 : prefer-perf
	 *
	 * Prefer-perf is a function that operates on cgroup basis managed by
	 * schedtune. When perfer-perf is set to 1, the tasks in the group are
	 * preferentially assigned to the performance cpu.
	 *
	 * It has a high priority because it is a function that is turned on
	 * temporarily in scenario requiring reactivity(touch, app laucning).
	 */
	target_cpu = prefer_perf_cpu(p);
	if (cpu_selected(target_cpu)) {
		strcpy(state, "prefer-perf");
		goto out;
	}

	/*
	 * Priority 3 : task band
	 *
	 * The tasks in a process are likely to interact, and its operations are
	 * sequential and share resources. Therefore, if these tasks are packed and
	 * and assign on a specific cpu or cluster, the latency for interaction
	 * decreases and the reusability of the cache increases, thereby improving
	 * performance.
	 *
	 * The "task band" is a function that groups tasks on a per-process basis
	 * and assigns them to a specific cpu or cluster. If the attribute "band"
	 * of schedtune.cgroup is set to '1', task band operate on this cgroup.
	 */
	target_cpu = band_play_cpu(p);
	if (cpu_selected(target_cpu)) {
		strcpy(state, "task band");
		goto out;
	}

	/*
	 * Priority 4 : global boosting
	 *
	 * Global boost is a function that preferentially assigns all tasks in the
	 * system to the performance cpu. Unlike prefer-perf, which targets only
	 * group tasks, global boost targets all tasks. So, it maximizes performance
	 * cpu utilization.
	 *
	 * Typically, prefer-perf operates on groups that contains UX related tasks,
	 * such as "top-app" or "foreground", so that major tasks are likely to be
	 * assigned to performance cpu. On the other hand, global boost assigns
	 * all tasks to performance cpu, which is not as effective as perfer-perf.
	 * For this reason, global boost has a lower priority than prefer-perf.
	 */
	target_cpu = global_boosting(p);
	if (cpu_selected(target_cpu)) {
		strcpy(state, "global boosting");
		goto out;
	}

	/*
	 * Priority 5 : prefer-idle
	 *
	 * Prefer-idle is a function that operates on cgroup basis managed by
	 * schedtune. When perfer-idle is set to 1, the tasks in the group are
	 * preferentially assigned to the idle cpu.
	 *
	 * Prefer-idle has a smaller performance impact than the above. Therefore
	 * it has a relatively low priority.
	 */
	target_cpu = prefer_idle_cpu(p);
	if (cpu_selected(target_cpu)) {
		strcpy(state, "prefer-idle");
		goto out;
	}

	/*
	 * Priority 6 : energy cpu
	 *
	 * A scheduling scheme based on cpu energy, find the least power consumption
	 * cpu with energy table when assigning task.
	 */
	target_cpu = select_energy_cpu(p, prev_cpu, sd_flag, sync);
	if (cpu_selected(target_cpu)) {
		strcpy(state, "energy cpu");
		goto out;
	}

	/*
	 * Priority 7 : proper cpu
	 *
	 * If the task failed to find a cpu to assign from the above conditions,
	 * it means that assigning task to any cpu does not have performance and
	 * power benefit. In this case, select cpu for balancing cpu utilization.
	 */
	target_cpu = select_proper_cpu(p, prev_cpu);
	if (cpu_selected(target_cpu))
		strcpy(state, "proper cpu");

out:
	trace_ems_wakeup_balance(p, target_cpu, state);
	return target_cpu;
}

const struct cpumask *cpu_slowest_mask(void)
{
	return &slowest_mask;
}

const struct cpumask *cpu_fastest_mask(void)
{
	return &fastest_mask;
}

static void cpumask_speed_init(void)
{
	cpumask_clear(&slowest_mask);
	cpumask_clear(&fastest_mask);
	cpumask_copy(&slowest_mask, cpu_coregroup_mask(0));
	cpumask_copy(&fastest_mask, cpu_coregroup_mask(4));
}

struct kobject *ems_kobj;

static int __init init_sysfs(void)
{
	cpumask_speed_init();
	ems_kobj = kobject_create_and_add("ems", kernel_kobj);

	lb_env = alloc_percpu(struct lb_env);
	lb_work = alloc_percpu(struct cpu_stop_work);

	return 0;
}
core_initcall(init_sysfs);
