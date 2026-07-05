import { useServiceEvent, useServiceRequest } from "./useBridge";

export type ReplayEventType =
  | "syscall" | "process_start" | "process_exit" | "signal"
  | "file_open" | "file_write" | "network_connect" | "network_listen"
  | "gpu_alloc" | "gpu_free" | "gpu_submit"
  | "service_start" | "service_stop" | "service_fail"
  | "dmesg" | "oom_kill" | "panic" | "user_login" | "user_logout";

export interface ReplayEvent {
  timestamp_ns: number;
  type: ReplayEventType;
  pid: number;
  uid: number;
  process_name: string;
  detail: string;        // JSON payload, event-specific
}

export interface ReplayStats {
  total_events: number;
  buffer_size_mb: number;
  oldest_event_ns: number;
  newest_event_ns: number;
  recording: boolean;
}

/** Live recorder statistics from the replay service. */
export function useReplayStats() {
  return useServiceEvent<ReplayStats>("replay", "stats");
}

/** Query recorded events or trigger crash analysis. */
export function useReplayQuery() {
  return useServiceRequest<
    | { action: "query";   since_ns?: number; type_filter?: ReplayEventType[]; limit?: number }
    | { action: "analyze"; since_ns?: number }
    | { action: "export";  path: string },
    | { events: ReplayEvent[] }
    | { analysis: string }
    | { ok: boolean }
  >("replay");
}
