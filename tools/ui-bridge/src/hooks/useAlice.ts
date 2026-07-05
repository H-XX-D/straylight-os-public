import { useServiceEvent, useServiceRequest } from "./useBridge";

export type AlertSeverity = "info" | "warning" | "critical";

export interface AliceAlert {
  id: string;
  timestamp_ms: number;
  severity: AlertSeverity;
  subsystem: string;    // "thermal", "memory", "gpu", "disk", "network", "logs"
  summary: string;
  detail: string;
  acknowledged: boolean;
}

export interface AliceModelInfo {
  loaded: boolean;
  model_path: string;
  context_size: number;
  gpu_offload: boolean;
  last_inference_ms: number;
}

export interface AliceState {
  model: AliceModelInfo;
  alerts: AliceAlert[];
  open_alert_count: number;
  last_cycle_ms: number;
}

/** Live Alice AI monitor state: model status + open alerts. */
export function useAliceState() {
  return useServiceEvent<AliceState>("alice", "state");
}

/** Acknowledge an alert or submit a natural-language query to Alice. */
export function useAliceAction() {
  return useServiceRequest<
    | { action: "ack";   alert_id: string }
    | { action: "query"; prompt: string },
    | { ok: boolean }
    | { response: string; inference_ms: number }
  >("alice");
}
