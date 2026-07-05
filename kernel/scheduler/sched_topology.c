// SPDX-License-Identifier: GPL-2.0
/*
 * StrayLight OS — Scheduler Topology Discovery
 * Copyright (C) 2026 StrayLight Systems
 *
 * NUMA node discovery, P-core / E-core detection, affinity matrix.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/topology.h>
#include <linux/cpufreq.h>
#include <linux/slab.h>
#include <linux/numa.h>
#include <linux/nodemask.h>
#include <linux/arch_topology.h>

#include "sched.h"

/*
 * Heuristic for P-core vs E-core on Intel hybrid (Alder Lake+).
 *
 * CPUID leaf 0x1A (Native Model ID) returns the core type:
 *   EAX bits 31:24 = 0x40 for Atom (E-core)
 *   EAX bits 31:24 = 0x20 for Core (P-core)
 *
 * Fallback: use max frequency — P-cores have higher max freq.
 */

#define INTEL_CORE_TYPE_ATOM    0x20    /* E-core (Atom) */
#define INTEL_CORE_TYPE_CORE    0x40    /* P-core (Core) */

static enum sl_core_type detect_core_type(unsigned int cpu)
{
#ifdef CONFIG_X86_64
	u32 eax, ebx, ecx, edx;
	u32 core_type_id;

	/*
	 * Execute CPUID on the target CPU.  In a module context we
	 * can read it locally — for remote CPUs we use smp_call_function_single
	 * or rely on cached topology data.
	 *
	 * Check CPUID leaf 0x1A availability first.
	 */
	if (cpu == smp_processor_id()) {
		eax = 0;
		cpuid_count(0, 0, &eax, &ebx, &ecx, &edx);

		if (eax >= 0x1A) {
			cpuid_count(0x1A, 0, &eax, &ebx, &ecx, &edx);
			core_type_id = (eax >> 24) & 0xFF;

			if (core_type_id == INTEL_CORE_TYPE_CORE)
				return SL_CORE_P_TYPE;
			if (core_type_id == INTEL_CORE_TYPE_ATOM)
				return SL_CORE_E_TYPE;
		}
	}
#endif

	/*
	 * Fallback: Use the CPU's capacity as a proxy.
	 * arch_scale_cpu_capacity() returns a value 0-1024 where
	 * higher = more capable.  On hybrid systems, P-cores report
	 * higher capacity than E-cores.
	 */
	{
		unsigned long cap = arch_scale_cpu_capacity(cpu);

		/*
		 * Threshold: if capacity > 768 (75% of max), treat as P-core.
		 * This heuristic works for typical Intel Alder Lake / Raptor Lake
		 * configurations where E-cores report ~60% of P-core capacity.
		 */
		if (cap > 768)
			return SL_CORE_P_TYPE;
		else if (cap > 0 && cap <= 768)
			return SL_CORE_E_TYPE;
	}

	return SL_CORE_UNKNOWN;
}

static unsigned int get_cpu_freq_khz(unsigned int cpu, bool max)
{
	struct cpufreq_policy *pol;
	unsigned int freq = 0;

	pol = cpufreq_cpu_get(cpu);
	if (pol) {
		freq = max ? pol->cpuinfo.max_freq : pol->cpuinfo.min_freq;
		cpufreq_cpu_put(pol);
	}

	return freq;
}

/* ---- Topology discovery ------------------------------------------------ */

