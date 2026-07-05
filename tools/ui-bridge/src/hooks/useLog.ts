import { useServiceEvent, useServiceRequest } from "./useBridge";

export type LogLevel = "trace" | "debug" | "info" | "warn" | "error" | "critical";

export interface LogEntry {
  timestamp_ms: number;
  level: LogLevel;
  service: string;
  message: string;
  pid: number;
  fields: Record<string, unknown>;
}

export interface LogState {
  tail: LogEntry[];     // last N entries (bridge sends rolling window)
  total_entries: number;
  services: string[];   // known service names
}

/** Live log tail from the log service. */
export function useLogTail() {
  return useServiceEvent<LogState>("log", "tail");
}

/** Query historical logs or change log levels. */
export function useLogAction() {
  return useServiceRequest<
    | { action: "query";     service?: string; level?: LogLevel; limit?: number; since_ms?: number }
    | { action: "set_level"; service: string; level: LogLevel }
    | { action: "clear";     service?: string },
    | { entries: LogEntry[] }
    | { ok: boolean }
  >("log");
}
