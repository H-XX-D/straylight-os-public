import { useServiceEvent } from "./useBridge";

export interface Prediction {
  id: string;
  model: string;
  label: string;
  confidence: number;
  timestamp_ms: number;
  metadata: Record<string, unknown>;
}

/** Latest inference predictions from the predict service. */
export function usePredict() {
  return useServiceEvent<Prediction[]>("predict", "results");
}
