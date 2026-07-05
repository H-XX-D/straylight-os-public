import { useServiceEvent, useServiceRequest } from "./useBridge";

export interface VaultEntry {
  key: string;
  namespace: string;
  created_at_ms: number;
  updated_at_ms: number;
  metadata: Record<string, string>;  // tags, labels — no secret values exposed
}

export interface VaultState {
  locked: boolean;
  entry_count: number;
  namespaces: string[];
  last_access_ms: number;
}

/** Live vault status (locked/unlocked, entry count). Does NOT expose secret values. */
export function useVaultState() {
  return useServiceEvent<VaultState>("vault", "state");
}

/** List, read, write, or delete vault entries. Secret values transit encrypted. */
export function useVaultAction() {
  return useServiceRequest<
    | { action: "unlock";    passphrase: string }
    | { action: "lock" }
    | { action: "list";      namespace?: string }
    | { action: "read";      key: string; namespace?: string }
    | { action: "write";     key: string; namespace?: string; value: string; metadata?: Record<string, string> }
    | { action: "delete";    key: string; namespace?: string },
    | { ok: boolean }
    | { entries: VaultEntry[] }
    | { value: string }
  >("vault");
}
