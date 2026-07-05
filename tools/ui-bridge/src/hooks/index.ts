/**
 * Per-service typed hooks for Straylight UI Bridge.
 * Each hook wraps useServiceEvent / useServiceRequest with typed payloads
 * matching the C++ service structs.
 */

export { useBridgeStatus, useServiceEvent, useServiceRequest } from "./useBridge";

// ── Notifications ────────────────────────────────────────────────────────────
export type { Notification, NotificationRule } from "./useNotify";
export { useNotifications, useNotifyRule } from "./useNotify";

// ── Health ────────────────────────────────────────────────────────────────────
export type { HealthStatus } from "./useHealth";
export { useHealth } from "./useHealth";

// ── Power ─────────────────────────────────────────────────────────────────────
export type { PowerState, PowerSource } from "./usePower";
export { usePower, usePowerAction } from "./usePower";

// ── Network ───────────────────────────────────────────────────────────────────
export type { NetworkState, NetworkType } from "./useNetwork";
export { useNetwork } from "./useNetwork";

// ── Audio ─────────────────────────────────────────────────────────────────────
export type { AudioState } from "./useAudio";
export { useAudio } from "./useAudio";

// ── Disk ──────────────────────────────────────────────────────────────────────
export type { DiskMount } from "./useDisk";
export { useDisks } from "./useDisk";

// ── Hooks (system-event hooks) ────────────────────────────────────────────────
export type { SystemEvent, HookEntry } from "./useHooks";
export { useSystemEvents } from "./useHooks";

// ── Predict ───────────────────────────────────────────────────────────────────
export type { Prediction } from "./usePredict";
export { usePredict } from "./usePredict";

// ── Display ───────────────────────────────────────────────────────────────────
export type { DisplayOutput, DisplayState, DisplayRotation, DisplayScale } from "./useDisplay";
export { useDisplayState, useDisplayAction } from "./useDisplay";

// ── Input ─────────────────────────────────────────────────────────────────────
export type { InputDevice, InputState, InputDeviceType } from "./useInput";
export { useInputState, useInputConfig } from "./useInput";

// ── Policy ────────────────────────────────────────────────────────────────────
export type { PolicyRule, PolicyState, PolicyEffect } from "./usePolicy";
export { usePolicyState, usePolicyAction } from "./usePolicy";

// ── Flux (named event streams) ────────────────────────────────────────────────
export type { FluxEvent, FluxStreamInfo, FluxState } from "./useFlux";
export { useFluxState, useFluxStream, useFluxAction } from "./useFlux";

// ── Fabric (hardware topology) ────────────────────────────────────────────────
export type { FabricDevice, FabricEdge, FabricTopology, FabricDeviceType } from "./useFabric";
export { useFabricTopology, useFabricPath } from "./useFabric";

// ── Mesh (distributed GPU pool) ───────────────────────────────────────────────
export type { RemoteGpu, MeshAllocation, MeshPoolState, PlacementPolicy } from "./useMesh";
export { useMeshPool, useMeshAllocate } from "./useMesh";

// ── Sandbox ───────────────────────────────────────────────────────────────────
export type { SandboxInfo, SandboxState, SandboxStatus } from "./useSandbox";
export { useSandboxState, useSandboxAction } from "./useSandbox";

// ── Rewind (process checkpointing) ────────────────────────────────────────────
export type { CheckpointEntry, TrackedProcess, RewindState } from "./useRewind";
export { useRewindState, useRewindAction } from "./useRewind";

// ── Replay (event flight recorder) ────────────────────────────────────────────
export type { ReplayEvent, ReplayStats, ReplayEventType } from "./useReplay";
export { useReplayStats, useReplayQuery } from "./useReplay";

// ── Snapshot ──────────────────────────────────────────────────────────────────
export type { SnapshotEntry, SnapshotState } from "./useSnapshot";
export { useSnapshotState, useSnapshotAction } from "./useSnapshot";

// ── Backup ────────────────────────────────────────────────────────────────────
export type { BackupJob, BackupState, BackupStatus } from "./useBackup";
export { useBackupState, useBackupAction } from "./useBackup";

// ── Vault (secrets store) ─────────────────────────────────────────────────────
export type { VaultEntry, VaultState } from "./useVault";
export { useVaultState, useVaultAction } from "./useVault";

// ── Intent (NL → system action) ───────────────────────────────────────────────
export type { IntentResult, IntentContext, IntentState, IntentActionType } from "./useIntent";
export { useIntentState, useIntentSubmit } from "./useIntent";

// ── Cron (task scheduler) ─────────────────────────────────────────────────────
export type { CronTask, CronState, TaskRun, TaskSchedule } from "./useCron";
export { useCronState, useCronAction } from "./useCron";

// ── Update ────────────────────────────────────────────────────────────────────
export type { UpdatePackage, UpdateState, UpdateChannel, UpdateStatus } from "./useUpdate";
export { useUpdateState, useUpdateAction } from "./useUpdate";

// ── Users ─────────────────────────────────────────────────────────────────────
export type { UserEntry, UsersState, UserRole } from "./useUsers";
export { useUsersState, useUsersAction } from "./useUsers";

// ── Log ───────────────────────────────────────────────────────────────────────
export type { LogEntry, LogState, LogLevel } from "./useLog";
export { useLogTail, useLogAction } from "./useLog";

// ── Shield (runtime security) ────────────────────────────────────────────────
export type { ShieldEvent, ShieldState, ThreatLevel } from "./useShield";
export { useShieldState, useShieldAction } from "./useShield";

// ── Ghost (live process migration) ───────────────────────────────────────────
export type { MigrationStatus, GhostState, MigrationState } from "./useGhost";
export { useGhostState, useGhostAction } from "./useGhost";

// ── Alice (AI system monitor) ─────────────────────────────────────────────────
export type { AliceAlert, AliceState, AliceModelInfo, AlertSeverity } from "./useAlice";
export { useAliceState, useAliceAction } from "./useAlice";

// ── Agent (task executor) ─────────────────────────────────────────────────────
export type { AgentTask, AgentState, TaskStatus } from "./useAgent";
export { useAgentState, useAgentAction } from "./useAgent";

// ── Core (orchestrator) ───────────────────────────────────────────────────────
export type { SubsystemEntry, CoreState, SubsystemPriority } from "./useCore";
export { useCoreState, useCoreAction } from "./useCore";

// ── Bus (service registry) ────────────────────────────────────────────────────
export type { ServiceRegistryEntry, BusState } from "./useBus";
export { useBusState } from "./useBus";
