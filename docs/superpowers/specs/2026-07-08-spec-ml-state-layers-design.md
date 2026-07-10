# Spec-ML State Layers: Agent State + Warm Cooperative Trainer Design

**Date:** 2026-07-08
**Status:** Approved for planning
**Scope:** Extend `benchmarks/speculative-ml-research/` to exercise and verify
AgentVFS's two remaining state layers — the agent-state journal and cooperative
runtime (process) snapshots — alongside the filesystem layer it already covers.

## Context

The spec-ml benchmark validates AgentVFS checkpoint/branch/merge/rollback and
cgroup session routing under a real ML-research workload (20-step runs
completed 2026-07-08 with `verify_failures=0`). It does not touch the two
features this branch (`process-criu`) exists for:

1. **Agent-state journal** (`state append/describe/latest/restore`): CAS-native
   semantic continuation records, linked to `fs_commit` and optionally to a
   union runtime state. Pure control-socket API; no process cooperation needed.
2. **Live runtime snapshots** (`runtime snapshot/restore/status`): cooperative,
   non-CRIU fork-template process state. The target must be launched via
   `agentvfs-run` and call `agentvfs_runtime_boundary()` at quiescence points.

Two implementation facts constrain the design (verified in
`src/cas/daemon.cpp:restore_runtime`):

- **No branch retargeting.** Restore resumes the runtime on the branch recorded
  at snapshot time and rolls *that branch's files* back to the snapshot's
  `fs_commit` — a coupled process+filesystem rollback.
- **Restore wipes post-snapshot edits** on that branch. Any per-experiment edit
  must therefore be applied *after* the restore, not before.

Benchmark measurements motivating the process half: each fresh trainer launch
pays ~17 s of Python+torch startup per step (~340 s across a 20-step run), and
a fresh process per experiment is the only thing preventing in-process
contamination from failed experiments.

## Goals

1. Exercise the agent-state journal in the benchmark loop, verified with the
   same shadow-model philosophy as the filesystem layer (ground truth kept by
   the harness, byte-compared against what AgentVFS returns).
2. Add a warm cooperative trainer path that replaces per-step process launch
   with fork-template restore, delivering both correctness coverage (restore +
   union-state + agent-state linkage) and a measurable perf story
   (startup amortization; pristine warm memory per experiment).
3. Keep the comparison matrix clean: the trainer axis is orthogonal to the
   mode axis — `{regular, spec} × {fresh, warm}`, with `regular-native`
   remaining fresh-only (no daemon).
4. All new correctness tests runnable without root where the daemon allows it
   (`agentvfs-run` needs the control socket, not cgroups).

## Non-Goals

- No warm trainers on speculative branches (no branch retargeting in the
  daemon's first slice; a per-branch template would need a per-branch warm-up,
  defeating the purpose). Speculative pre-launches keep the fresh path.
- No mid-training snapshots (the implemented slice requires explicit
  quiescence boundaries).
- No dataset preloading across experiments beyond what interpreter warmth
  gives: `train.py` still loads its own data inside the experiment. The warm
  win is interpreter+torch startup, quantified honestly.
- No changes to the replay check (it remains a plain-directory, fresh-process
  sequential replay — that is its point).

## Design

### 1. Warm cooperative trainer

New driver `benchmarks/speculative-ml-research/src/warm_trainer.py`, launched
once per run by the harness:

```
agentvfs-run --sock <sock> --branch main -- <python> src/warm_trainer.py
```

with `cwd` on the mount. Driver lifecycle:

1. **Warm-up:** import torch (and anything else import-heavy). This is the
   state the template preserves.
2. **Boundary-offering loop:** while no go-marker exists, repeatedly call
   `agentvfs_runtime_boundary()` (ctypes into the runtime client library)
   with a short sleep. A boundary with no pending snapshot returns
   `continue` immediately and costs one socket round-trip, so the offers are
   cheap — and they make the snapshot rendezvous race-free: whenever the
   harness's blocking `runtime snapshot` request lands, it latches the next
   offered boundary deterministically. Once the go-marker appears: read
   `train.py`, execute it in-process (`runpy.run_path`, stdout redirected to
   a metric file so `parse_train_output` works unchanged), loop.

The harness waits for the runtime to register (poll `runtime status`), then
snapshots once (`runtime snapshot <id> --agent-state <state-id>`, a blocking
call that rendezvouses with an offered boundary), producing the union state
used by every subsequent restore. The launch environment must carry
`TRAIN_BUDGET_S` (it is baked into the template) alongside
`SML_WARM_IMPORT`, `PYTHONDONTWRITEBYTECODE=1`, and `OMP_NUM_THREADS`.

