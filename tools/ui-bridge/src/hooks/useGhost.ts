import { useServiceEvent, useServiceRequest } from "./useBridge";

export type MigrationState =
  | "pending" | "freezing" | "capturing" | "streaming"
  | "lazy-active" | "restoring" | "complete" | "failed" | "cancelled";

export interface MigrationStatus {
  migration_id: number;
  source_pid: number;
  target_host: string;
  state: MigrationState;
  total_pages: number;
  pages_transferred: number;
  pages_remaining: number;
  hot_pages: number;
  bandwidth_mbps: number;
  elapsed_ms: number;
  error_message: string | null;
}

export interface GhostState {
  active_migrations: MigrationStatus[];
  completed_today: number;
  failed_today: number;
}

/** Live VM / process migration state from the ghost service. */
export function useGhostState() {
  return useServiceEvent<GhostState>("ghost", "state");
}

/** Start, cancel, or query a live migration. */
export function useGhostAction() {
  return useServiceRequest<
    | { action: "migrate"; pid: number; target_host: string; lazy?: boolean }
    | { action: "cancel";  migration_id: number }
    | { action: "status";  migration_id: number },
    { ok: boolean; migration_id?: number; status?: MigrationStatus; message?: string }
  >("ghost");
}
