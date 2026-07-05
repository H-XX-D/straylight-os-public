import { useServiceEvent, useServiceRequest } from "./useBridge";

export type UpdateChannel = "stable" | "testing" | "edge";
export type UpdateStatus = "idle" | "checking" | "downloading" | "installing" | "reboot_required" | "failed";

export interface UpdatePackage {
  name: string;
  current_version: string;
  available_version: string;
  size_bytes: number;
  critical: boolean;
  description: string;
}

export interface UpdateState {
  status: UpdateStatus;
  channel: UpdateChannel;
  last_check_ms: number;
  pending_count: number;
  pending: UpdatePackage[];
  progress_pct: number;
  current_package: string | null;
  last_error: string | null;
}

/** Live system update state from the update service. */
export function useUpdateState() {
  return useServiceEvent<UpdateState>("update", "state");
}

/** Check for updates, start update, or change channel. */
export function useUpdateAction() {
  return useServiceRequest<
    | { action: "check" }
    | { action: "install"; packages?: string[] }
    | { action: "set_channel"; channel: UpdateChannel }
    | { action: "schedule_reboot"; at_ms?: number },
    { ok: boolean; message?: string }
  >("update");
}
