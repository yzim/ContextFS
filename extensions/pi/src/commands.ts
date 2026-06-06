/**
 * pi-agentvfs-rewind — /rewind, /agentvfs, and /tree restore integration
 *
 * Registers user-facing commands and keeps Pi tree navigation aligned with
 * AgentVFS rollback.
 */

import type { ExtensionAPI } from "@earendil-works/pi-coding-agent";
import type { RewindState, CheckpointData } from "./state.js";
import {
  createCheckpoint,
  rollbackTo,
  saveCheckpointMetadata,
  sanitizeForLabel,
} from "./core.js";

// ============================================================================
// Helpers
// ============================================================================

function formatTimestamp(ts: number): string {
  const d = new Date(ts);
  const hh = String(d.getHours()).padStart(2, "0");
  const mm = String(d.getMinutes()).padStart(2, "0");
  const ss = String(d.getSeconds()).padStart(2, "0");
  return `${hh}:${mm}:${ss}`;
}

function formatCheckpointLabel(
  cp: CheckpointData,
  index: number,
): string {
  const time = formatTimestamp(cp.timestamp);
  if (cp.description) {
    return `#${index + 1} [${time}] ${cp.description}`;
  }
  if (cp.trigger === "resume") return `#${index + 1} [${time}] Session start`;
  return `#${index + 1} [${time}] Turn ${cp.turnIndex}`;
}

// ============================================================================
// Restore
// ============================================================================

async function performRestore(
  state: RewindState,
  ctx: { ui: { notify: (msg: string, level: "info" | "warning" | "error") => void } },
  target: CheckpointData,
  opts: {
    safetyLabel?: string;
    safetyTrigger?: CheckpointData["trigger"];
    safetyDescription?: string;
  } = {},
): Promise<void> {
  if (!state.socketPath || !state.sessionId) return;

  // 1. Create before-restore checkpoint as a safety net
  try {
    const ts = Date.now();
    const trigger = opts.safetyTrigger ?? "before-restore";
    const label = opts.safetyLabel ?? "before-restore";
    const id = `${trigger}-${state.sessionId}-${ts}`;
    const result = await createCheckpoint({
      socket: state.socketPath,
      id,
      label,
      sessionId: state.sessionId,
      turnIndex: 0,
      trigger,
      description: opts.safetyDescription,
    });
    const cp: CheckpointData = {
      id,
      label,
      commitHash: result.commitHash,
      sessionId: state.sessionId,
      turnIndex: 0,
      trigger,
      description: opts.safetyDescription,
      timestamp: ts,
    };
    state.redoStack.push(cp);
    await persistCheckpointMetadata(state);
  } catch {
    // Continue anyway — we tried
  }

  // 2. Roll back via agentvfs
  try {
    await rollbackTo(state.socketPath, target.commitHash);
  } catch (err) {
    ctx.ui.notify(
      `Rollback failed: ${err instanceof Error ? err.message : String(err)}`,
      "error"
    );
    throw err; // Re-throw so caller knows it failed
  }
}

async function persistCheckpointMetadata(state: RewindState): Promise<void> {
  if (!state.workspaceRoot || !state.sessionId) return;
  try {
    await saveCheckpointMetadata(state.workspaceRoot, state.sessionId, {
      checkpoints: [...state.checkpoints.values()],
      resumeCheckpoint: state.resumeCheckpoint,
      redoStack: state.redoStack,
      lastTreeRestore: state.lastTreeRestore,
    });
  } catch {
    // Best effort only; the checkpoint commits remain in agentvfs.
  }
}

function findCheckpointByCommit(
  state: RewindState,
  commitHash: string,
): CheckpointData | null {
  for (const cp of state.checkpoints.values()) {
    if (cp.commitHash === commitHash) return cp;
  }
  return null;
}

function findCheckpointAtOrBefore(
  state: RewindState,
  timestamp: number,
): CheckpointData | null {
  const sorted = [...state.checkpoints.values()].sort(
    (a, b) => b.timestamp - a.timestamp
  );
  return sorted.find((cp) => cp.timestamp <= timestamp) ?? state.resumeCheckpoint;
}

