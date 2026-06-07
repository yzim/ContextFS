# Pi AgentVFS autostart and cwd handoff

**Date:** 2026-06-05
**Status:** Draft, awaiting user review
**Scope:** Pi launch-time workspace handoff plus Pi extension integration. No daemon filesystem semantics change.

## Problem

The Pi extension currently assumes Pi has already been launched from inside an
AgentVFS mount. If Pi starts from the original source directory, the extension
disables rewind because file reads and writes bypass FUSE, so rollback cannot
restore the files Pi actually edited.

We want an opt-in mode where Pi startup does the missing setup:

1. Detect the user's intended project directory.
2. Create or reuse an `agentvfs workspace`.
3. Mount it.
4. Switch Pi's working directory into the mounted tree before agent tools run.
5. Continue using the existing checkpoint and rewind behavior.

## Goals

1. Add this behavior as an explicit option, disabled by default.
2. Reuse `agentvfs workspace init/start/status` instead of spawning the daemon
   directly from TypeScript.
3. Preserve the user's relative cwd. If Pi starts in `<project>/sub/dir`, the
   post-mount cwd should be `<mount>/sub/dir`.
4. Be idempotent. Starting Pi repeatedly for the same project should reuse the
   same workspace and mount.
5. Fail closed. If auto-mount is required but setup fails, Pi should not proceed
   in the original source directory with rewind pretending to work.
6. Keep the current manual flow valid: users can still start Pi from an existing
   AgentVFS mount with no auto-mount option enabled.

## Non-goals

- Bidirectional sync from the AgentVFS workspace back to the original source
  tree.
- Automatic re-seeding of an existing workspace from the original source tree.
- Managing multiple AgentVFS branches from Pi startup.
- Changing core AgentVFS daemon or FUSE behavior.
- macOS/Windows support for this option before `agentvfs workspace` is portable.

## Proposed option

Add a Pi extension option named `agentvfs.autoMount`.

Accepted values:

| Value | Behavior |
|---|---|
| `off` | Default. Current behavior: require Pi cwd to already be inside an AgentVFS mount. |
| `auto` | If cwd is already inside a mount, reuse it. Otherwise create/reuse a workspace, mount it, and switch cwd. Warn and continue without rewind only if setup fails. |
| `require` | Same as `auto`, but setup failure aborts startup or blocks the first agent turn. |

Environment aliases for early implementation:

```bash
PI_AGENTVFS_AUTOMOUNT=off|auto|require
PI_AGENTVFS_WORKSPACE=<name>
PI_AGENTVFS_ROOT=<workspace-root>
PI_AGENTVFS_SOURCE=<source-root>
PI_AGENTVFS_MOUNT=<mount-dir>
PI_AGENTVFS_TELEMETRY=auto|none|<csv>
```

The environment variables make the feature usable even if Pi's extension
configuration surface is limited or changes.

## Source and workspace resolution

At startup, capture `initialCwd = ctx.cwd || process.cwd()`.

1. If `initialCwd` is already inside a `fuse.agentvfs` mount, do nothing. The
   extension continues with today's `ensureAgentvfs` path.
2. Otherwise resolve `sourceRoot`:
   - `PI_AGENTVFS_SOURCE`, if set.
   - The nearest Git root containing `initialCwd`, if one exists.
   - `initialCwd`, as fallback.
3. Compute `relativeCwd = path.relative(sourceRoot, initialCwd)`.
   - Reject if it escapes `sourceRoot`.
   - Empty relative cwd means the project root.
4. Resolve `workspaceName`:
   - `PI_AGENTVFS_WORKSPACE`, if set.
   - `pi-<basename(sourceRoot)>-<sha256(sourceRoot)[0..8]>`, sanitized to the
     existing `agentvfs workspace` name grammar.
5. Resolve `workspaceRoot`:
   - `PI_AGENTVFS_ROOT`, if set.
   - AgentVFS default (`$XDG_RUNTIME_DIR/agentvfs` or `/tmp/agentvfs-<uid>`).

This gives stable reuse for the same source directory while avoiding collisions
between projects with the same basename.

## Startup flow

### Preferred: pre-Pi launcher

The reliable implementation is a launcher that runs before Pi starts. This
avoids a split-brain cwd where Pi has already resolved some relative paths
against the original source directory and later tool execution resolves new
relative paths against the AgentVFS mount.

