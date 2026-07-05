import { useServiceEvent } from "./useBridge";

export interface ServiceRegistryEntry {
  name: string;
  pid: number;
  registered_at_ms: number;
}

export interface BusState {
  services: ServiceRegistryEntry[];
  service_count: number;
  messages_routed: number;
}

/** Live service registry state from the message bus. */
export function useBusState() {
  return useServiceEvent<BusState>("bus", "state");
}
