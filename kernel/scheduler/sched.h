/* SPDX-License-Identifier: GPL-2.0 */
/*
 * StrayLight OS — ML-aware Scheduler
 * Copyright (C) 2026 StrayLight Systems
 *
 * Task classification, scheduling policy, topology structures.
 */

#ifndef _STRAYLIGHT_SCHED_H
#define _STRAYLIGHT_SCHED_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/cpumask.h>
#include <linux/proc_fs.h>
#include <linux/atomic.h>

/* ---- Task classification ----------------------------------------------- */

enum sl_task_class {
	SL_CLASS_SYSTEM      = 0,       /* Normal system tasks           */
	SL_CLASS_ML_INFERENCE = 1,      /* ML inference workloads        */
	SL_CLASS_ML_TRAINING  = 2,      /* ML training workloads         */
	SL_CLASS_ML_DATA      = 3,      /* Data pipeline / preprocessing */
	SL_CLASS_NR,
};

static const char * const sl_class_names[SL_CLASS_NR] = {
	[SL_CLASS_SYSTEM]       = "system",
	[SL_CLASS_ML_INFERENCE] = "ml_inference",
	[SL_CLASS_ML_TRAINING]  = "ml_training",
	[SL_CLASS_ML_DATA]      = "ml_data",
};

/* ---- Classification rules ---------------------------------------------- */

/*
 * Process name patterns used for automatic classification.
 * Checked against task->comm (16 chars max).
 */
#define SL_MAX_RULES            64
#define SL_RULE_COMM_LEN        16

struct sl_class_rule {
	char                    comm_pattern[SL_RULE_COMM_LEN];
	char                    cgroup_pattern[64];
	enum sl_task_class      task_class;
	bool                    active;
};

/* ---- Scheduling policy per class --------------------------------------- */

struct sl_class_policy {
	int                     nice_boost;      /* nice adjustment (-20..19) */
	int                     cpu_shares;      /* relative weight (1..10000) */
	bool                    prefer_p_cores;  /* bind to P-cores if avail  */
	bool                    prefer_numa_local; /* keep on local NUMA node */
	unsigned int            burst_threshold_ms; /* burst detection window */
	unsigned int            burst_boost;     /* extra priority during burst */
};

/* ---- Burst detection --------------------------------------------------- */

#define SL_BURST_WINDOW_SIZE    16

struct sl_burst_tracker {
	u64                     timestamps[SL_BURST_WINDOW_SIZE];
	unsigned int            head;
	unsigned int            count;
	spinlock_t              lock;
	bool                    burst_active;
};

/* ---- Per-task tracking ------------------------------------------------- */

struct sl_task_info {
	struct list_head        list;
	pid_t                   pid;
	char                    comm[TASK_COMM_LEN];
	enum sl_task_class      task_class;
	int                     original_nice;
	int                     boosted_nice;
	int                     assigned_cpu;
	int                     numa_node;
	struct sl_burst_tracker burst;
	u64                     last_scheduled_ns;
	u64                     total_runtime_ns;
	u64                     classify_time_ns;
};

/* ---- NUMA / core topology ---------------------------------------------- */

#define SL_MAX_NUMA_NODES       8
#define SL_MAX_CPUS             512

enum sl_core_type {
	SL_CORE_UNKNOWN = 0,
	SL_CORE_P_TYPE  = 1,   /* Performance core */
	SL_CORE_E_TYPE  = 2,   /* Efficiency core  */
};

struct sl_cpu_info {
	unsigned int            cpu_id;
	int                     numa_node;
	enum sl_core_type       core_type;
	unsigned int            base_freq_khz;
	unsigned int            max_freq_khz;
	bool                    online;
};

struct sl_topology {
	unsigned int            nr_cpus;
	unsigned int            nr_numa_nodes;
	unsigned int            nr_p_cores;
	unsigned int            nr_e_cores;
	struct sl_cpu_info      cpus[SL_MAX_CPUS];

	/* Per-NUMA node CPU masks */
	struct cpumask          numa_cpumask[SL_MAX_NUMA_NODES];

	/* Core-type CPU masks */
	struct cpumask          p_core_mask;
	struct cpumask          e_core_mask;

	/*
	 * Affinity matrix: for each task class, the preferred CPU mask.
	 * Built from topology + policy configuration.
	 */
	struct cpumask          affinity[SL_CLASS_NR];
};

/* ---- Global scheduler state -------------------------------------------- */

struct sl_scheduler {
	struct proc_dir_entry   *proc_dir;
	struct proc_dir_entry   *proc_status;
	struct proc_dir_entry   *proc_tasks;
	struct proc_dir_entry   *proc_topology;
	struct proc_dir_entry   *proc_policy;

	struct sl_class_policy  policies[SL_CLASS_NR];
	struct sl_class_rule    rules[SL_MAX_RULES];
	unsigned int            nr_rules;
	struct mutex            rules_lock;

	struct list_head        tracked_tasks;
	struct mutex            tasks_lock;

	struct sl_topology      topology;

	/* Stats */
	atomic64_t              classifications;
	atomic64_t              boosts_applied;
	atomic64_t              bursts_detected;
};

/* ---- Sub-module API ---------------------------------------------------- */

/* sched_ml.c */
enum sl_task_class sl_classify_task(struct sl_scheduler *sched,
				    const char *comm,
				    const char *cgroup_path);
int  sl_apply_priority_boost(struct sl_scheduler *sched,
			     struct sl_task_info *ti);
void sl_remove_priority_boost(struct sl_scheduler *sched,
			      struct sl_task_info *ti);
bool sl_task_pid_alive(pid_t nr);
bool sl_detect_burst(struct sl_burst_tracker *bt, u64 now_ns,
		     unsigned int threshold_ms);
int  sl_ml_init_rules(struct sl_scheduler *sched);

/* sched_topology.c */
int  sl_topology_discover(struct sl_topology *topo);
void sl_topology_build_affinity(struct sl_topology *topo,
				struct sl_class_policy *policies);
int  sl_topology_select_cpu(struct sl_topology *topo,
			    enum sl_task_class task_class,
			    int current_cpu);

#endif /* _STRAYLIGHT_SCHED_H */
