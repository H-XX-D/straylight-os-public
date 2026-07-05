import { useServiceEvent } from "./useBridge";

export type SystemEvent =
  | "Boot" | "Shutdown" | "Suspend" | "Resume"
  | "NetworkUp" | "NetworkDown"
  | "UsbAttach" | "UsbDetach"
  | "LidOpen" | "LidClose"
  | "BatteryLow" | "PowerAC" | "PowerBattery";

export interface HookEntry {
  id: number;
  event: SystemEvent;
  command: string;
  enabled: boolean;
  last_triggered_ms: number | null;
  last_exit_code: number | null;
}

/** Live list of registered system-event hooks. */
export function useSystemEvents() {
  return useServiceEvent<HookEntry[]>("hooks", "list");
}