**Per-experiment cycle (harness side):**

```
runtime restore <union-id>     # process forks from warm template;
                               # branch files roll back to warm commit
apply_edit(candidate)          # complete train.py, exact bytes
write go-marker                # driver proceeds
poll metric file (timeout_s)   # driver runs experiment in-process
```

Marker and metric files live in a dot-directory on the mount
(`.sml-run/`). They are volatile artifacts: untracked by the shadow (so
`check_main` ignores them, like trainer logs today) and wiped by the next
restore — self-cleaning.

**Failure handling:** on metric timeout or unparseable output, the harness
issues another `runtime restore` (freezing the runaway generation) and records
the step as a failed experiment — identical tolerance semantics to the fresh
path. If restore itself fails (template death, daemon error), that is an
infrastructure failure: fail closed, as today.

### 2. Harness integration

- `main.py --trainer fresh|warm` (default `fresh`; rejected with `--mode
  regular-native`).
- Runners accept a `warm=None` kwarg carrying a `WarmTrainer` harness
  instance (`restore(step)` / `run_experiment(step, timeout_s) ->
  TrainResult`); `RunConfig` gains `startup_baseline_s`. The class owns the
  `agentvfs-run` subprocess, registration wait + snapshot rendezvous, the
  per-experiment cycle, and teardown — teardown must kill the ACTIVE
  generation's process group (`runtime status` →
  `active_process_group_id` → `killpg`) before dropping the template, since
  restored generations live in their own process groups and would otherwise
  outlive the run and EBUSY the unmount.
- `AgentVFS` wrapper gains `runtime_snapshot`, `runtime_restore`,
  `runtime_status`, `state_append`, `state_latest`, `state_describe` — all
  recorded as verbs so their latencies land in `verbs.csv`.

**Step order in warm mode** (the subtle part, order is load-bearing):

```
read train.py (actor prompt input)   # mount holds last step's outcome
actor decides
if discard: avfs.rollback(best) + check_rollback + resync   # unchanged
runtime restore                       # mount resets to warm/base commit
apply_edit(new candidate)             # complete file, so base reset is harmless
train via go-marker/metric cycle
checkpoint + check_main + best-tracking                     # unchanged
```

The actor always reads the mount *before* the restore, and every experiment
writes a complete `train.py`, so the restore's rollback-to-base never loses
information. Spec mode: only the sequential/miss-path training switches to
warm; branch pre-launches, replay, and native mode are untouched.

### 3. Agent-state chain

Always on in agentvfs modes, both trainer types. The journal's durability
contract (verified in `agent_state_service.cpp`): a synced append requires
BOTH `--parent` and `--snapshot-base` anchors referencing readable records,
and only synced appends publish the `state latest` ref. The chain therefore
opens with a logical-only ROOT record (no `--sync`; describable but not
"latest"), and every subsequent record is durable:

```
# first record of the run (root):
state append --agent sml-actor --kind session --schema sml.step.v1 \
  --payload '{...step 0 payload...}' [--fs-commit <ckpt>]
# every later record:
state append --agent sml-actor --kind session --schema sml.step.v1 \
  --payload '{"step":N,"val_bpb":...,"decision":"keep|discard|failed",
              "train_py_sha":"..."}' \
  [--fs-commit <ckpt>] --parent <previous-id> --snapshot-base <root-id> --sync
```

(`kind` must be one of the daemon's enumerated kinds — `session` here; the
record JSON returns the payload under `payload_inline`, nested under a
`"state"` wrapper, which the harness wrapper normalizes.)

Failed and discarded steps append too — the journal is semantic history, not
a mirror of the file tree. Failed steps have no checkpoint of their own, so
they append **without** `--fs-commit`; successful steps always link theirs.
The warm snapshot passes `--agent-state` so the union object records all
three layers (fs commit + process template + agent state).

### 4. Verification

Extends `Verifier` with the same fail-closed, dump-on-mismatch machinery:

- **`check_agent_chain(label, expected_history)`**: walk the parent chain from
  `state latest`, decode payloads, compare the reconstructed sequence
  byte-for-byte against the harness's in-memory history. Run after each append
  (cheap: the chain is in CAS) or at minimum at end of run and after each
  rollback.
- **Union linkage check** after the warm snapshot: the snapshot response's
  `fs_commit` is verified against the mount tree. The `agent_state_id` half
  is write-only (no daemon verb reads a union state back); it is exercised
  instead through `state restore --mode runtime` on a record appended with
  `--union-state <union-id>`, which resolves the union and restores the
  process — covered in the cooperative tests.
