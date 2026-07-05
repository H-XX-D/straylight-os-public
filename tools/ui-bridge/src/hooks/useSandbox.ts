import { useServiceEvent, useServiceRequest } from "./useBridge";

export type SandboxStatus = "running" | "stopped" | "crashed" | "unknown";

export interface SandboxInfo {
  id: string;
  name: string;
  pid: number;
  status: SandboxStatus;
  profile: string;       // seccomp/bpf profile name
  cpu_pct: number;
  mem_mb: number;
  net_isolated: boolean;
  fs_overlay: boolean;
  started_at_ms: number;
}

export interface SandboxState {
  sandboxes: SandboxInfo[];
  active_count: number;
}

/** Live sandbox status from the sandbox service. */
export function useSandboxState() {
  return useServiceEvent<SandboxState>("sandbox", "state");
}

/** Start, stop, or kill a sandbox. */
export function useSandboxAction() {
  return useServiceRequest<
    | { action: "start";  name: string; command: string; profile?: string; net_isolated?: boolean }
    | { action: "stop";   id: string }
    | { action: "kill";   id: string }
    | { action: "attach"; id: string },
    { ok: boolean; id?: string; message?: string }
  >("sandbox");
}
