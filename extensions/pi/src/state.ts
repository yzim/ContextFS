/**
 * pi-agentvfs-rewind — Shared state
 *
 * Mutable state shared between index.ts, commands.ts, and ui.ts.
 */

export interface CheckpointData {
  /** Unique checkpoint ID (used internally) */
  id: string;
  /** Label passed to agentvfs-ctl */
  label: string;
  /** Commit hash returned by agentvfs */
  commitHash: string;
  /** Session this checkpoint belongs to */
  sessionId: string;
  /** Turn index when checkpoint was created */
  turnIndex: number;
  /** What triggered this checkpoint */
  trigger: "resume" | "pre-tool" | "tool" | "before-restore" | "before-tree-restore" | "manual";
  /** Human-readable description */
  description?: string;
  /** Epoch ms when created */
  timestamp: number;
}

export interface TreeRestoreData {
  targetId: string;
  checkpointId: string;
  commitHash: string;
  timestamp: number;
  mode: "entry" | "timestamp";
}

export interface RewindState {
  /** Is agentvfs reachable via socket? */
  agentvfsAvailable: boolean;
  /** Path to agentvfs control socket */
  socketPath: string | null;
  /** agentvfs FUSE mount containing the Pi cwd */
  mountPoint: string | null;
  /** Current session ID (UUID) */
  sessionId: string | null;
  /** Workspace key used for persisted checkpoint metadata */
  workspaceRoot: string | null;
  /** In-memory checkpoint cache: checkpoint ID → data */
  checkpoints: Map<string, CheckpointData>;
  /** Checkpoint taken at session start (fallback for restore) */
  resumeCheckpoint: CheckpointData | null;
  /** Stack of before-restore checkpoints for undo */
  redoStack: CheckpointData[];
  /** Whether /tree should restore the workspace before Pi switches leaf. */
  treeIntegrationEnabled: boolean;
  /** Last successful /tree-linked workspace restore. */
  lastTreeRestore: TreeRestoreData | null;
  /** True if checkpoint creation failed (stop retrying) */
  failed: boolean;
  /** Promise of in-flight checkpoint (avoid races) */
  pending: Promise<void> | null;
  /** Current turn index (updated by turn_start) */
  currentTurnIndex: number;
  /** Current user prompt (updated by before_agent_start) */
  currentPrompt: string;
  /** Pending tool info captured from tool_call (before execution ends) */
  pendingToolInfo: Map<string, string>;
  /** Tool descriptions accumulated during the current turn */
  turnToolDescriptions: string[];
  /** Whether the current turn had any mutating tool calls */
  turnHadMutations: boolean;
}

function defaultTreeIntegrationEnabled(): boolean {
  const raw =
    process.env.PI_AGENTVFS_TREE_INTEGRATION ??
    process.env.PI_AGENTVFS_TREE ??
    "restore";
  return raw !== "off" && raw !== "0" && raw !== "false";
}

export function createInitialState(): RewindState {
  return {
    agentvfsAvailable: false,
    socketPath: null,
    mountPoint: null,
    sessionId: null,
    workspaceRoot: null,
    checkpoints: new Map(),
    resumeCheckpoint: null,
    redoStack: [],
    treeIntegrationEnabled: defaultTreeIntegrationEnabled(),
    lastTreeRestore: null,
    failed: false,
    pending: null,
    currentTurnIndex: 0,
    currentPrompt: "",
    pendingToolInfo: new Map(),
    turnToolDescriptions: [],
    turnHadMutations: false,
  };
}

export function resetState(state: RewindState): void {
  const treeIntegrationEnabled = state.treeIntegrationEnabled;
  state.agentvfsAvailable = false;
  state.socketPath = null;
  state.mountPoint = null;
  state.sessionId = null;
  state.workspaceRoot = null;
  state.checkpoints.clear();
  state.resumeCheckpoint = null;
  state.redoStack = [];
  state.treeIntegrationEnabled = treeIntegrationEnabled;
  state.lastTreeRestore = null;
  state.failed = false;
  state.pending = null;
  state.currentTurnIndex = 0;
  state.currentPrompt = "";
  state.pendingToolInfo.clear();
  state.turnToolDescriptions = [];
  state.turnHadMutations = false;
}