- **Warm-restore postconditions** per experiment: mount tree hash equals the
  warm-snapshot tree (full-tree compare — a strong coupled-rollback check),
  runtime generation increments, the prior generation's process group is gone
  (`runtime status`).
- **Resume check (`state restore`)** at end of run: pick the best step's
  state-id from the chain, run `state restore <id> --mode full`, and verify
  (a) the mount tree hash equals that step's checkpoint tree and (b) the
  returned payload reconstructs the harness history up to that step. This
  exercises agent-state *rollback*, not just journaling. `--mode session`
  round-trip is asserted in tests; `--mode runtime` joins the union and is
  exercised in the warm toy tests.

### 5. Metrics

- `verbs.csv` gains `runtime_snapshot`, `runtime_restore`, `state_append`
  rows (latency per call).
- `totals.csv` gains: `restores`, `mean_restore_ms`, `startup_saved_s`, and
  `agent_state_records`. The startup baseline behind `startup_saved_s` is
  measured once at run start by timing one throwaway fresh launch that
  imports torch and exits immediately (same interpreter, same cwd); saved
  time = that baseline minus mean restore latency, × warm experiments.
- The paper matrix: `{regular, spec} × {fresh, warm}` wall-clock comparison,
  plus restore-latency distribution vs the ~17 s fresh startup it replaces.

### 6. Testing

- **No-root (daemon + FUSE):**
  - toy cooperative driver (tiny script, no torch) — snapshot/restore
    round-trip, generation increment, coupled fs rollback, post-restore
    write visibility;
  - runtime lifecycle edges: `runtime drop` disposes the template and a
    subsequent restore fails (typically "unknown template" — the dropped
    record is reaped when the parked template consumes the drop); restore of
    an unknown union-state id errors cleanly;
  - agent-state chain round-trip against a scripted history, including a
    failed step; `state restore --mode session` and `--mode full`
    round-trips;
  - branch-scoped fs verbs: `checkpoint --branch` / `rollback --branch` on a
    non-main branch, verified against the branch view (the wrapper regains
    these kwargs — this time with callers);
  - warm-trainer cycle end-to-end with the stub trainer executed in-process
    by the driver (go-marker/metric protocol, restore-per-experiment).
- **cgroup-gated:** spec+warm interplay — warm main-path trainer coexisting
  with a fresh speculative branch pre-launch in a cgroup.
- **Slow/real:** one warm real-training experiment cycle (torch, 30 s budget).
- The suite keeps the existing SKIP-vs-abort discipline.

## Coverage map (after this spec)

What the benchmark + tests verify across the agentvfs verb surface:

| Layer | Verb / behavior | Coverage |
|---|---|---|
| fs | checkpoint / rollback (main) | benchmark loop, shadow-verified |
| fs | checkpoint / rollback (`--branch`) | no-root tests (new) |
| fs | branch create / merge / delete | benchmark spec mode |
| session | register / unregister, cgroup routing | benchmark + gated tests |
| process | runtime snapshot (template) | warm run, once + toy tests |
| process | runtime restore (coupled fs+process) | warm run per experiment + toy tests |
| process | runtime status / list | readiness + postconditions |
| process | runtime drop, restore-failure paths | no-root tests (new) |
| agent | state append / describe / latest | benchmark chain, shadow-verified |
| agent | state restore (session / full / runtime) | resume check + tests (new) |

**Explicitly deferred** (documented follow-ups, not covered here):

- Per-step process snapshots with restore-to-best on discard (true process
  checkpoint/rollback symmetry; needs live-template memory-cost measurement
  and `runtime drop` in the loop) — natural next iteration of the warm
  runner.
- Non-default `--boundary` kinds; `state append --snapshot-base` delta
  records; `policy install`.
- The fs postcondition narrowing from the original benchmark review
  (branch-create view check, miss-path branch check, rollback
  fd-invalidation/ESTALE) — tracked separately.

## Risks / open items for the plan

1. **Boundary hook ABI:** identify the built artifact exporting
   `agentvfs_runtime_boundary()` and the token/env handshake `agentvfs-run`
   passes to the child. If only a static library exists, build a thin shared
   shim. This is the plan's first task; everything else layers on it.
2. **Template readiness signal:** how the harness knows the warm-up boundary
   was reached (poll `runtime status` / `runtime list`).
3. **`state append` payload size limits** for the per-step payload (expected
   tiny; verify).
4. **Restore latency** is assumed ≪ fresh startup; if a restore costs seconds,
   the perf story shrinks — measure early with the toy driver.
