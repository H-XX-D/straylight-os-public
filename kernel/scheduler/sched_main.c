// SPDX-License-Identifier: GPL-2.0
/*
 * StrayLight OS — ML-aware Scheduler Main Module
 * Copyright (C) 2026 StrayLight Systems
 *
 * Module init/exit, /proc/straylight/sched interface, task tracking.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/ktime.h>
#include <linux/uaccess.h>
#include <linux/string.h>

#include "sched.h"

static struct sl_scheduler *g_sched;

/* ---- Default policies -------------------------------------------------- */

static const struct sl_class_policy default_policies[SL_CLASS_NR] = {
	[SL_CLASS_SYSTEM] = {
		.nice_boost        = 0,
		.cpu_shares        = 1024,
		.prefer_p_cores    = false,
		.prefer_numa_local = false,
		.burst_threshold_ms = 0,
		.burst_boost       = 0,
	},
	[SL_CLASS_ML_INFERENCE] = {
		.nice_boost        = -5,
		.cpu_shares        = 4096,
		.prefer_p_cores    = true,
		.prefer_numa_local = true,
		.burst_threshold_ms = 50,
		.burst_boost       = 3,
	},
	[SL_CLASS_ML_TRAINING] = {
		.nice_boost        = -10,
		.cpu_shares        = 8192,
		.prefer_p_cores    = true,
		.prefer_numa_local = true,
		.burst_threshold_ms = 200,
		.burst_boost       = 5,
	},
	[SL_CLASS_ML_DATA] = {
		.nice_boost        = -3,
		.cpu_shares        = 2048,
		.prefer_p_cores    = false,
		.prefer_numa_local = true,
		.burst_threshold_ms = 100,
		.burst_boost       = 2,
	},
};

/* ---- /proc/straylight/sched/status ------------------------------------- */

static int sched_status_show(struct seq_file *m, void *v)
{
	struct sl_scheduler *s = g_sched;
	struct sl_task_info *ti;
	unsigned int counts[SL_CLASS_NR] = {0};
	int i;

	if (!s)
		return -ENODEV;

	seq_puts(m, "StrayLight ML-Aware Scheduler\n");
	seq_puts(m, "=============================\n\n");

	/* Topology summary */
	seq_printf(m, "CPUs: %u (P-cores: %u, E-cores: %u)\n",
		   s->topology.nr_cpus,
		   s->topology.nr_p_cores,
		   s->topology.nr_e_cores);
	seq_printf(m, "NUMA nodes: %u\n\n", s->topology.nr_numa_nodes);

	/* Task class counts */
	mutex_lock(&s->tasks_lock);
	list_for_each_entry(ti, &s->tracked_tasks, list) {
		if (ti->task_class < SL_CLASS_NR)
			counts[ti->task_class]++;
	}
	mutex_unlock(&s->tasks_lock);

	seq_puts(m, "Task Classification:\n");
	for (i = 0; i < SL_CLASS_NR; i++)
		seq_printf(m, "  %-16s  %u tasks\n", sl_class_names[i], counts[i]);

	seq_printf(m, "\nTotal classifications: %lld\n",
		   atomic64_read(&s->classifications));
	seq_printf(m, "Priority boosts applied: %lld\n",
		   atomic64_read(&s->boosts_applied));
	seq_printf(m, "Bursts detected: %lld\n",
		   atomic64_read(&s->bursts_detected));

	return 0;
}

static int sched_status_open(struct inode *inode, struct file *file)
{
	return single_open(file, sched_status_show, NULL);
}

