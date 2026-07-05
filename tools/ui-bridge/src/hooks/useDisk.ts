import { useServiceEvent } from "./useBridge";

export interface DiskMount {
  device: string;
  mountpoint: string;
  fstype: string;
  total_bytes: number;
  used_bytes: number;
  free_bytes: number;
  removable: boolean;
}

/** Live list of mounted disks from the disk service. */
export function useDisks() {
  return useServiceEvent<DiskMount[]>("disk", "mounts");
}