int sl_topology_discover(struct sl_topology *topo)
{
	unsigned int cpu;
	int node;

	memset(topo, 0, sizeof(*topo));

	/* Init CPU masks */
	cpumask_clear(&topo->p_core_mask);
	cpumask_clear(&topo->e_core_mask);
	for (node = 0; node < SL_MAX_NUMA_NODES; node++)
		cpumask_clear(&topo->numa_cpumask[node]);

	/* Enumerate online CPUs */
	for_each_online_cpu(cpu) {
		struct sl_cpu_info *ci;

		if (topo->nr_cpus >= SL_MAX_CPUS) {
			pr_warn("straylight-sched: CPU limit reached (%u)\n",
				SL_MAX_CPUS);
			break;
		}

		ci = &topo->cpus[topo->nr_cpus];
		ci->cpu_id   = cpu;
		ci->online   = true;
		ci->numa_node = cpu_to_node(cpu);

		if (ci->numa_node < 0)
			ci->numa_node = 0;
		if (ci->numa_node >= SL_MAX_NUMA_NODES)
			ci->numa_node = 0;

		/* Core type detection */
		ci->core_type = detect_core_type(cpu);

		/* Frequency info */
		ci->base_freq_khz = get_cpu_freq_khz(cpu, false);
		ci->max_freq_khz  = get_cpu_freq_khz(cpu, true);

		/*
		 * If cpufreq is not available, try a frequency heuristic
		 * from topology data.
		 */
		if (ci->max_freq_khz == 0) {
			/*
			 * Use a default estimate — P-cores ~3GHz, E-cores ~2GHz.
			 * This is refined once cpufreq becomes available.
			 */
			switch (ci->core_type) {
			case SL_CORE_P_TYPE:
				ci->base_freq_khz = 2400000;
				ci->max_freq_khz  = 5000000;
				break;
			case SL_CORE_E_TYPE:
				ci->base_freq_khz = 1800000;
				ci->max_freq_khz  = 3800000;
				break;
			default:
				ci->base_freq_khz = 2000000;
				ci->max_freq_khz  = 3500000;
				break;
			}
		}

		/* Update masks */
		if (ci->core_type == SL_CORE_P_TYPE) {
			cpumask_set_cpu(cpu, &topo->p_core_mask);
			topo->nr_p_cores++;
		} else if (ci->core_type == SL_CORE_E_TYPE) {
			cpumask_set_cpu(cpu, &topo->e_core_mask);
			topo->nr_e_cores++;
		} else {
			/* Unknown treated as P-core for scheduling purposes */
			cpumask_set_cpu(cpu, &topo->p_core_mask);
			topo->nr_p_cores++;
		}

		cpumask_set_cpu(cpu, &topo->numa_cpumask[ci->numa_node]);

		topo->nr_cpus++;
	}

	/* Count NUMA nodes actually in use */
	topo->nr_numa_nodes = 0;
	for (node = 0; node < SL_MAX_NUMA_NODES; node++) {
		if (!cpumask_empty(&topo->numa_cpumask[node]))
			topo->nr_numa_nodes++;
	}

	pr_info("straylight-sched: topology: %u CPUs, %u P-cores, "
		"%u E-cores, %u NUMA nodes\n",
		topo->nr_cpus, topo->nr_p_cores,
		topo->nr_e_cores, topo->nr_numa_nodes);

	return 0;
}

/* ---- Build per-class affinity masks ------------------------------------ */

void sl_topology_build_affinity(struct sl_topology *topo,
				struct sl_class_policy *policies)
{
	int i;

	for (i = 0; i < SL_CLASS_NR; i++) {
		struct sl_class_policy *pol = &policies[i];
		struct cpumask *mask = &topo->affinity[i];

		cpumask_clear(mask);

		if (pol->prefer_p_cores && !cpumask_empty(&topo->p_core_mask)) {
			/*
			 * P-core preference: start with P-cores only.
			 * If no P-cores are available (unlikely), fall back
			 * to all online CPUs.
			 */
			cpumask_copy(mask, &topo->p_core_mask);
		} else {
			/* Use all online CPUs */
			cpumask_copy(mask, cpu_online_mask);
		}

		/* Ensure at least one CPU is in the mask */
		if (cpumask_empty(mask))
			cpumask_copy(mask, cpu_online_mask);
	}

	pr_debug("straylight-sched: affinity masks rebuilt\n");
}

/* ---- CPU selection for task placement ---------------------------------- */

int sl_topology_select_cpu(struct sl_topology *topo,
			   enum sl_task_class task_class,
			   int current_cpu)
{
	struct cpumask *affinity;
	int candidate;

	if (task_class >= SL_CLASS_NR)
		return current_cpu;

	affinity = &topo->affinity[task_class];

	/* If current CPU is already in the preferred affinity, keep it */
	if (cpumask_test_cpu(current_cpu, affinity))
		return current_cpu;

	/*
	 * Try to find a CPU on the same NUMA node first to minimize
	 * memory latency.
	 */
	{
		int node = cpu_to_node(current_cpu);
		struct cpumask tmp;

		if (node >= 0 && node < SL_MAX_NUMA_NODES) {
			cpumask_and(&tmp, affinity,
				    &topo->numa_cpumask[node]);
			if (!cpumask_empty(&tmp)) {
				candidate = cpumask_first(&tmp);
				if (candidate < nr_cpu_ids)
					return candidate;
			}
		}
	}

	/* Fall back to any CPU in the affinity mask */
	candidate = cpumask_first(affinity);
	if (candidate < nr_cpu_ids)
		return candidate;

	/* Last resort: stay on current CPU */
	return current_cpu;
}
