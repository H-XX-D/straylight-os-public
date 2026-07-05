import { useServiceEvent, useServiceRequest } from "./useBridge";

export type TaskStatus = "queued" | "running" | "done" | "failed" | "cancelled";

export interface AgentTask {
  id: string;
  command: string;
  args: string[];
  status: TaskStatus;
  submitted_at_ms: number;
  started_at_ms: number | null;
  finished_at_ms: number | null;
  exit_code: number | null;
  stdout: string;
  stderr: string;
}

export interface AgentState {
  queue_depth: number;
  running_count: number;
  tasks: AgentTask[];    // recent tasks (running + last N completed)
  socket_path: string;
}

/** Live agent task queue state. */
export function useAgentState() {
  return useServiceEvent<AgentState>("agent", "state");
}

/** Submit, cancel, or query agent tasks. */
export function useAgentAction() {
  return useServiceRequest<
    | { action: "submit"; command: string; args?: string[] }
    | { action: "cancel"; id: string }
    | { action: "status"; id: string },
    | { ok: boolean; id?: string }
    | AgentTask
  >("agent");
}
