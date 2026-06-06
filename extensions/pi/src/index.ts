/**
 * pi-agentvfs-rewind — Extension entry point
 *
 * Automatic checkpoint/rewind for the Pi coding agent backed by ContextFS.
 * Communicates with a running agentvfs daemon via its UNIX control socket.
 *
 * Checkpoint strategy:
 *   - 1 resume checkpoint on session start
 *   - 1 checkpoint at turn_end (after ALL tools in a response finish)
 *   - Label: user prompt + list of mutating tools that ran
 *   - No per-tool or per-turn-start checkpoints (noisy, redundant)
 *
 * Usage:
 *   pi -e ./src/index.ts
 *   pi install github.com/thustorage/ContextFS/extensions/pi
 */

import type { ExtensionAPI } from "@earendil-works/pi-coding-agent";
import type { CheckpointData } from "./state.js";
import {
  findAgentvfsSocket,
  createCheckpoint,
  findAgentvfsMountForPath,
  getStatus,
  loadCheckpointMetadata,
  MUTATING_TOOLS,
  saveCheckpointMetadata,
  truncate,
  sanitizeForLabel,
} from "./core.js";
import { createInitialState, resetState } from "./state.js";
import { updateStatus, clearStatus } from "./ui.js";
import {
  appendCheckpointEntry,
  registerCommands,
  handleForkRestore,
  handleTreeRestore,
  handleTreeComplete,
} from "./commands.js";

type CheckpointTrigger = CheckpointData["trigger"];

/** Extract a human-readable description from a tool_call event */
function describeToolCall(toolName: string, input: any): string {
  if (!input) return toolName;
  switch (toolName) {
    case "write":
      return `write → ${input.path || "?"}`;
    case "edit":
      return `edit → ${input.path || "?"}`;
    case "bash":
      return `bash: ${truncate(String(input.command || ""), 50)}`;
    default:
      return toolName;
  }
}