```text
shell cwd = <sourceRoot>/subdir
  -> pi-agentvfs captures original cwd and argv
  -> pi-agentvfs resolves invocation-time relative paths to absolute paths
  -> pi-agentvfs init/start/reuses AgentVFS workspace
  -> pi-agentvfs computes next cwd = <mount>/subdir
  -> pi-agentvfs exports AGENTVFS_* handoff env
  -> pi-agentvfs chdir(next cwd)
  -> exec pi with normalized argv
  -> pi extension sees cwd already inside AgentVFS mount
```

The launcher must normalize path-valued startup arguments before `chdir`. The
rule is:

- Paths under `sourceRoot` are rewritten to the corresponding path under
  `mount`.
- Paths outside `sourceRoot` are made absolute and left outside the mount.
- The extension path itself should be absolute before `exec pi`, so
  `pi -e ./extensions/pi/src/index.ts` keeps working even though cwd changes.
- User prompts and subsequent tool paths are not pre-normalized; after `exec`,
  they naturally resolve under the mounted cwd.

The extension can still own checkpoint/rewind behavior, but startup mount/cwd
handoff should happen before Pi initializes sessions, config, tools, or path
caches.

### Extension-only fallback

The extension gets a new startup phase before checkpoint initialization:

```text
Pi starts
  -> pi-agentvfs extension loads
  -> maybeAutoMount(initialCwd)
       if inside existing AgentVFS mount:
         return mount info
       if autoMount=off:
         return disabled
       resolve sourceRoot/workspaceName/workspaceRoot/relativeCwd
       ensure workspace exists
       start or reuse workspace
       switch Pi cwd to mount/relativeCwd
       export AGENTVFS_SOCK=<socket>
  -> existing initSession()
  -> first user turn/tool runs from mounted cwd
```

This path is allowed only if Pi exposes an API that atomically updates every
cwd-dependent component before any user turn or tool path is resolved. Calling
`process.chdir()` inside the extension is not enough unless Pi explicitly
documents that all file tools, shell commands, session paths, and path caches
derive from `process.cwd()` after the extension hook.

If this cannot be proven, `agentvfs.autoMount=auto` should warn and stay
disabled, and `agentvfs.autoMount=require` should fail with a launcher
suggestion.

Workspace creation:

1. Run `agentvfs workspace status <name> --root <root>`.
2. If status is `started`, parse `mount=` and `socket=`.
3. If status is `stale` or `stopped` for an initialized workspace, run:

   ```bash
   agentvfs workspace start <name> --root <root> --telemetry <telemetry> [--mount <mount>]
   ```

4. If there is no initialized workspace, run:

   ```bash
   agentvfs workspace init <name> --from <sourceRoot> --root <root> [--mount <mount>]
   agentvfs workspace start <name> --root <root> --telemetry <telemetry>
   ```

Important: `init` is only for first creation. Later starts reuse the workspace's
own `source/` tree. We should not silently copy fresh files from the original
source into an existing AgentVFS workspace because that would mix two sources of
truth.

### Initialization marker

The current workspace CLI does not clearly distinguish "never initialized" from
"initialized but stopped" through `workspace status` alone. For this feature,
add a small workspace metadata improvement:

- `workspace init` writes `workspace.json` even when there is no mount override
  or `allow_root`, with fields:

  ```json
  {
    "initialized": true,
    "seeded_from": "/absolute/source/root",
    "seeded_from_hash": "<sha256(sourceRoot)>",
    "mount_override": "",
    "allow_root": false
  }
  ```

- `workspace status` prints `status=initialized` when `workspace.json` says the
  workspace has been seeded but there is no live/stopped `session.json` yet.
- Existing configs without `initialized` remain valid. For compatibility, the
  Pi extension can treat an existing `session.json` as initialized.

This prevents the extension from calling `workspace start` on an unseeded
workspace and accidentally giving Pi an empty mount.

## Switching Pi cwd

The reliable behavior is: every Pi tool, file read, write, shell command, and
session path should see `mount/relativeCwd` as cwd.

Implementation order:

1. Preferred: use the `pi-agentvfs` launcher, because Pi starts with the final
   cwd from byte zero.
2. Acceptable only with proof: use an official Pi API if one exists, for example
   `ctx.workspace.setCwd(nextCwd)` or equivalent. This must update tool cwd,
   session metadata, and any path caches, not only Node's process cwd.
3. Development-only fallback: call `process.chdir(nextCwd)` and update local
   extension state. This mode must not claim strict safety unless Pi tools are
   verified to derive cwd from `process.cwd()` after the hook.

   ```bash
   pi-agentvfs --auto-mount=require -- pi -e <extension> ...
   ```

