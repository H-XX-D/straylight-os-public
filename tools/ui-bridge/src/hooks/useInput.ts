import { useServiceEvent, useServiceRequest } from "./useBridge";

export type InputDeviceType = "keyboard" | "pointer" | "tablet" | "touchscreen" | "gamepad";

export interface InputDevice {
  id: number;
  name: string;
  type: InputDeviceType;
  product_id: string;       // "vendor:product"
  seat: string;
  enabled: boolean;
  has_libinput: boolean;
}

export interface InputState {
  devices: InputDevice[];
  keyboard_layout: string;   // e.g. "us", "de"
  keyboard_variant: string;
  pointer_acceleration: number; // -1.0 .. 1.0
  natural_scroll: boolean;
  tap_to_click: boolean;
}

/** Live input device state from the input service. */
export function useInputState() {
  return useServiceEvent<InputState>("input", "state");
}

/** Request an input setting change. */
export function useInputConfig() {
  return useServiceRequest<
    {
      keyboard_layout?: string;
      keyboard_variant?: string;
      pointer_acceleration?: number;
      natural_scroll?: boolean;
      tap_to_click?: boolean;
      device_id?: number;
      enabled?: boolean;
    },
    { ok: boolean }
  >("input");
}
