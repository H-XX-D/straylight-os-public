import { useServiceEvent, useServiceRequest } from "./useBridge";

export type PolicyEffect = "allow" | "deny" | "audit";

export interface PolicyRule {
  id: number;
  name: string;
  subject: string;        // user / group / service pattern
  resource: string;       // filesystem path, socket, capability, etc.
  action: string;         // read, write, exec, mount, …
  effect: PolicyEffect;
  priority: number;
  enabled: boolean;
}

export interface PolicyState {
  mode: "enforce" | "permissive" | "audit";
  rules: PolicyRule[];
  violations_today: number;
}

/** Live policy enforcement state. */
export function usePolicyState() {
  return useServiceEvent<PolicyState>("policy", "state");
}

/** Add, update, or delete a policy rule. */
export function usePolicyAction() {
  return useServiceRequest<
    | { action: "add";    rule: Omit<PolicyRule, "id"> }
    | { action: "update"; rule: PolicyRule }
    | { action: "delete"; id: number }
    | { action: "set_mode"; mode: PolicyState["mode"] },
    { ok: boolean; message?: string }
  >("policy");
}
