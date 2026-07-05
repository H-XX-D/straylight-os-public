// SPDX-License-Identifier: GPL-2.0
/*
 * StrayLight OS — ML Task Classifier & Priority Booster
 * Copyright (C) 2026 StrayLight Systems
 *
 * Classifies tasks by comm name / cgroup, applies nice-value boosts,
 * detects compute bursts for additional priority elevation.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/ktime.h>
#include <linux/pid.h>

#include "sched.h"

/* ---- Default classification rules -------------------------------------- */

/*
 * Rules are evaluated in order; first match wins.
 * comm_pattern: prefix match against task->comm
 * cgroup_pattern: substring match against cgroup path (if non-empty)
 */

struct default_rule {
	const char              *comm;
	const char              *cgroup;
	enum sl_task_class      cls;
};

static const struct default_rule builtin_rules[] = {
	/* ML inference runtimes */
	{ "python",         "ml/inference",   SL_CLASS_ML_INFERENCE },
	{ "tritonserver",   "",               SL_CLASS_ML_INFERENCE },
	{ "trtexec",        "",               SL_CLASS_ML_INFERENCE },
	{ "onnxruntime",    "",               SL_CLASS_ML_INFERENCE },
	{ "vllm",           "",               SL_CLASS_ML_INFERENCE },
	{ "llama",          "",               SL_CLASS_ML_INFERENCE },
	{ "ollama",         "",               SL_CLASS_ML_INFERENCE },
	{ "llamafile",      "",               SL_CLASS_ML_INFERENCE },
	{ "tgi",            "",               SL_CLASS_ML_INFERENCE },
	{ "mlx_lm",        "",               SL_CLASS_ML_INFERENCE },

	/* ML training frameworks */
	{ "python",         "ml/training",    SL_CLASS_ML_TRAINING },
	{ "torchrun",       "",               SL_CLASS_ML_TRAINING },
	{ "deepspeed",      "",               SL_CLASS_ML_TRAINING },
	{ "horovod",        "",               SL_CLASS_ML_TRAINING },
	{ "accelerate",     "",               SL_CLASS_ML_TRAINING },
	{ "fairseq",        "",               SL_CLASS_ML_TRAINING },
	{ "nemo_",          "",               SL_CLASS_ML_TRAINING },
	{ "lightning",      "",               SL_CLASS_ML_TRAINING },

	/* Data pipelines */
	{ "python",         "ml/data",        SL_CLASS_ML_DATA },
	{ "spark",          "",               SL_CLASS_ML_DATA },
	{ "ray",            "",               SL_CLASS_ML_DATA },
	{ "dask",           "",               SL_CLASS_ML_DATA },
	{ "celery",         "",               SL_CLASS_ML_DATA },
	{ "airflow",        "",               SL_CLASS_ML_DATA },
	{ "prefect",        "",               SL_CLASS_ML_DATA },
	{ "dagster",        "",               SL_CLASS_ML_DATA },

	/* GPU-related processes get inference by default */
	{ "nvidia-smi",     "",               SL_CLASS_ML_INFERENCE },
	{ "cuda",           "",               SL_CLASS_ML_INFERENCE },
	{ "nv_",            "",               SL_CLASS_ML_INFERENCE },

	{ NULL, NULL, SL_CLASS_SYSTEM },
};

/* ---- simple prefix match ----------------------------------------------- */

static bool comm_prefix_match(const char *comm, const char *pattern)
{
	size_t plen;

	if (!pattern || pattern[0] == '\0')
		return false;

	plen = strlen(pattern);
	return strncmp(comm, pattern, plen) == 0;
}

static bool cgroup_substring_match(const char *cgroup_path,
				   const char *pattern)
{
	if (!pattern || pattern[0] == '\0')
		return true; /* empty pattern = don't care */
	if (!cgroup_path || cgroup_path[0] == '\0')
		return false;

	return strstr(cgroup_path, pattern) != NULL;
}

/* ---- Rule initialization ----------------------------------------------- */

int sl_ml_init_rules(struct sl_scheduler *sched)
{
	unsigned int i;

	mutex_lock(&sched->rules_lock);
	sched->nr_rules = 0;

	for (i = 0; builtin_rules[i].comm != NULL; i++) {
		struct sl_class_rule *r;

		if (sched->nr_rules >= SL_MAX_RULES)
			break;

		r = &sched->rules[sched->nr_rules];
		strscpy(r->comm_pattern, builtin_rules[i].comm,
			SL_RULE_COMM_LEN);
		strscpy(r->cgroup_pattern, builtin_rules[i].cgroup,
			sizeof(r->cgroup_pattern));
		r->task_class = builtin_rules[i].cls;
		r->active = true;
		sched->nr_rules++;
	}

	mutex_unlock(&sched->rules_lock);

	pr_info("straylight-sched: loaded %u classification rules\n",
		sched->nr_rules);
	return 0;
}

/* ---- Task classification ----------------------------------------------- */

