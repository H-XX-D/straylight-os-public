import { useServiceEvent, useServiceRequest } from "./useBridge";

export interface SnapshotEntry {
  id: string;
  label: string;
  created_at_ms: number;
  size_mb: number;
  device: string;         // e.g. "sda", "nvme0n1"
  partition: string;
  incremental: boolean;
  parent_id: string | null;
  verified: boolean;
}

export interface SnapshotState {
  snapshots: SnapshotEntry[];
  in_progress: boolean;
  last_snapshot_ms: number;
  total_size_mb: number;
}

/** Live snapshot list from the snapshot service. */
export function useSnapshotState() {
  return useServiceEvent<SnapshotState>("snapshot", "state");
}

/** Create, delete, restore, or verify a snapshot. */
export function useSnapshotAction() {
  return useServiceRequest<
    | { action: "create";  label?: string; device?: string; incremental?: boolean }
    | { action: "restore"; id: string }
    | { action: "delete";  id: string }
    | { action: "verify";  id: string },
    { ok: boolean; id?: string; message?: string }
  >("snapshot");
}