function findCheckpointFromTreeEntry(
  state: RewindState,
  ctx: any,
  targetId: string,
): { checkpoint: CheckpointData; mode: "entry" | "timestamp" } | null {
  const getBranch = ctx.sessionManager?.getBranch;
  if (typeof getBranch === "function") {
    const branch = getBranch.call(ctx.sessionManager, targetId);
    for (let i = branch.length - 1; i >= 0; i--) {
      const entry = branch[i];
      if (entry?.type !== "custom") continue;
      if (entry.customType !== "agentvfs_checkpoint") continue;
      const commitHash = entry.data?.commitHash;
      if (typeof commitHash !== "string") continue;
      const cp = findCheckpointByCommit(state, commitHash);
      if (cp) return { checkpoint: cp, mode: "entry" };
    }
  }

  const entry = ctx.sessionManager?.getEntry?.(targetId);
  const targetTs = entry?.timestamp
    ? new Date(entry.timestamp).getTime()
    : Date.now();
  const cp = findCheckpointAtOrBefore(state, targetTs);
  return cp ? { checkpoint: cp, mode: "timestamp" } : null;
}

export function appendCheckpointEntry(pi: ExtensionAPI, cp: CheckpointData): void {
  const appendEntry = (pi as any).appendEntry;
  if (typeof appendEntry !== "function") return;
  try {
    appendEntry("agentvfs_checkpoint", {
      checkpointId: cp.id,
      label: cp.label,
      commitHash: cp.commitHash,
      trigger: cp.trigger,
      turnIndex: cp.turnIndex,
      description: cp.description,
      timestamp: cp.timestamp,
    });
  } catch {
    // Older Pi builds may not expose appendEntry; timestamp matching remains.
  }
}

async function createManualCheckpoint(
  state: RewindState,
  pi: ExtensionAPI,
  labelArg: string | undefined,
): Promise<CheckpointData | null> {
  if (!state.agentvfsAvailable || !state.socketPath || !state.sessionId) {
    return null;
  }
  const ts = Date.now();
  const label = sanitizeForLabel(labelArg?.trim() || `manual-${ts}`);
  const id = `manual-${state.sessionId}-${ts}`;
  const result = await createCheckpoint({
    socket: state.socketPath,
    id,
    label,
    sessionId: state.sessionId,
    turnIndex: state.currentTurnIndex,
    trigger: "manual",
    description: labelArg?.trim() || "Manual checkpoint",
  });
  const cp: CheckpointData = {
    id,
    label,
    commitHash: result.commitHash,
    sessionId: state.sessionId,
    turnIndex: state.currentTurnIndex,
    trigger: "manual",
    description: labelArg?.trim() || "Manual checkpoint",
    timestamp: ts,
  };
  state.checkpoints.set(cp.id, cp);
  appendCheckpointEntry(pi, cp);
  await persistCheckpointMetadata(state);
  return cp;
}

// ============================================================================
// Rewind flow
// ============================================================================

export async function runRewindFlow(
  state: RewindState,
  ctx: any,
): Promise<void> {
  try {
    if (!state.agentvfsAvailable || !state.socketPath || !state.sessionId) {
      ctx.ui.notify("Rewind not available (agentvfs not connected)", "warning");
      return;
    }

    const MAX_DISPLAY = 25;
    const checkpoints = [...state.checkpoints.values()]
      .sort((a, b) => b.timestamp - a.timestamp)
      .slice(0, MAX_DISPLAY);

    if (checkpoints.length === 0) {
      ctx.ui.notify("No checkpoints available", "warning");
      return;
    }

    // Build picker items
    const items: string[] = [];
    const undoRef =
      state.redoStack.length > 0
        ? state.redoStack[state.redoStack.length - 1]
        : null;
    if (undoRef) {
      items.push("↩ Undo last rewind");
    }
    for (let i = 0; i < checkpoints.length; i++) {
      items.push(formatCheckpointLabel(checkpoints[i], i));
    }

    const choice = await ctx.ui.select("Rewind to checkpoint:", items);
    if (!choice) {
      ctx.ui.notify("Rewind cancelled", "info");
      return;
    }

    // Handle undo
    if (choice === "↩ Undo last rewind" && undoRef) {
      await performRestore(state, ctx, undoRef);
      state.redoStack = state.redoStack.filter((cp) => cp.id !== undoRef.id);
      await persistCheckpointMetadata(state);
      ctx.ui.notify("Undo successful — workspace restored to before last rewind", "info");
      return;
    }

    // Find selected checkpoint
    const idx = items.indexOf(choice) - (undoRef ? 1 : 0);
    if (idx < 0 || idx >= checkpoints.length) return;
    const target = checkpoints[idx];

    await performRestore(state, ctx, target);

    ctx.ui.notify(`Rewound workspace to checkpoint #${idx + 1}`, "info");
  } catch (err) {
    ctx.ui.notify(
      `Rewind failed: ${err instanceof Error ? err.message : String(err)}`,
      "error"
    );
  }
}

