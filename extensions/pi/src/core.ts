/**
 * pi-agentvfs-rewind — Core agentvfs operations
 *
 * Thin wrapper over agentvfs-ctl with zero pi-coding-agent dependency.
 */

import { spawn } from "child_process";
import { createHash } from "crypto";
import { readdirSync } from "fs";
import { mkdir, readFile, rename, writeFile } from "fs/promises";
import { homedir } from "os";
import { dirname, join, resolve } from "path";
import type { CheckpointData } from "./state.js";

// ============================================================================
// Constants
// ============================================================================

/** Tools that modify the filesystem and warrant a checkpoint */
export const MUTATING_TOOLS = new Set(["write", "edit", "bash"]);

export const DEFAULT_MAX_CHECKPOINTS = 50;

const METADATA_VERSION = 1;

// ============================================================================
// Socket discovery
// ============================================================================

/** Look for an agentvfs control socket. */
export async function findAgentvfsSocket(): Promise<string | null> {
  const env = process.env.AGENTVFS_SOCK;
  if (env) return env;

  try {
    const files = readdirSync("/tmp");
    const sock = files.find(
      (f) => f.startsWith("agentvfs-") && f.endsWith(".sock")
    );
    if (sock) return `/tmp/${sock}`;
  } catch {
    // ignore
  }
  return null;
}

// ============================================================================
// Mount discovery
// ============================================================================

export interface AgentvfsMount {
  mountPoint: string;
}

function decodeMountPath(path: string): string {
  return path.replace(/\\([0-7]{3})/g, (_m, octal) =>
    String.fromCharCode(parseInt(octal, 8))
  );
}

function isWithinPath(path: string, parent: string): boolean {
  const p = resolve(path);
  const base = resolve(parent);
  return p === base || p.startsWith(base.endsWith("/") ? base : `${base}/`);
}

/** Find the agentvfs FUSE mount that contains cwd, if any. */
export async function findAgentvfsMountForPath(
  cwd: string
): Promise<AgentvfsMount | null> {
  try {
    const raw = await readFile("/proc/mounts", "utf8");
    const matches: AgentvfsMount[] = [];
    for (const line of raw.split("\n")) {
      if (!line.trim()) continue;
      const fields = line.split(" ");
      if (fields.length < 3) continue;
      const fstype = fields[2];
      if (fstype !== "fuse.agentvfs") continue;
      const mountPoint = decodeMountPath(fields[1]);
      if (isWithinPath(cwd, mountPoint)) matches.push({ mountPoint });
    }
    matches.sort((a, b) => b.mountPoint.length - a.mountPoint.length);
    return matches[0] ?? null;
  } catch {
    return null;
  }
}

// ============================================================================
// agentvfs-ctl wrapper
// ============================================================================

const CTL_TIMEOUT_MS = 30000;

function runCtl(
  socket: string,
  args: string[],
  opts: { input?: string; timeout?: number } = {}
): Promise<string> {
  return new Promise((resolve, reject) => {
    const proc = spawn("agentvfs-ctl", ["--sock", socket, ...args], {
      stdio: ["pipe", "pipe", "pipe"],
    });

    let stdout = "";
    let stderr = "";
    let done = false;

    const timeout = setTimeout(() => {
      if (done) return;
      done = true;
      try { proc.kill("SIGTERM"); } catch { /* ignore */ }
      reject(new Error(`agentvfs-ctl timeout (${opts.timeout ?? CTL_TIMEOUT_MS}ms)`));
    }, opts.timeout ?? CTL_TIMEOUT_MS);

    proc.stdout.on("data", (d) => (stdout += d));
    proc.stderr.on("data", (d) => (stderr += d));

    proc.on("close", (code) => {
      if (done) return;
      done = true;
      clearTimeout(timeout);
      if (code === 0) {
        resolve(stdout.trim());
      } else {
        reject(new Error(stderr || `agentvfs-ctl exited ${code}`));
      }
    });
    proc.on("error", (err) => {
      if (done) return;
      done = true;
      clearTimeout(timeout);
      reject(err);
    });

    if (opts.input && proc.stdin) {
      proc.stdin.write(opts.input);
      proc.stdin.end();
    } else if (proc.stdin) {
      proc.stdin.end();
    }
  });
}

// ============================================================================
// Checkpoint CRUD
// ============================================================================

export interface CreateCheckpointOpts {
  socket: string;
  id: string;
  label?: string;
  sessionId: string;
  turnIndex: number;
  trigger: "resume" | "pre-tool" | "tool" | "before-restore" | "before-tree-restore" | "manual";
  description?: string;
}

export interface CheckpointResult {
  commitHash: string;
  timestamp: number;
}

export interface PersistedCheckpointState {
  checkpoints: CheckpointData[];
  resumeCheckpoint: CheckpointData | null;
  redoStack: CheckpointData[];
  lastTreeRestore?: {
    targetId: string;
    checkpointId: string;
    commitHash: string;
    timestamp: number;
    mode: "entry" | "timestamp";
  } | null;
}

/** Create a checkpoint via agentvfs-ctl. Returns the commit hash. */
export async function createCheckpoint(
  opts: CreateCheckpointOpts
): Promise<CheckpointResult> {
  const args = opts.label ? ["checkpoint", opts.label] : ["checkpoint"];
  const out = await runCtl(opts.socket, args);
  const commitHash = out.split("\n")[0].trim();
  return { commitHash, timestamp: Date.now() };
}

