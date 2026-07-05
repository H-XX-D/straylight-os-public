import { useServiceEvent, useServiceRequest } from "./useBridge";

export type BackupStatus = "idle" | "running" | "paused" | "failed" | "completed";

export interface BackupJob {
  id: string;
  label: string;
  source: string;
  destination: string;
  schedule: string;       // "every 1h", "daily 03:00", etc.
  status: BackupStatus;
  last_run_ms: number;
  next_run_ms: number;
  files_total: number;
  files_done: number;
  bytes_total: number;
  bytes_done: number;
  last_error: string | null;
}

export interface BackupState {
  jobs: BackupJob[];
  active_count: number;
  total_stored_mb: number;
}

/** Live backup job state from the backup service. */
export function useBackupState() {
  return useServiceEvent<BackupState>("backup", "state");
}

/** Create, run, pause, resume, or delete a backup job. */
export function useBackupAction() {
  return useServiceRequest<
    | { action: "create"; label: string; source: string; destination: string; schedule?: string }
    | { action: "run";    id: string }
    | { action: "pause";  id: string }
    | { action: "resume"; id: string }
    | { action: "delete"; id: string },
    { ok: boolean; id?: string; message?: string }
  >("backup");
}
