# pi-agentvfs-rewind

Checkpoint/rewind extension for the [Pi coding agent](https://github.com/earendil-works/pi-coding-agent), backed by [ContextFS](https://github.com/thustorage/ContextFS) `agentvfs` daemon.

## What it does

- Automatically creates **checkpoints** on the ContextFS FUSE mount at the end of every model turn that had mutating tools (`write`, `edit`, `bash`).
- Integrates with Pi's native **`/tree`** command so conversation-tree navigation restores the matching agentvfs checkpoint before Pi switches branches.
- Provides a **`/agentvfs`** command for status, manual checkpoints, direct rewind, and toggling `/tree` integration.
- Keeps **`/rewind`** as a compatibility command to browse checkpoints and roll the agentvfs workspace back without navigating the Pi conversation.
- Persists checkpoint metadata under the user's state directory so resumed Pi sessions can show checkpoints from before exit.
- Maintains an **undo stack** so you can undo a rewind itself.
- Keeps **fork** and **tree navigation** aligned with the matching agentvfs checkpoint; it does not offer a "keep current files" mode.

## Requirements

1. `agentvfs` daemon must be running on your workspace mount.
2. `agentvfs-ctl` must be on your `$PATH`.
3. `AGENTVFS_SOCK` should point to the daemon's control socket. The extension
   can fall back to scanning `/tmp/agentvfs-*.sock`, but an explicit socket is
   safer when more than one daemon may be running.

## Usage

### Mount your project with agentvfs

Pick stable paths for the mount and control socket:

```bash
export AGENTVFS_MOUNT=/tmp/agentvfs-mnt
export AGENTVFS_SOCK=/tmp/pi-agentvfs.sock

agentvfs \
  --source ~/workspace/ContextFS/ \
  --mountpoint "$AGENTVFS_MOUNT" \
  --control-sock "$AGENTVFS_SOCK" \
  --telemetry=none
```

Run the daemon in the foreground while testing. If you start it in another
terminal or background it with `&`, keep the same `AGENTVFS_SOCK` value for Pi.

### Install the extension in Pi

```bash
pi install github.com/thustorage/ContextFS/extensions/pi
```

Start Pi from inside the AgentVFS mount and pass the socket explicitly:

```bash
cd "$AGENTVFS_MOUNT"
AGENTVFS_SOCK="$AGENTVFS_SOCK" pi
```

Or during extension development:

```bash
cd "$AGENTVFS_MOUNT"
AGENTVFS_SOCK="$AGENTVFS_SOCK" \
  pi -e ~/workspace/ContextFS/extensions/pi/src/index.ts
```

Pi must run with its `cwd` inside the agentvfs mount. Running Pi from the original `--source` directory bypasses FUSE, so `agentvfs-ctl rollback` cannot restore those files.

### Pi commands

Use Pi's native tree view for normal time travel and workspace rollback:

```text
/tree
```

When you select a previous node in `/tree`, the extension restores the
workspace to the nearest matching AgentVFS checkpoint before Pi switches to
that conversation node. In normal use, `/tree` is the rewind UI: it rolls back
both the conversation branch and the files together.

For direct AgentVFS operations:

```text
/agentvfs status
/agentvfs checkpoint before-refactor
/agentvfs rewind
/agentvfs tree off
/agentvfs tree on
```

`/rewind` remains available as an alias for direct filesystem rewind when you do
not want to navigate the Pi conversation tree.

### Manual checkpoint / rollback

The extension handles checkpoints automatically, but you can also drive the daemon directly:

```bash
# Create a checkpoint
agentvfs-ctl --sock "$AGENTVFS_SOCK" checkpoint before-refactor

# Roll back
agentvfs-ctl --sock "$AGENTVFS_SOCK" rollback before-refactor

# View status
agentvfs-ctl --sock "$AGENTVFS_SOCK" status
```

## Architecture

```
┌─────────────┐    tool events     ┌──────────────────┐
│  Pi agent   │ ─────────────────► │ pi-agentvfs-     │
│  (extension)│                    │ rewind           │
└─────────────┘                    └────────┬─────────┘
       ▲                                    │ checkpoint / rollback
       │ select & restore                   │ via Unix socket
       └────────────────────────────────────┘
                                    ┌────────┴─────────┐
                                    │  agentvfs daemon  │
                                    │  (FUSE mount)     │
                                    └───────────────────┘
```

## Files

| File | Purpose |
|------|---------|
| `src/index.ts` | Extension entry point — event listeners |
| `src/core.ts` | `agentvfs-ctl` wrapper, socket discovery |
| `src/state.ts` | Shared mutable state |
| `src/commands.ts` | `/rewind`, `/agentvfs`, and fork/tree workspace alignment handlers |
| `src/ui.ts` | Footer status updates |

## License

Apache-2.0 — see the top-level [LICENSE](../LICENSE) in the ContextFS repository.
