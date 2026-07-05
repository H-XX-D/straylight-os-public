import { useServiceEvent, useServiceRequest } from "./useBridge";

/** A single event pulled from a named flux stream. */
export interface FluxEvent {
  stream: string;
  sequence: number;
  timestamp_ms: number;
  payload: unknown;
}

/** Summary of all active named streams. */
export interface FluxStreamInfo {
  name: string;
  capacity: number;
  event_count: number;
  subscriber_count: number;
  last_event_ms: number;
}

export interface FluxState {
  streams: FluxStreamInfo[];
  total_events_emitted: number;
}

/** Live summary of all flux streams. */
export function useFluxState() {
  return useServiceEvent<FluxState>("flux", "state");
}

/** Subscribe to a specific named stream via the bridge. */
export function useFluxStream(stream: string) {
  return useServiceEvent<FluxEvent>(
    "flux",
    (ev): ev is FluxEvent => (ev as FluxEvent).stream === stream
  );
}

/** Publish an event or create/destroy a stream. */
export function useFluxAction() {
  return useServiceRequest<
    | { action: "publish"; stream: string; payload: unknown }
    | { action: "create";  stream: string; capacity?: number }
    | { action: "destroy"; stream: string },
    { ok: boolean }
  >("flux");
}
