import { useServiceEvent, useServiceRequest } from "./useBridge";

export type PlacementPolicy = "best_fit" | "least_loaded" | "local_first" | "round_robin" | "pinned";

export interface RemoteGpu {
  host: string;
  gpu_index: number;
  name: string;
  vendor: string;
  vram_total_mb: number;
  vram_available_mb: number;
  vram_pct: number;
  temperature_c: number;
  utilization_pct: number;
  latency_ms: number;
  is_local: boolean;
  is_available: boolean;
}

export interface MeshAllocation {
  handle: number;
  host: string;
  gpu_index: number;
  size_bytes: number;
  is_local: boolean;
}

export interface MeshPoolState {
  gpus: RemoteGpu[];
  allocations: MeshAllocation[];
  total_vram_mb: number;
  available_vram_mb: number;
  node_count: number;
}

/** Live GPU pool state from the mesh service (~5 s refresh). */
export function useMeshPool() {
  return useServiceEvent<MeshPoolState>("mesh", "pool_state");
}

/** Allocate GPU memory across the mesh. */
export function useMeshAllocate() {
  return useServiceRequest<
    { bytes: number; policy: PlacementPolicy },
    MeshAllocation
  >("mesh");
}