enum sl_task_class sl_classify_task(struct sl_scheduler *sched,
				    const char *comm,
				    const char *cgroup_path)
{
	unsigned int i;
	enum sl_task_class result = SL_CLASS_SYSTEM;

	if (!comm)
		return SL_CLASS_SYSTEM;

	mutex_lock(&sched->rules_lock);

	for (i = 0; i < sched->nr_rules; i++) {
		struct sl_class_rule *r = &sched->rules[i];

		if (!r->active)
			continue;

		if (!comm_prefix_match(comm, r->comm_pattern))
			continue;

		if (!cgroup_substring_match(cgroup_path, r->cgroup_pattern))
			continue;

		result = r->task_class;
		break;
	}

	mutex_unlock(&sched->rules_lock);
	return result;
}

/* ---- Priority boosting ------------------------------------------------- */

static struct task_struct *sl_get_task_by_pid(pid_t nr)
{
	struct pid *pid;
	struct task_struct *task;

	pid = find_get_pid(nr);
	if (!pid)
		return NULL;

	rcu_read_lock();
	task = pid_task(pid, PIDTYPE_PID);
	if (task)
		get_task_struct(task);
	rcu_read_unlock();

	put_pid(pid);
	return task;
}

bool sl_task_pid_alive(pid_t nr)
{
	struct pid *pid;
	struct task_struct *task;

	pid = find_get_pid(nr);
	if (!pid)
		return false;

	rcu_read_lock();
	task = pid_task(pid, PIDTYPE_PID);
	rcu_read_unlock();
	put_pid(pid);

	return task != NULL;
}

int sl_apply_priority_boost(struct sl_scheduler *sched,
			    struct sl_task_info *ti)
{
	struct sl_class_policy *pol;
	struct task_struct *task;
	int target_nice;

	if (ti->task_class >= SL_CLASS_NR)
		return -EINVAL;

	pol = &sched->policies[ti->task_class];

	/* Calculate target nice value */
	target_nice = ti->original_nice + pol->nice_boost;

	/* Clamp to valid range */
	if (target_nice < -20)
		target_nice = -20;
	if (target_nice > 19)
		target_nice = 19;

	/* Apply burst bonus if active */
	if (ti->burst.burst_active && pol->burst_boost > 0) {
		target_nice -= pol->burst_boost;
		if (target_nice < -20)
			target_nice = -20;
	}

	if (target_nice == ti->boosted_nice)
		return 0; /* no change needed */

	task = sl_get_task_by_pid(ti->pid);
	if (!task)
		return -ESRCH;

	set_user_nice(task, target_nice);
	ti->boosted_nice = target_nice;

	/* Optionally set CPU affinity based on topology */
	if (sched->topology.nr_cpus > 0) {
		int cpu = sl_topology_select_cpu(&sched->topology,
							 ti->task_class,
							 ti->assigned_cpu);
		if (cpu >= 0 && cpu != ti->assigned_cpu) {
			struct cpumask mask;

			cpumask_clear(&mask);
			cpumask_set_cpu(cpu, &mask);
			set_cpus_allowed_ptr(task, &mask);
			ti->assigned_cpu = cpu;
			ti->numa_node = cpu_to_node(cpu);
		}
	}

	put_task_struct(task);

	atomic64_inc(&sched->boosts_applied);
	return 0;
}

void sl_remove_priority_boost(struct sl_scheduler *sched,
			      struct sl_task_info *ti)
{
	struct task_struct *task;

	if (ti->boosted_nice == ti->original_nice)
		return;

	task = sl_get_task_by_pid(ti->pid);
	if (!task)
		return;

	set_user_nice(task, ti->original_nice);
	ti->boosted_nice = ti->original_nice;

	/* Restore full CPU affinity */
	set_cpus_allowed_ptr(task, cpu_online_mask);

	put_task_struct(task);
}

/* ---- Burst detection --------------------------------------------------- */

/*
 * A "burst" is detected when SL_BURST_WINDOW_SIZE scheduling events
 * occur within threshold_ms milliseconds.  This indicates sustained
 * compute-intensive behavior that warrants additional priority.
 */

bool sl_detect_burst(struct sl_burst_tracker *bt, u64 now_ns,
		     unsigned int threshold_ms)
{
	u64 window_ns;
	u64 oldest;
	unsigned long flags;
	bool was_burst;

	if (threshold_ms == 0)
		return false;

	window_ns = (u64)threshold_ms * NSEC_PER_MSEC;

	spin_lock_irqsave(&bt->lock, flags);

	was_burst = bt->burst_active;

	/* Record this event */
	bt->timestamps[bt->head] = now_ns;
	bt->head = (bt->head + 1) % SL_BURST_WINDOW_SIZE;
	if (bt->count < SL_BURST_WINDOW_SIZE)
		bt->count++;

	/* Check if the window is full and dense enough */
	if (bt->count == SL_BURST_WINDOW_SIZE) {
		/* Oldest timestamp in the circular buffer */
		oldest = bt->timestamps[bt->head % SL_BURST_WINDOW_SIZE];

		if ((now_ns - oldest) <= window_ns) {
			bt->burst_active = true;
		} else {
			bt->burst_active = false;
		}
	} else {
		bt->burst_active = false;
	}

	spin_unlock_irqrestore(&bt->lock, flags);

	/* Return true only on transition to burst state */
	return bt->burst_active && !was_burst;
}
