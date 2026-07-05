import { useServiceEvent, useServiceRequest } from "./useBridge";

export type SubsystemPriority = "critical" | "normal" | "optional";
export type HealthStatus = "healthy" | "degraded" | "failed" | "unknown";

export interface SubsystemEntry {
  name: string;
  priority: SubsystemPriority;
  last_health: HealthStatus;
  restart_count: number;
}

export interface CoreState {
  ready: boolean;
  subsystems: SubsystemEntry[];
  critical_count: number;
  healthy_count: number;
  failed_count: number;
  uptime_s: number;
}

/** Live StrayLight core orchestrator state. */
export function useCoreState() {
  return useServiceEvent<CoreState>("core", "state");
}

/** Restart a subsystem or query readiness. */
export function useCoreAction() {
  return useServiceRequest<
    | { action: "restart"; subsystem: string }
    | { action: "readiness" },
    { ok: boolean; ready?: boolean; message?: string }
  >("core");
}
