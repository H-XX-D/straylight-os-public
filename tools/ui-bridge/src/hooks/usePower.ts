import { useServiceEvent, useServiceRequest } from "./useBridge";

export type PowerSource = "ac" | "battery" | "unknown";

export interface PowerState {
  source: PowerSource;
  battery_percent: number;
  charging: boolean;
  time_remaining_s: number;
  lid_open: boolean;
}

/** Live power / battery state from the power service. */
export function usePower() {
  return useServiceEvent<PowerState>("power", "state");
}

/** Request a suspend / shutdown / reboot. */
export function usePowerAction() {
  return useServiceRequest<{ action: "suspend" | "shutdown" | "reboot" }, { ok: boolean }>(
    "power",
    "action"
  );
}