The launcher is not a separate product mode. It is the execution layer for the
same option when we need launch-time path consistency.

## State additions

Extend `RewindState`:

```ts
interface AutoMountState {
  mode: "off" | "auto" | "require";
  sourceRoot: string | null;
  initialCwd: string | null;
  effectiveCwd: string | null;
  workspaceName: string | null;
  workspaceRoot: string | null;
  workspaceSessionMount: string | null;
  autoMounted: boolean;
}
```

Keep checkpoint metadata keyed by the mount path, as today. The original source
path is startup metadata only; rollback should continue operating against the
AgentVFS mount.

## CLI wrapper in TypeScript

Add core helpers in `extensions/pi/src/core.ts`:

- `runAgentvfsWorkspace(args): Promise<Record<string,string>>`
- `resolveSourceRoot(initialCwd, env): Promise<string>`
- `deriveWorkspaceName(sourceRoot, env): string`
- `ensureWorkspaceMounted(opts): Promise<{ mount, socket, sourceRoot }>`
- `switchCwd(nextCwd, ctx): Promise<"pi-api"|"process"|"unsupported">`

All `agentvfs workspace` output is already line-oriented `key=value`; parse it
with the existing `parse_key_value_lines` contract mirrored in TypeScript.

## Failure behavior

| Situation | `auto` | `require` |
|---|---|---|
| `agentvfs` missing | Warn, disable rewind | Abort/block startup |
| FUSE mount fails | Warn, disable rewind | Abort/block startup |
| `relativeCwd` missing under mount | Warn, switch to mount root | Abort/block startup |
| Cannot switch Pi cwd | Warn, disable rewind and suggest launcher | Abort/block startup with launcher suggestion |
| Existing workspace is healthy | Reuse | Reuse |
| Existing workspace is stale | Run `workspace start` recovery | Run `workspace start` recovery |

The extension must never report rewind as enabled when cwd switching failed.

## UX

Manual flow remains:

```bash
cd /run/user/$(id -u)/agentvfs/my-task/mount
pi -e /path/to/extensions/pi/src/index.ts
```

Auto flow:

```bash
cd /path/to/project
PI_AGENTVFS_AUTOMOUNT=auto pi -e /path/to/extensions/pi/src/index.ts
```

Strict flow for repeatable use:

```bash
cd /path/to/project/subdir
PI_AGENTVFS_AUTOMOUNT=require \
PI_AGENTVFS_WORKSPACE=my-task \
pi -e /path/to/extensions/pi/src/index.ts
```

Expected startup notification:

```text
agentvfs mounted: workspace=my-task cwd=/run/user/1000/agentvfs/my-task/mount/subdir
```

If launcher fallback is needed:

```text
agentvfs auto-mount started, but this Pi build does not allow extensions to change cwd.
Restart with: pi-agentvfs --auto-mount=require -- pi ...
```

## `/tree` integration

Pi already exposes the right lifecycle for merging `/rewind` behavior into the
native `/tree` command:

- `/tree` opens Pi's built-in tree selector.
- After the user selects a target and optional branch summary behavior, Pi calls
  `session.navigateTree(targetId, ...)`.
- `navigateTree` emits `session_before_tree` before changing the session leaf
  and before rebuilding the agent message state.
- If any extension returns `{ cancel: true }`, Pi aborts navigation.
- After a successful leaf change, Pi emits `session_tree`.

The current extension already hooks `session_before_tree` through
`handleTreeRestore`. That is the right event to make `/tree` naturally use
AgentVFS rollback: when the user selects a prior point in the Pi tree, the
extension restores the filesystem to the nearest matching AgentVFS checkpoint
before Pi commits the tree navigation.

The user-facing model should become:

- `/tree` is the primary time-travel UI for both conversation and workspace.
- `/rewind` remains available as a direct filesystem checkpoint browser, useful
  when the user wants to restore files without navigating the Pi conversation.
- `Esc Esc` should default back to Pi's native `/tree` behavior when this
  integration is enabled, instead of shadowing it with the extension's quick
  rewind picker.

Implementation adjustments:

1. Add an option `agentvfs.treeIntegration=off|restore`, default `restore` when
   AgentVFS is available.
