import { useServiceEvent } from "./useBridge";

export type NetworkType = "ethernet" | "wifi" | "none";

export interface NetworkState {
  connected: boolean;
  type: NetworkType;
  interface: string;
  ssid: string | null;
  ip4: string;
  ip6: string;
  signal_pct: number;
  rx_kbps: number;
  tx_kbps: number;
}

/** Live network state from the network service. */
export function useNetwork() {
  return useServiceEvent<NetworkState>("network", "state");
}
