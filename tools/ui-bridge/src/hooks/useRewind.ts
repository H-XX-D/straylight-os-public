import { useServiceEvent, useServiceRequest } from "./useBridge";

export interface CheckpointEntry {
  checkpoint_id: string;
  pid: number;
  process_name: string;
  created_at_ms: number;
  size_bytes: number;
  is_delta: boolean;
  parent_id: string | null;
}

export interface TrackedProcess {
  pid: number;
  name: string;
  tracking_since_ms: number;
  checkpoint_count: number;
  last_checkpoint_ms: number;
  auto_interval_s: number;
}

export interface RewindState {
  tracked: TrackedProcess[];
  checkpoints: CheckpointEntry[];
  store_size_mb: number;
}

/** Live checkpoint / rewind state. */
export function useRewindState() {
  return useServiceEvent<RewindState>("rewind", "state");
}

/** Checkpoint or restore a process. */
export function useRewindAction() {
  return useServiceRequest<
    | { action: "checkpoint";   pid: number }
    | { action: "restore";      checkpoint_id: string }
    | { action: "track";        pid: number; interval_s?: number }
    | { action: "untrack";      pid: number }
    | { action: "delete";       checkpoint_id: string },
    { ok: boolean; checkpoint_id?: string; message?: string }
  >("rewind");
}
