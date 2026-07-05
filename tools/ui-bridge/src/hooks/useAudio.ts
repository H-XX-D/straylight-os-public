import { useServiceEvent, useServiceRequest } from "./useBridge";

export interface AudioState {
  volume_pct: number;
  muted: boolean;
  default_sink: string;
  default_source: string;
}

/** Live audio state from the audio service. */
export function useAudio() {
  return useServiceEvent<AudioState>("audio", "state");
}

/** Set volume (0–100) or mute state. */
export function useAudioControl() {
  return useServiceRequest<
    { volume?: number; muted?: boolean },
    { ok: boolean }
  >("audio", "set");
}
