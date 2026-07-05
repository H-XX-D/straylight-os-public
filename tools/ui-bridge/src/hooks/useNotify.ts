import { useServiceEvent } from "./useBridge";

export type Urgency = "low" | "normal" | "critical";

export interface Notification {
  id: number;
  app_name: string;
  summary: string;
  body: string;
  icon: string;
  urgency: Urgency;
  timeout_ms: number;
  actions: string[];
  timestamp: number;
}

export interface NotificationRule {
  id: number;
  app_pattern: string;
  urgency_min: Urgency;
  suppress: boolean;
  sound: string;
}

/** Subscribe to live notification events from the notify service. */
export function useNotifications() {
  return useServiceEvent<Notification[]>("notify", "notifications");
}

/** Subscribe to active notification rules. */
export function useNotifyRule() {
  return useServiceEvent<NotificationRule[]>("notify", "rules");
}