static const struct proc_ops sched_status_ops = {
	.proc_open    = sched_status_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

/* ---- /proc/straylight/sched/tasks -------------------------------------- */

static int sched_tasks_show(struct seq_file *m, void *v)
{
	struct sl_scheduler *s = g_sched;
	struct sl_task_info *ti;

	if (!s)
		return -ENODEV;

	seq_printf(m, "%-8s %-16s %-14s %-6s %-6s %-6s %-6s %s\n",
		   "PID", "COMM", "CLASS", "NICE", "BOOST", "CPU", "NUMA",
		   "BURST");
	seq_puts(m, "-------- ---------------- -------------- "
		    "------ ------ ------ ------ -----\n");

	mutex_lock(&s->tasks_lock);
	list_for_each_entry(ti, &s->tracked_tasks, list) {
		seq_printf(m, "%-8d %-16s %-14s %-6d %-6d %-6d %-6d %s\n",
			   ti->pid,
			   ti->comm,
			   sl_class_names[ti->task_class],
			   ti->original_nice,
			   ti->boosted_nice,
			   ti->assigned_cpu,
			   ti->numa_node,
			   ti->burst.burst_active ? "yes" : "no");
	}
	mutex_unlock(&s->tasks_lock);

	return 0;
}

static int sched_tasks_open(struct inode *inode, struct file *file)
{
	return single_open(file, sched_tasks_show, NULL);
}

static const struct proc_ops sched_tasks_ops = {
	.proc_open    = sched_tasks_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

/* ---- /proc/straylight/sched/topology ----------------------------------- */

static int sched_topology_show(struct seq_file *m, void *v)
{
	struct sl_scheduler *s = g_sched;
	struct sl_topology *t;
	unsigned int i;

	if (!s)
		return -ENODEV;

	t = &s->topology;

	seq_puts(m, "CPU Topology\n");
	seq_puts(m, "============\n\n");

	for (i = 0; i < t->nr_cpus; i++) {
		struct sl_cpu_info *ci = &t->cpus[i];
		const char *type;

		switch (ci->core_type) {
		case SL_CORE_P_TYPE: type = "P-core"; break;
		case SL_CORE_E_TYPE: type = "E-core"; break;
		default:             type = "unknown"; break;
		}

		seq_printf(m, "  CPU %-4u  NUMA %-2d  %-8s  "
			   "base=%u MHz  max=%u MHz  %s\n",
			   ci->cpu_id, ci->numa_node, type,
			   ci->base_freq_khz / 1000,
			   ci->max_freq_khz / 1000,
			   ci->online ? "online" : "offline");
	}

	seq_puts(m, "\nAffinity Masks:\n");
	for (i = 0; i < SL_CLASS_NR; i++) {
		seq_printf(m, "  %-16s  cpus=%*pbl\n",
			   sl_class_names[i],
			   cpumask_pr_args(&t->affinity[i]));
	}

	return 0;
}

static int sched_topology_open(struct inode *inode, struct file *file)
{
	return single_open(file, sched_topology_show, NULL);
}

static const struct proc_ops sched_topology_ops = {
	.proc_open    = sched_topology_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

/* ---- /proc/straylight/sched/policy ------------------------------------- */

static int sched_policy_show(struct seq_file *m, void *v)
{
	struct sl_scheduler *s = g_sched;
	int i;

	if (!s)
		return -ENODEV;

	seq_puts(m, "Scheduling Policies\n");
	seq_puts(m, "===================\n\n");

	for (i = 0; i < SL_CLASS_NR; i++) {
		struct sl_class_policy *p = &s->policies[i];

		seq_printf(m, "[%s]\n", sl_class_names[i]);
		seq_printf(m, "  nice_boost        = %d\n", p->nice_boost);
		seq_printf(m, "  cpu_shares        = %d\n", p->cpu_shares);
		seq_printf(m, "  prefer_p_cores    = %s\n",
			   p->prefer_p_cores ? "yes" : "no");
		seq_printf(m, "  prefer_numa_local = %s\n",
			   p->prefer_numa_local ? "yes" : "no");
		seq_printf(m, "  burst_threshold   = %u ms\n",
			   p->burst_threshold_ms);
		seq_printf(m, "  burst_boost       = %u\n\n",
			   p->burst_boost);
	}

	return 0;
}

static int sched_policy_open(struct inode *inode, struct file *file)
{
	return single_open(file, sched_policy_show, NULL);
}

/*
 * Policy write format (one per line):
 *   <class_name> <field> <value>
 * Example:
 *   ml_training nice_boost -15
 */
static ssize_t sched_policy_write(struct file *file,
				  const char __user *ubuf,
				  size_t count, loff_t *ppos)
{
	struct sl_scheduler *s = g_sched;
	char buf[128];
	char class_name[32];
	char field[32];
	int value;
	int class_idx = -1;
	int i, ret;

	if (!s)
		return -ENODEV;

	if (count >= sizeof(buf))
		return -EINVAL;

	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';

	ret = sscanf(buf, "%31s %31s %d", class_name, field, &value);
	if (ret != 3)
		return -EINVAL;

	for (i = 0; i < SL_CLASS_NR; i++) {
		if (strcmp(class_name, sl_class_names[i]) == 0) {
			class_idx = i;
			break;
		}
	}

	if (class_idx < 0)
		return -EINVAL;

	if (strcmp(field, "nice_boost") == 0) {
		if (value < -20 || value > 19)
			return -EINVAL;
		s->policies[class_idx].nice_boost = value;
	} else if (strcmp(field, "cpu_shares") == 0) {
		if (value < 1 || value > 10000)
			return -EINVAL;
		s->policies[class_idx].cpu_shares = value;
	} else if (strcmp(field, "prefer_p_cores") == 0) {
		s->policies[class_idx].prefer_p_cores = !!value;
	} else if (strcmp(field, "prefer_numa_local") == 0) {
		s->policies[class_idx].prefer_numa_local = !!value;
	} else if (strcmp(field, "burst_threshold") == 0) {
		if (value < 0 || value > 10000)
			return -EINVAL;
		s->policies[class_idx].burst_threshold_ms = value;
	} else if (strcmp(field, "burst_boost") == 0) {
		if (value < 0 || value > 20)
			return -EINVAL;
		s->policies[class_idx].burst_boost = value;
	} else {
		return -EINVAL;
	}

	/* Rebuild affinity masks after policy change */
	sl_topology_build_affinity(&s->topology, s->policies);

	pr_info("straylight-sched: policy %s.%s = %d\n",
		class_name, field, value);
	return count;
}

static const struct proc_ops sched_policy_ops = {
	.proc_open    = sched_policy_open,
	.proc_read    = seq_read,
	.proc_write   = sched_policy_write,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

/* ---- Scan running tasks and classify ----------------------------------- */

static void sl_scan_tasks(struct sl_scheduler *s)
{
	struct task_struct *task;

	rcu_read_lock();
	for_each_process(task) {
		struct sl_task_info *ti;
		enum sl_task_class cls;
		bool found = false;

		/* Skip kernel threads */
		if (task->flags & PF_KTHREAD)
			continue;

		/* Check if already tracked */
		mutex_lock(&s->tasks_lock);
		list_for_each_entry(ti, &s->tracked_tasks, list) {
			if (ti->pid == task->pid) {
				found = true;
				break;
			}
		}
		mutex_unlock(&s->tasks_lock);

		if (found)
			continue;

		cls = sl_classify_task(s, task->comm, NULL);

		ti = kzalloc(sizeof(*ti), GFP_ATOMIC);
		if (!ti)
			continue;

		ti->pid = task->pid;
		strscpy(ti->comm, task->comm, TASK_COMM_LEN);
		ti->task_class     = cls;
		ti->original_nice  = task_nice(task);
		ti->boosted_nice   = ti->original_nice;
		ti->assigned_cpu   = task_cpu(task);
		ti->numa_node      = cpu_to_node(ti->assigned_cpu);
		ti->classify_time_ns = ktime_get_ns();

		spin_lock_init(&ti->burst.lock);
		ti->burst.head = 0;
		ti->burst.count = 0;
		ti->burst.burst_active = false;

		mutex_lock(&s->tasks_lock);
		list_add_tail(&ti->list, &s->tracked_tasks);
		mutex_unlock(&s->tasks_lock);

		/* Apply scheduling boost for ML tasks */
		if (cls != SL_CLASS_SYSTEM)
			sl_apply_priority_boost(s, ti);

		atomic64_inc(&s->classifications);
	}
	rcu_read_unlock();
}

/* ---- Garbage collect dead tasks ---------------------------------------- */

static void __maybe_unused sl_gc_tasks(struct sl_scheduler *s)
{
	struct sl_task_info *ti, *tmp;

	mutex_lock(&s->tasks_lock);
	list_for_each_entry_safe(ti, tmp, &s->tracked_tasks, list) {
		if (!sl_task_pid_alive(ti->pid)) {
			list_del(&ti->list);
			kfree(ti);
		}
	}
	mutex_unlock(&s->tasks_lock);
}

/* ---- Module init / exit ------------------------------------------------ */

static int __init sl_sched_init(void)
{
	int ret;

	g_sched = kzalloc(sizeof(*g_sched), GFP_KERNEL);
	if (!g_sched)
		return -ENOMEM;

	INIT_LIST_HEAD(&g_sched->tracked_tasks);
	mutex_init(&g_sched->tasks_lock);
	mutex_init(&g_sched->rules_lock);
	atomic64_set(&g_sched->classifications, 0);
	atomic64_set(&g_sched->boosts_applied, 0);
	atomic64_set(&g_sched->bursts_detected, 0);

	/* Install default policies */
	memcpy(g_sched->policies, default_policies, sizeof(default_policies));

	/* Discover CPU topology */
	ret = sl_topology_discover(&g_sched->topology);
	if (ret)
		pr_warn("straylight-sched: topology discovery incomplete (%d)\n", ret);

	/* Build affinity masks from topology + policy */
	sl_topology_build_affinity(&g_sched->topology, g_sched->policies);

	/* Load default classification rules */
	sl_ml_init_rules(g_sched);

	/* Create /proc/straylight/sched/ */
	proc_mkdir("straylight", NULL);
	g_sched->proc_dir = proc_mkdir("straylight/sched", NULL);
	if (!g_sched->proc_dir) {
		pr_err("straylight-sched: failed to create /proc/straylight/sched\n");
		ret = -ENOMEM;
		goto err;
	}

	g_sched->proc_status = proc_create("status", 0444,
					   g_sched->proc_dir,
					   &sched_status_ops);
	g_sched->proc_tasks = proc_create("tasks", 0444,
					  g_sched->proc_dir,
					  &sched_tasks_ops);
	g_sched->proc_topology = proc_create("topology", 0444,
					     g_sched->proc_dir,
					     &sched_topology_ops);
	g_sched->proc_policy = proc_create("policy", 0644,
					   g_sched->proc_dir,
					   &sched_policy_ops);

	/* Initial task scan */
	sl_scan_tasks(g_sched);

	pr_info("straylight-sched: ML-aware scheduler loaded "
		"(%u CPUs, %u rules)\n",
		g_sched->topology.nr_cpus, g_sched->nr_rules);
	return 0;

err:
	kfree(g_sched);
	g_sched = NULL;
	return ret;
}

static void __exit sl_sched_exit(void)
{
	struct sl_task_info *ti, *tmp;

	if (!g_sched)
		return;

	/* Remove proc entries */
	if (g_sched->proc_dir)
		remove_proc_subtree("straylight/sched", NULL);

	/* Remove priority boosts and free tracked tasks */
	mutex_lock(&g_sched->tasks_lock);
	list_for_each_entry_safe(ti, tmp, &g_sched->tracked_tasks, list) {
		sl_remove_priority_boost(g_sched, ti);
		list_del(&ti->list);
		kfree(ti);
	}
	mutex_unlock(&g_sched->tasks_lock);

	kfree(g_sched);
	g_sched = NULL;

	pr_info("straylight-sched: module unloaded\n");
}

module_init(sl_sched_init);
module_exit(sl_sched_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("StrayLight Systems");
MODULE_DESCRIPTION("StrayLight ML-aware CPU Scheduler");
MODULE_VERSION("1.0.0");
