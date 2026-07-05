import { useServiceEvent, useServiceRequest } from "./useBridge";

export type UserRole = "admin" | "user" | "guest" | "service";

export interface UserEntry {
  uid: number;
  username: string;
  display_name: string;
  role: UserRole;
  groups: string[];
  shell: string;
  home: string;
  locked: boolean;
  last_login_ms: number | null;
  session_active: boolean;
}

export interface UsersState {
  users: UserEntry[];
  active_sessions: number;
  current_uid: number;
}

/** Live user list and session state from the users service. */
export function useUsersState() {
  return useServiceEvent<UsersState>("users", "state");
}

/** Create, modify, lock, delete, or switch user sessions. */
export function useUsersAction() {
  return useServiceRequest<
    | { action: "create";   username: string; role?: UserRole; display_name?: string }
    | { action: "modify";   uid: number; changes: Partial<Pick<UserEntry, "display_name" | "role" | "shell">> }
    | { action: "lock";     uid: number }
    | { action: "unlock";   uid: number }
    | { action: "delete";   uid: number }
    | { action: "set_password"; uid: number; password: string },
    { ok: boolean; message?: string }
  >("users");
}
