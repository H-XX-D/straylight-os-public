import { useServiceEvent, useServiceRequest } from "./useBridge";

export type DisplayRotation = "normal" | "left" | "right" | "inverted";
export type DisplayScale = 1 | 1.25 | 1.5 | 1.75 | 2;

export interface DisplayOutput {
  name: string;          // e.g. "DP-1", "HDMI-1"
  connected: boolean;
  enabled: boolean;
  primary: boolean;
  width_px: number;
  height_px: number;
  refresh_hz: number;
  x: number;             // position in compositor space
  y: number;
  scale: number;
  rotation: DisplayRotation;
  model: string;
}

export interface DisplayState {
  outputs: DisplayOutput[];
  active_count: number;
}

/** Live list of connected outputs from the display service. */
export function useDisplayState() {
  return useServiceEvent<DisplayState>("display", "state");
}

/** Request a display reconfiguration (position, resolution, scale, rotation). */
export function useDisplayAction() {
  return useServiceRequest<
    {
      output: string;
      width_px?: number;
      height_px?: number;
      refresh_hz?: number;
      scale?: number;
      rotation?: DisplayRotation;
      enabled?: boolean;
      primary?: boolean;
      x?: number;
      y?: number;
    },
    { ok: boolean; message?: string }
  >("display");
}
