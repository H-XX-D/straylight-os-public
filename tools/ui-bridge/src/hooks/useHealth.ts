import { useServiceEvent } from "./useBridge";

export interface HealthStatus {
  cpu_percent: number;
  ram_percent: number;
  gpu_percent: number;
  gpu_vram_percent: number;
  disk_percent: number;
  net_rx_kbps: number;
  net_tx_kbps: number;
  cpu_temp_c: number;
  uptime_s: number;
}

/** Live system health metrics from the health service (~1 Hz). */
export function useHealth() {
  return useServiceEvent<HealthStatus>("health", "status");
}