// ============================================================================
// Handle fork/tree workspace alignment
// ============================================================================

export async function handleForkRestore(
  state: RewindState,
  event: { entryId: string },
  ctx: any
): Promise<{ cancel: true } | undefined> {
  if (!state.agentvfsAvailable || !state.socketPath || !state.sessionId)
    return undefined;
  if (!ctx.hasUI) return undefined;

  try {
    const entry = ctx.sessionManager.getEntry(event.entryId);
    const targetTs = entry?.timestamp
      ? new Date(entry.timestamp).getTime()
      : Date.now();

    const cp = findCheckpointAtOrBefore(state, targetTs);

    if (!cp) {
      ctx.ui.notify("No checkpoint available", "warning");
      return undefined;
    }

    await performRestore(state, ctx, cp);
    ctx.ui.notify("Workspace restored from checkpoint", "info");

    return undefined;
  } catch (err) {
    ctx.ui.notify(
      `Fork restore failed: ${err instanceof Error ? err.message : String(err)}`,
      "error"
    );
    return { cancel: true };
  }
}

export async function handleTreeRestore(
  state: RewindState,
  event: { preparation: { targetId: string } },
  ctx: any
): Promise<{ cancel: true } | undefined> {
  if (!state.treeIntegrationEnabled) return undefined;
  if (!state.agentvfsAvailable || !state.socketPath || !state.sessionId)
    return undefined;
  if (!ctx.hasUI) return undefined;

  try {
    const match = findCheckpointFromTreeEntry(
      state,
      ctx,
      event.preparation.targetId,
    );

    if (match) {
      await performRestore(state, ctx, match.checkpoint, {
        safetyLabel: "before-tree-restore",
        safetyTrigger: "before-tree-restore",
        safetyDescription: "Before /tree restore",
      });
      state.lastTreeRestore = {
        targetId: event.preparation.targetId,
        checkpointId: match.checkpoint.id,
        commitHash: match.checkpoint.commitHash,
        timestamp: Date.now(),
        mode: match.mode,
      };
      await persistCheckpointMetadata(state);
      ctx.ui.notify(
        `Workspace restored for /tree (${match.mode} checkpoint match)`,
        "info",
      );
    }

    return undefined;
  } catch (err) {
    ctx.ui.notify(
      `Tree restore failed: ${err instanceof Error ? err.message : String(err)}`,
      "error"
    );
    return { cancel: true };
  }
}

export async function handleTreeComplete(
  state: RewindState,
  _event: { newLeafId: string | null; oldLeafId: string | null },
  ctx: any,
): Promise<void> {
  if (!ctx.hasUI || !state.agentvfsAvailable) return;
  if (state.lastTreeRestore) {
    ctx.ui.setStatus(
      "agentvfs-tree",
      `tree restore: ${state.lastTreeRestore.mode}`,
    );
  }
}

