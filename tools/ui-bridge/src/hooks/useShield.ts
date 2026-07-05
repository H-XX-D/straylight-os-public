import { useServiceEvent, useServiceRequest } from "./useBridge";

export type ThreatLevel = "none" | "low" | "medium" | "high" | "critical";

export interface ShieldEvent {
  id: string;
  timestamp_ms: number;
  threat_level: ThreatLevel;
  category: string;       // "intrusion", "policy_violation", "anomaly", "exploit_attempt", …
  source: string;         // process name or network address
  description: string;
  mitigated: boolean;
  action_taken: string;
}

export interface ShieldState {
  active: boolean;
  threat_level: ThreatLevel;
  events_today: number;
  open_threats: ShieldEvent[];
  blocked_processes: string[];
  blocked_ips: string[];
}

/** Live security / shield state. */
export function useShieldState() {
  return useServiceEvent<ShieldState>("shield", "state");
}

/** Acknowledge a threat event, block/unblock a process, or toggle shield. */
export function useShieldAction() {
  return useServiceRequest<
    | { action: "ack";            event_id: string }
    | { action: "block_process";  name: string }
    | { action: "unblock_process"; name: string }
    | { action: "block_ip";       address: string }
    | { action: "unblock_ip";     address: string }
    | { action: "set_active";     active: boolean },
    { ok: boolean; message?: string }
  >("shield");
}
