import { useServiceEvent, useServiceRequest } from "./useBridge";

export interface TaskRun {
  task_name: string;
  started_at: string;   // ISO 8601
  finished_at: string;
  exit_code: number;
  duration_s: number;
  success: boolean;
}

export interface TaskSchedule {
  spec: string;                 // "every 1h", "daily 03:00"
  interval_seconds: number;
  next_run_ms: number;
  missed_policy: "skip" | "run";
}

export interface CronTask {
  name: string;
  command: string;
  schedule: TaskSchedule;
  enabled: boolean;
  running: boolean;
  last_run_at: string;
  last_run_success: boolean;
  consecutive_failures: number;
  max_retries: number;
  depends_on: string[];
}

export interface CronState {
  tasks: CronTask[];
  running_count: number;
  next_task: string | null;
  next_run_ms: number | null;
}

/** Live cron task state from the cron service. */
export function useCronState() {
  return useServiceEvent<CronState>("cron", "state");
}

/** Add, edit, enable, disable, run, or delete a cron task. */
export function useCronAction() {
  return useServiceRequest<
    | { action: "add";     task: Omit<CronTask, "running" | "last_run_at" | "last_run_success" | "consecutive_failures"> }
    | { action: "update";  name: string; changes: Partial<CronTask> }
    | { action: "enable";  name: string }
    | { action: "disable"; name: string }
    | { action: "run";     name: string }
    | { action: "delete";  name: string },
    { ok: boolean; message?: string }
  >("cron");
}