function formatStatus(state: RewindState, ctx: any): string {
  const last = [...state.checkpoints.values()].sort(
    (a, b) => b.timestamp - a.timestamp,
  )[0];
  const lines = [
    `agentvfs: ${state.agentvfsAvailable ? "connected" : "not connected"}`,
    `mount: ${state.mountPoint ?? "(none)"}`,
    `socket: ${state.socketPath ?? "(none)"}`,
    `workspace: ${state.workspaceRoot ?? "(none)"}`,
    `cwd: ${ctx.cwd ?? process.cwd()}`,
    `checkpoints: ${state.checkpoints.size}`,
    `last checkpoint: ${last ? `${last.label} ${last.commitHash.slice(0, 12)}` : "(none)"}`,
    `tree integration: ${state.treeIntegrationEnabled ? "restore" : "off"}`,
    `last /tree restore: ${
      state.lastTreeRestore
        ? `${state.lastTreeRestore.mode} ${state.lastTreeRestore.commitHash.slice(0, 12)}`
        : "(none)"
    }`,
  ];
  return lines.join("\n");
}

async function runAgentvfsCommand(
  state: RewindState,
  pi: ExtensionAPI,
  args: string,
  ctx: any,
): Promise<void> {
  const trimmed = args.trim();
  const [subcommand, ...rest] = trimmed ? trimmed.split(/\s+/) : ["status"];
  switch (subcommand) {
    case "":
    case "status":
      ctx.ui.notify(formatStatus(state, ctx), "info");
      return;

    case "checkpoint": {
      const label = rest.join(" ").trim();
      const cp = await createManualCheckpoint(state, pi, label || undefined);
      if (!cp) {
        ctx.ui.notify("agentvfs checkpoint unavailable (not connected)", "warning");
        return;
      }
      ctx.ui.notify(
        `agentvfs checkpoint ${cp.label}: ${cp.commitHash.slice(0, 12)}`,
        "info",
      );
      return;
    }

    case "rewind":
      await runRewindFlow(state, ctx);
      return;

    case "tree": {
      const mode = rest[0];
      if (mode === "on" || mode === "restore") {
        state.treeIntegrationEnabled = true;
      } else if (mode === "off") {
        state.treeIntegrationEnabled = false;
      } else {
        ctx.ui.notify(
          `tree integration: ${state.treeIntegrationEnabled ? "restore" : "off"}`,
          "info",
        );
        return;
      }
      await persistCheckpointMetadata(state);
      ctx.ui.notify(
        `agentvfs /tree integration ${state.treeIntegrationEnabled ? "enabled" : "disabled"}`,
        "info",
      );
      return;
    }

    case "mount":
      ctx.ui.notify(
        state.mountPoint
          ? `agentvfs mount: ${state.mountPoint}`
          : "agentvfs mount: unavailable",
        state.mountPoint ? "info" : "warning",
      );
      return;

    default:
      ctx.ui.notify(
        "Usage: /agentvfs [status|checkpoint <label>|rewind|tree on|tree off|mount]",
        "warning",
      );
  }
}

// ============================================================================
// Registration
// ============================================================================

export function registerCommands(pi: ExtensionAPI, state: RewindState): void {
  pi.registerCommand("rewind", {
    description: "Rewind workspace to an agentvfs checkpoint",
    handler: async (_args, ctx) => {
      try {
        await runRewindFlow(state, ctx);
      } catch (err) {
        ctx.ui.notify(
          `/rewind failed: ${err instanceof Error ? err.message : String(err)}`,
          "error"
        );
      }
    },
  });

  pi.registerCommand("agentvfs", {
    description: "Show and control AgentVFS workspace integration",
    getArgumentCompletions: (prefix: string) => {
      const items = ["status", "checkpoint", "rewind", "tree on", "tree off", "mount"];
      const filtered = items.filter((item) => item.startsWith(prefix));
      return filtered.length > 0
        ? filtered.map((item) => ({ value: item, label: item }))
        : null;
    },
    handler: async (args, ctx) => {
      try {
        await runAgentvfsCommand(state, pi, String(args || ""), ctx);
      } catch (err) {
        ctx.ui.notify(
          `/agentvfs failed: ${err instanceof Error ? err.message : String(err)}`,
          "error"
        );
      }
    },
  });
}