export default function (pi: ExtensionAPI) {
  const state = createInitialState();

  // Register /rewind compatibility command and /agentvfs control surface.
  registerCommands(pi, state);

  // ========================================================================
  // Session lifecycle
  // ========================================================================

  async function ensureAgentvfs(ctx: any): Promise<boolean> {
    const cwd = String(ctx.cwd || process.cwd());
    const mount = await findAgentvfsMountForPath(cwd);
    if (!mount) {
      state.agentvfsAvailable = false;
      state.socketPath = null;
      state.mountPoint = null;
      if (ctx.hasUI) {
        clearStatus(ctx);
        ctx.ui.notify(
          "agentvfs rewind disabled: Pi cwd is not inside an agentvfs mount. Start Pi from the mountpoint, e.g. cd /tmp/agentvfs-mnt",
          "warning"
        );
      }
      return false;
    }

    if (
      state.agentvfsAvailable &&
      state.socketPath &&
      state.mountPoint === mount.mountPoint
    ) {
      return true;
    }

    const sock = await findAgentvfsSocket();
    if (!sock) {
      if (ctx.hasUI) clearStatus(ctx);
      return false;
    }

    const status = await getStatus(sock);
    if (!status) {
      if (ctx.hasUI) clearStatus(ctx);
      return false;
    }

    state.socketPath = sock;
    state.mountPoint = mount.mountPoint;
    state.agentvfsAvailable = true;
    if (!state.sessionId) state.sessionId = ctx.sessionManager.getSessionId();
    state.workspaceRoot = mount.mountPoint;
    return true;
  }

  async function recordCheckpoint(
    ctx: any,
    trigger: CheckpointTrigger,
    labelPrefix: string,
    description?: string
  ): Promise<void> {
    if (state.failed) return;
    if (!(await ensureAgentvfs(ctx))) return;
    if (!state.socketPath || !state.sessionId) return;

    if (state.pending) await state.pending;

    state.pending = (async () => {
      const ts = Date.now();
      const id = `${trigger}-${state.sessionId}-${state.currentTurnIndex}-${ts}`;
      const label = sanitizeForLabel(
        `${state.currentTurnIndex}-${labelPrefix}`.slice(0, 64)
      );
      const result = await createCheckpoint({
        socket: state.socketPath!,
        id,
        label,
        sessionId: state.sessionId!,
        turnIndex: state.currentTurnIndex,
        trigger,
        description,
      });
      const cp: CheckpointData = {
        id,
        label,
        commitHash: result.commitHash,
        sessionId: state.sessionId!,
        turnIndex: state.currentTurnIndex,
        trigger,
        description,
        timestamp: ts,
      };
      state.checkpoints.set(cp.id, cp);
      appendCheckpointEntry(pi, cp);
      await persistCheckpointMetadata();
      if (ctx.hasUI) updateStatus(state, ctx);
    })();

    try {
      await state.pending;
    } catch (err) {
      state.pending = null;
      if (ctx.hasUI) {
        ctx.ui.notify(
          `agentvfs checkpoint failed: ${err instanceof Error ? err.message : String(err)}`,
          "warning"
        );
      }
    } finally {
      if (state.pending) state.pending = null;
    }
  }

  async function persistCheckpointMetadata(): Promise<void> {
    if (!state.workspaceRoot || !state.sessionId) return;
    try {
      await saveCheckpointMetadata(state.workspaceRoot, state.sessionId, {
        checkpoints: [...state.checkpoints.values()],
        resumeCheckpoint: state.resumeCheckpoint,
        redoStack: state.redoStack,
        lastTreeRestore: state.lastTreeRestore,
      });
    } catch {
      // Metadata persistence is best-effort; agentvfs owns the actual data.
    }
  }

  async function initSession(ctx: any): Promise<void> {
    resetState(state);
    state.sessionId = ctx.sessionManager.getSessionId();
    const cwd = String(ctx.cwd || process.cwd());

    if (!(await ensureAgentvfs(ctx))) return;
    const sessionId = state.sessionId;
    const workspaceRoot = state.workspaceRoot;
    if (!sessionId || !workspaceRoot) return;

    try {
      const persisted = await loadCheckpointMetadata(workspaceRoot, sessionId);
      const legacy: Awaited<ReturnType<typeof loadCheckpointMetadata>> =
        cwd !== workspaceRoot
        ? await loadCheckpointMetadata(cwd, sessionId)
        : { checkpoints: [], resumeCheckpoint: null, redoStack: [] };

      state.resumeCheckpoint =
        persisted.resumeCheckpoint ?? legacy.resumeCheckpoint;
      state.redoStack = [...legacy.redoStack, ...persisted.redoStack];
      state.lastTreeRestore =
        persisted.lastTreeRestore ?? legacy.lastTreeRestore ?? null;
      for (const cp of [...legacy.checkpoints, ...persisted.checkpoints]) {
        state.checkpoints.set(cp.id, cp);
      }
    } catch {
      // We'll still create a fresh session-start checkpoint below.
    }

    // Create resume checkpoint (snapshot of current state on session start)
    try {
      const ts = Date.now();
      const id = `resume-${state.sessionId}-${ts}`;
      const result = await createCheckpoint({
        socket: state.socketPath!,
        id,
        label: "session-start",
        sessionId: state.sessionId!,
        turnIndex: 0,
        trigger: "resume",
      });
      const cp = {
        id,
        label: "session-start",
        commitHash: result.commitHash,
        sessionId: state.sessionId!,
        turnIndex: 0,
        trigger: "resume" as const,
        timestamp: ts,
      };
      state.resumeCheckpoint = cp;
      state.checkpoints.set(cp.id, cp);
      appendCheckpointEntry(pi, cp);
      await persistCheckpointMetadata();
    } catch {
      // Resume checkpoint is optional
    }

    if (ctx.hasUI) updateStatus(state, ctx);
  }

  pi.on("session_start", async (event, ctx) => {
    if (event.reason === "fork") {
      // Fork: just update session ID for new checkpoint tagging
      if (!state.agentvfsAvailable) return;
      state.sessionId = ctx.sessionManager.getSessionId();
      state.workspaceRoot = state.mountPoint;
      return;
    }
    // startup, reload, new, resume: full re-initialization
    await initSession(ctx);
  });

  // ========================================================================
  // Capture user prompt for checkpoint labels
  // ========================================================================

  pi.on("before_agent_start", async (event, _ctx) => {
    state.currentPrompt = truncate(String(event.prompt || ""), 60);
    // Reset tool list for this new turn
    state.turnToolDescriptions = [];
    state.turnHadMutations = false;
  });

  // ========================================================================
  // Track turn index
  // ========================================================================

  pi.on("turn_start", async (event, _ctx) => {
    state.currentTurnIndex = event.turnIndex;
  });

  // ========================================================================
  // Capture tool args for checkpoint labels
  // ========================================================================

  pi.on("tool_call", async (event, ctx) => {
    if (MUTATING_TOOLS.has(event.toolName)) {
      const desc = describeToolCall(event.toolName, event.input);
      state.pendingToolInfo.set(event.toolCallId, desc);
      await recordCheckpoint(ctx, "pre-tool", `before-${desc}`, `Before ${desc}`);
    }
  });

  // User-entered ! / !! commands do not go through the LLM tool pipeline.
  pi.on("user_bash", async (event, ctx) => {
    const desc = `bash: ${truncate(String(event.command || ""), 50)}`;
    await recordCheckpoint(ctx, "pre-tool", `before-${desc}`, `Before ${desc}`);
  });

  // ========================================================================
  // Track mutating tools (accumulate per turn, checkpoint at turn_end)
  // ========================================================================

  pi.on("tool_execution_end", async (event, _ctx) => {
    if (!MUTATING_TOOLS.has(event.toolName)) return;

    state.turnHadMutations = true;

    // Get the description captured from tool_call
    const toolDesc =
      state.pendingToolInfo.get(event.toolCallId) || event.toolName;
    state.pendingToolInfo.delete(event.toolCallId);

    state.turnToolDescriptions.push(toolDesc);
  });

  // ========================================================================
  // Create checkpoint at turn_end (1 per model response, like Cline)
  // ========================================================================

  pi.on("turn_end", async (_event, ctx) => {
    if (!state.agentvfsAvailable || !state.socketPath || state.failed) return;
    if (!state.sessionId) return;

    // Only create checkpoint if this turn had mutating tools
    if (state.turnHadMutations) {
      // Build description: prompt + tools
      const promptLabel = state.currentPrompt ? `"${state.currentPrompt}"` : "";
      const toolsLabel = state.turnToolDescriptions.join(", ");
      const desc =
        promptLabel && toolsLabel
          ? `${promptLabel} → ${toolsLabel}`
          : promptLabel || toolsLabel || `Turn ${state.currentTurnIndex}`;

      await recordCheckpoint(ctx, "tool", desc, desc);
    }

    // Reset turn state
    state.turnToolDescriptions = [];
    state.turnHadMutations = false;
  });

  // ========================================================================
  // Fork / tree restore hooks
  // ========================================================================

  pi.on("session_before_fork", async (event, ctx) => {
    return handleForkRestore(state, event, ctx);
  });

  pi.on("session_before_tree", async (event, ctx) => {
    return handleTreeRestore(state, event, ctx);
  });

  pi.on("session_tree", async (event, ctx) => {
    await handleTreeComplete(state, event, ctx);
  });

  // ========================================================================
  // Shutdown
  // ========================================================================

  pi.on("session_shutdown", async () => {
    if (state.pending) await state.pending;
    await persistCheckpointMetadata();
  });
}
