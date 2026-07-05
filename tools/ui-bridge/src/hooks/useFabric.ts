import { useServiceEvent, useServiceRequest } from "./useBridge";

export type FabricDeviceType =
  | "cpu" | "gpu" | "nvme" | "nic" | "usb"
  | "memory" | "pcie_switch" | "pcie_root" | "numa_node" | "cache" | "unknown";

export interface FabricDevice {
  id: string;            // sysfs path fragment
  type: FabricDeviceType;
  label: string;         // human-readable name
  vendor: string;
  bandwidth_gb_s: number;
  latency_ns: number;
  numa_node: number;
}

export interface FabricEdge {
  src: string;
  dst: string;
  bandwidth_gb_s: number;
  latency_ns: number;
  link_type: string;     // "pcie", "numa", "dma", "nvlink", …
}

export interface FabricTopology {
  devices: FabricDevice[];
  edges: FabricEdge[];
  scan_time_ms: number;
}

/** Live hardware fabric topology from the fabric service. */
export function useFabricTopology() {
  return useServiceEvent<FabricTopology>("fabric", "topology");
}

/** Request an optimal path between two devices. */
export function useFabricPath() {
  return useServiceRequest<
    { from: string; to: string; optimize: "latency" | "bandwidth" },
    { path: string[]; total_latency_ns: number; min_bandwidth_gb_s: number }
  >("fabric");
}