/** Roll back to a checkpoint target (label or commit hash). */
export async function rollbackTo(
  socket: string,
  target: string
): Promise<string> {
  const out = await runCtl(socket, ["rollback", target]);
  return out.split("\n")[0].trim();
}

/** Get daemon status as parsed JSON. */
export async function getStatus(
  socket: string,
  timeout = 5000
): Promise<any | null> {
  try {
    const out = await runCtl(socket, ["status"], { timeout });
    return JSON.parse(out);
  } catch {
    return null;
  }
}

/** Delete a checkpoint label from agentvfs (best-effort). */
export async function deleteCheckpointLabel(
  socket: string,
  label: string
): Promise<void> {
  // agentvfs-ctl has no explicit delete; we simply drop it from local cache.
  // Future: could send a raw command if the protocol adds one.
  void label;
}

// ============================================================================
// Local metadata persistence
// ============================================================================

function metadataPath(workspace: string): string {
  const base =
    process.env.XDG_STATE_HOME || join(homedir(), ".local", "state");
  const hash = createHash("sha256").update(workspace).digest("hex").slice(0, 16);
  return join(base, "pi-agentvfs-rewind", `checkpoints-${hash}.json`);
}

function isCheckpointData(value: any): value is CheckpointData {
  return Boolean(
    value &&
    typeof value.id === "string" &&
    typeof value.label === "string" &&
    typeof value.commitHash === "string" &&
    typeof value.sessionId === "string" &&
    typeof value.turnIndex === "number" &&
    typeof value.trigger === "string" &&
    typeof value.timestamp === "number"
  );
}

/** Load persisted checkpoint metadata for this workspace/session. */
export async function loadCheckpointMetadata(
  workspace: string,
  sessionId: string
): Promise<PersistedCheckpointState> {
  try {
    const raw = await readFile(metadataPath(workspace), "utf8");
    const parsed = JSON.parse(raw);
    if (parsed.version !== METADATA_VERSION) {
      return { checkpoints: [], resumeCheckpoint: null, redoStack: [] };
    }

    const checkpoints: CheckpointData[] = Array.isArray(parsed.checkpoints)
      ? parsed.checkpoints.filter(isCheckpointData)
      : [];
    const scoped = checkpoints.filter((cp) => cp.sessionId === sessionId);

    const resumeCheckpoint = isCheckpointData(parsed.resumeCheckpoint) &&
      parsed.resumeCheckpoint.sessionId === sessionId
        ? parsed.resumeCheckpoint
        : scoped.find((cp) => cp.trigger === "resume") ?? null;

    const parsedRedoStack: CheckpointData[] = Array.isArray(parsed.redoStack)
      ? parsed.redoStack.filter(isCheckpointData)
      : [];
    const redoStack = parsedRedoStack.filter((cp) => cp.sessionId === sessionId);

    const lastTreeRestore =
      parsed.lastTreeRestore &&
      typeof parsed.lastTreeRestore.targetId === "string" &&
      typeof parsed.lastTreeRestore.checkpointId === "string" &&
      typeof parsed.lastTreeRestore.commitHash === "string" &&
      typeof parsed.lastTreeRestore.timestamp === "number" &&
      (parsed.lastTreeRestore.mode === "entry" ||
        parsed.lastTreeRestore.mode === "timestamp")
        ? parsed.lastTreeRestore
        : null;

    return { checkpoints: scoped, resumeCheckpoint, redoStack, lastTreeRestore };
  } catch {
    return { checkpoints: [], resumeCheckpoint: null, redoStack: [] };
  }
}

/** Persist checkpoint metadata so resumed Pi sessions can show old checkpoints. */
export async function saveCheckpointMetadata(
  workspace: string,
  sessionId: string,
  state: PersistedCheckpointState
): Promise<void> {
  const path = metadataPath(workspace);
  await mkdir(dirname(path), { recursive: true });

  const checkpoints = state.checkpoints
    .filter((cp) => cp.sessionId === sessionId)
    .sort((a, b) => a.timestamp - b.timestamp)
    .slice(-DEFAULT_MAX_CHECKPOINTS);

  const payload = {
    version: METADATA_VERSION,
    workspace,
    sessionId,
    updatedAt: Date.now(),
    checkpoints,
    resumeCheckpoint: state.resumeCheckpoint,
    redoStack: state.redoStack.filter((cp) => cp.sessionId === sessionId),
    lastTreeRestore: state.lastTreeRestore ?? null,
  };

  const tmp = `${path}.${process.pid}.tmp`;
  await writeFile(tmp, JSON.stringify(payload, null, 2), "utf8");
  await rename(tmp, path);
}

// ============================================================================
// Utilities
// ============================================================================

/** Truncate a string to maxLen, adding ellipsis if needed. */
export function truncate(s: string, maxLen: number): string {
  if (s.length <= maxLen) return s;
  return s.slice(0, maxLen - 1) + "…";
}

/** Sanitize a string for use as an agentvfs label. */
export function sanitizeForLabel(s: string): string {
  return s.replace(/[^a-zA-Z0-9_\-.]/g, "_").slice(0, 64);
}