2. Keep `handleTreeRestore` in `session_before_tree`, but make the restore
   policy explicit:
   - create a `before-tree-restore` checkpoint for undo;
   - find the latest checkpoint at or before the selected tree entry timestamp;
   - rollback to that checkpoint;
   - return `{ cancel: true }` if rollback fails.
3. Listen to `session_tree` only for status/metadata cleanup, not for rollback.
   Rolling back after `session_tree` would leave Pi conversation state switched
   while the workspace restore may still fail.
4. Add restore metadata to local checkpoint state so `/agentvfs status` can
   report the last tree-linked restore.

Known limitation: timestamp matching is approximate. It is acceptable for the
current checkpoint strategy, but the better long-term design is to append a
small custom Pi session entry whenever an AgentVFS checkpoint is created:

```ts
ctx.sessionManager.appendEntry({
  type: "custom",
  customType: "agentvfs_checkpoint",
  data: { commitHash, checkpointId, label, trigger, turnIndex }
});
```

Then `/tree` restore can walk ancestors from `preparation.targetId` and choose
the nearest explicit `agentvfs_checkpoint` entry, instead of relying on
timestamps. If Pi's read-only `ctx.sessionManager` does not allow append from
normal event handlers, keep timestamp matching for v0 and revisit this with a
small Pi API addition.

## `/agentvfs` command

Add an extension command `/agentvfs` as the operational surface for this
integration. Proposed subcommands:

```text
/agentvfs
/agentvfs status
/agentvfs checkpoint [label]
/agentvfs rewind
/agentvfs tree on|off
/agentvfs mount
```

Behavior:

- `/agentvfs` and `/agentvfs status` show mount, socket, workspace name,
  source root, current checkpoint count, last checkpoint, last `/tree` restore,
  and whether `/tree` integration is enabled.
- `/agentvfs checkpoint [label]` creates a manual checkpoint through the same
  metadata path as automatic checkpoints.
- `/agentvfs rewind` opens the existing `/rewind` picker. This lets us keep
  `/rewind` as a compatibility alias while making `/agentvfs` the discoverable
  namespace.
- `/agentvfs tree on|off` toggles `agentvfs.treeIntegration` for the current
  process; persistence can come later if Pi exposes extension settings writes.
- `/agentvfs mount` reports whether the current process was launcher-mounted,
  manually mounted, or extension fallback mounted.

## Tests

Unit tests for TypeScript helpers:

- cwd already inside AgentVFS mount returns no-op.
- Git-root source detection preserves relative cwd.
- workspace name derivation is stable and valid.
- `key=value` parser handles warnings plus normal output.
- require mode throws on mount or cwd-switch failure.

Shell/integration tests:

1. Start from a normal project root with `PI_AGENTVFS_AUTOMOUNT=require`.
   Assert Pi-visible cwd is the AgentVFS mount.
2. Start from a project subdirectory. Assert relative cwd is preserved.
3. Start twice. Assert the second start reuses the same `mount=` path.
4. Kill daemon, start again. Assert `workspace start` recovers stale session.
5. Simulate unsupported cwd switch. Assert `auto` disables rewind and `require`
   blocks startup.

Existing `tests/cas/test_agentvfs_workspace.sh` should remain the workspace CLI
coverage; Pi tests should focus on orchestration and cwd handoff.

## Risks and mitigations

1. **Pi may not expose a real cwd mutation API.** Mitigation: implement a
   launcher fallback and keep extension-side switching behind a capability check.
2. **Users may edit the original source after first auto-init and expect those
   files in the workspace.** Mitigation: document that auto-init seeds once and
   the AgentVFS workspace becomes the active tree.
3. **Workspace name collisions or long socket paths.** Mitigation: hash source
   path into the default name and allow `PI_AGENTVFS_ROOT` for shorter roots.
4. **Starting from a nested mount/source combination.** Mitigation: always no-op
   when cwd is already inside a `fuse.agentvfs` mount.
5. **Partially mounted but unhealthy sessions.** Mitigation: rely on
   `agentvfs workspace start` locking and stale recovery, not custom cleanup in
   the Pi extension.

## Open questions

1. Should the default option be `off` for safety, or `auto` once this extension
   is explicitly installed?
2. Do we want the launcher as part of `extensions/pi`, or as a top-level
   installed binary next to `agentvfs-quickstart`?
3. Should first-run auto-init use Git root by default, or should it use the
   exact launch cwd unless `PI_AGENTVFS_SOURCE` is set?
4. Should there be an explicit `/agentvfs status` Pi command showing source,
   workspace name, mount, socket, and cwd-switch method?
