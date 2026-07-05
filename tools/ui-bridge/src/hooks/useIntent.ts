import { useServiceEvent, useServiceRequest } from "./useBridge";

export type IntentActionType =
  | "system_command" | "pipeline" | "compositor_action"
  | "vpu_action" | "service_control" | "file_operation" | "unknown";

export interface IntentContext {
  current_app: string;
  workspace: string;
  focused_window: string;
  selected_file: string;
  extra: Record<string, string>;
}

export interface IntentResult {
  action_type: IntentActionType;
  commands: string[];
  description: string;
  confidence: number;
  requires_confirmation: boolean;
  executed: boolean;
  error: string | null;
}

export interface IntentHistoryEntry {
  id: string;
  natural_text: string;
  result: IntentResult;
  timestamp_ms: number;
}

export interface IntentState {
  last_intent: IntentHistoryEntry | null;
  pattern_count: number;
  action_count: number;
}

/** Latest resolved intent from the intent service. */
export function useIntentState() {
  return useServiceEvent<IntentState>("intent", "state");
}

/** Submit a natural-language intent for resolution and execution. */
export function useIntentSubmit() {
  return useServiceRequest<
    { text: string; context?: Partial<IntentContext>; dry_run?: boolean },
    IntentResult
  >("intent");
}
