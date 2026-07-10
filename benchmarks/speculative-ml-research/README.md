# Speculative ML-Research Benchmark on AgentVFS

This benchmark implements the **speculative actions** pattern from
[*Speculative Actions* (arXiv:2510.04371)](https://arxiv.org/abs/2510.04371) on
a real filesystem-mutating agent workload, and uses that workload to
independently verify and evaluate AgentVFS's checkpoint, branch, merge, and
rollback verbs. The agent workload is Karpathy's
[`autoresearch`](./vendor/autoresearch/) ML-experimentation loop: an Actor
model repeatedly edits `train.py`, runs a fixed-budget CPU training run, reads
the resulting `val_bpb`, and keeps or discards the experiment. A
**Speculator** (the Actor's model with a speed-optimized prompt, per the
paper's chess configuration) predicts k candidate edits as compact anchored
SEARCH/REPLACE blocks *during* the Actor's deliberation latency; each
appliable candidate is pre-launched on its own AgentVFS branch inside a
cgroup-routed session (the paper's k-way parallel pre-launch). If the Actor
returns before the Speculator, the window is missed and no pre-launch happens
(cancel guard — speculation can never sit on the critical path). When the
Actor's real decision arrives the harness validates each applied candidate by
normalized diff. A hit merges that candidate's branch into `main` (keeping the
in-flight training's head start) and kills the sibling branches; a miss kills
the process groups and deletes the branches. The committed trajectory is
identical to sequential execution — **lossless**.

AgentVFS is the system under test. Because every speculative action lives on an
isolated branch, the speculate-verify-commit/discard loop is a demanding,
semantically meaningful workload that exercises every AgentVFS verb
(`checkpoint`, `rollback`, `branch create/merge/delete`, cgroup session routing)
with explicit correctness postconditions enforced by a shadow model
([`src/verifier.py`](./src/verifier.py)) and per-verb latency capture
([`src/agentvfs.py`](./src/agentvfs.py)). The design spec, including the
metrics, comparison axes, and the full speculate-verify-commit protocol, lives
at
[`docs/superpowers/specs/2026-07-07-speculative-ml-research-design.md`](../../docs/superpowers/specs/2026-07-07-speculative-ml-research-design.md).

## Prerequisites

- **Built AgentVFS binaries**: [`../../build/agentvfs`](../../build/agentvfs)
  and [`../../build/agentvfs-ctl`](../../build/agentvfs-ctl). From the repo
  root: `cmake -B build && cmake --build build -j`.
- **`fusermount3`** on `PATH` (e.g. `fuse3` package). The daemon is mounted with
  it and unmounted on cleanup.
- **cgroup v2 write access.** `main.py`'s `preconditions()` creates a probe
  cgroup (`CgroupSession("sml-probe")`) and aborts with
  `cgroup v2 unavailable ...` if it cannot. Run either **as root**, or set
  `SML_CGROUP_ROOT` to a delegated cgroup directory your user can create
  subdirectories in (e.g. a systemd user-slice delegation). The branch-routing
  precondition additionally writes a probe file on a probe branch and confirms
  `main` does not see it.
- **Python dependencies, including CPU torch** (training is CPU-only on this
  host): from this directory run `uv sync --extra train`. This installs torch
  from the CPU wheel index configured in [`pyproject.toml`](./pyproject.toml).
- **One-time data preparation (network required).** `prepare.py` (vendored at
  [`./vendor/autoresearch/prepare.py`](./vendor/autoresearch/prepare.py))
  downloads and tokenizes the training dataset into `~/.cache/autoresearch/`.
  Run it once before any real training: `uv run python vendor/autoresearch/prepare.py`.
  After this, all training runs reuse the prepared data offline.
- **API endpoints for the Actor and Speculator** (any OpenAI-compatible
  chat-completions endpoint), supplied via environment variables referenced by
  [`config.yml`](./config.yml):

  | Variable         | Used by       | Meaning                                              |
  |------------------|---------------|------------------------------------------------------|
  | `ACTOR_BASE_URL` | `actor`       | OpenAI-compatible base URL for the strong Actor model |
  | `ACTOR_API_KEY`  | `actor`       | API key for the Actor endpoint                        |
  | `SPEC_BASE_URL`  | `speculator`  | OpenAI-compatible base URL for the fast Speculator    |
  | `SPEC_API_KEY`   | `speculator`  | API key for the Speculator endpoint                   |

  `main.py` aborts at startup if any of the four is unset.

## Run commands

All commands run from this directory (`benchmarks/speculative-ml-research/`).

```bash
uv run pytest                  # fast tests (default: -m 'not slow'). CI-grade.
uv run pytest -m slow          # the real 30s CPU training gate (needs torch)
./run.sh                       # full comparison: regular-native, regular, spec
```

`run.sh` first runs the fast suite (must stay green), then executes all three
modes in sequence using [`config.yml`](./config.yml). Each mode writes a
timestamped directory under [`./results/`](./results/). Run modes individually:

```bash
uv run python main.py --mode regular-native --config config.yml
uv run python main.py --mode regular        --config config.yml
uv run python main.py --mode spec           --config config.yml
```

`--mode` is one of `regular` (sequential AgentVFS), `regular-native`
(sequential on a plain directory — FUSE overhead baseline), or `spec`
(speculative AgentVFS + replay/losslessness check). Replay is internal to
`spec` mode, not a CLI mode.

## Output schema

Each run writes a timestamped directory under `./results/<mode>-<YYYYMMDD>-<HHMMSS>/`
containing four files (plus, for `spec`, `committed.json` and `lossless.json`):

- **`perstep.csv`** — one row per Actor step:
  - `step` — 0-indexed step number
  - `actor_latency_s`, `spec_latency_s` — wall time of the Actor and Speculator calls
  - `predicted_hit` — any-of-k match: one of the Speculator's applied guesses matched the Actor's edit (normalized diff)
  - `prelaunched_hit` — a matching guess was pre-launched on a branch and its training was committed (the case that actually saves time; keep and discard decisions both qualify — the guess is a complete file, so a discard-step hit merges the branch in place of the rollback)
  - `hit_idx` — index of the matching guess among the launched candidates (empty when no match)
  - `window_missed` — the Actor returned before the Speculator; the step ran without any pre-launch
  - `spec_apply_failures` — candidates dropped because a SEARCH anchor did not match the current train.py
  - `head_start_s` — training wall-clock already banked on the winning branch when the Actor's decision arrived (hits only)
  - `train_wall_s` — training SUBPROCESS wall (launch→collect): startup, data load, eval and log I/O included, unlike the trainer's self-reported `train_time_s`; the FUSE data-path tax lives in the difference
  - `train_time_s` — wall time of that step's training run
  - `val_bpb` — parsed validation bits-per-byte metric (lower is better)
  - `decision` — `keep` or `discard` (regression → `rollback` to best checkpoint)
- **`verbs.csv`** — one row per AgentVFS control verb:
  - `verb` — `checkpoint | rollback | branch_create | merge | delete`
  - `step` — step index the verb was issued at (`-1` for setup)
  - `latency_us` — per-verb latency in microseconds
  - `status` — `ok` or `error`
  - `store_bytes_after` — content-addressed store size in bytes right after the verb (storage growth signal)
- **`totals.csv`** — single header row + single value row, aggregating the run:
  `wall_s, n_steps, predicted_hits, prelaunched_hits, accuracy_any,
  peak_store_bytes, verb_failures, windows_missed, spec_apply_failures,
  train_wall_total_s, mode, orphaned_branches (spec only),
  actor_tokens_in/out, spec_tokens_in/out (spec only), verify_failures`.
  `config.yml`'s `k` sets how many candidates are predicted (accuracy is
  scored over all of them); `k_launch` caps how many are pre-launched
  (selective branch launching, paper §5).
- **`summary.md`** — human-readable rendering of the totals dict.

`spec` runs additionally write `committed.json` (the list of committed
`train.py` contents, in order) and `lossless.json`
(`{content_match, trajectory_match, replay_trajectory, speculative_trajectory}`)
produced by the sequential replay check.

## State layers (--trainer warm, agent-state journal)

On top of the fs/session verbs, the benchmark now exercises AgentVFS's
**cooperative process snapshots** and **agent-state journal**. The design spec
lives at
[`docs/superpowers/specs/2026-07-08-spec-ml-state-layers-design.md`](../../docs/superpowers/specs/2026-07-08-spec-ml-state-layers-design.md).

### Warm cooperative trainer (`--trainer warm`)

```bash
uv run python main.py --mode regular --trainer warm --config config.yml
uv run python main.py --mode spec    --trainer warm --config config.yml
```

`--trainer warm` (Linux-only; **incompatible with `--mode regular-native`**)
forks a warm template once — a process parked at a quiescence boundary right
after the torch import — and restores a generation from that template per
experiment instead of launching a fresh process each step. The boundary hook is
dlopen'd from [`../../build/libagentvfs_runtime_client.so`](../../build/libagentvfs_runtime_client.so)
(via [`src/runtime_boundary.py`](./src/runtime_boundary.py)); the driver is
[`src/warm_trainer.py`](./src/warm_trainer.py) and the harness is
[`src/warm.py`](./src/warm.py). Each step's restore is shadow-verified against
the warm template tree (coupled fs + process rollback). Requires the extra
build artifact `build/libagentvfs_runtime_client.so` (built by `cmake --build
build -j`) and `build/agentvfs-run` in addition to the prerequisites above.

### Run matrix

The two main-path trainers compose with both runner modes:

|              | `regular` (sequential) | `spec` (speculative) |
|--------------|------------------------|----------------------|
| `--trainer fresh` (default) | per-step process launch | per-step launch + speculative pre-launch |
| `--trainer warm` | restore-per-experiment from a warm template | warm main path + fresh speculative pre-launch |

### Agent-state chain + resume check

Every step is recorded in an agent-state chain
([`StateChain`](./src/agentvfs.py)): a logical-only root, then `--sync`-anchored
descendants (`--parent` + `--snapshot-base`) that publish the `state latest`
ref. `Verifier.check_agent_chain` shadow-verifies the chain by walking the CAS
parent chain (or describing the lone root). After the run, `resume_check` runs
`state restore --mode full` on the best-`val_bpb` step's record and asserts the
fs rolls back to that step's checkpoint tree — closing the loop on agent-state
rollback.

New outputs and totals keys:

- **`resume.json`** — `{ok, step, state_id}` from the resume check (the restore
  target is the best-`val_bpb` step). A failed check aborts the run before the
  file is written, so a present `resume.json` always has `ok: true`.
- `totals.csv` gains `agent_state_records` (chain length), and (warm runs)
  `restores`, `mean_restore_ms`, and `startup_saved_s` (the warm speedup
  estimate: `max(0, startup_baseline_s − mean_restore_ms/1000)` per
  experiment restore; recovery restores after a timeout are excluded).

### Verification scope

The state-layer tests cover, across the verb surface:

| Layer | Verb / behavior | Coverage |
|---|---|---|
| process | runtime snapshot (template) | toy + warm tests |
| process | runtime restore (coupled fs + process) | per-experiment warm + toy tests |
| process | runtime status / drop, restore-failure paths | no-root tests |
| agent | state append / describe / latest | benchmark chain, shadow-verified |
| agent | state restore (session / full / runtime) | resume check + tests |

**Accepted gaps** (decided, not deferred): branch-create and miss-path branch
postconditions have no dedicated check. The hit path's `check_branch` hashes
the speculative branch's full content (inside the cgroup) before any merge, so
a wrong fork can never land on main — branch-create is verified implicitly by
the only path that consumes the branch. On a miss (and for a hit's losing
sibling branches) branches are killed and deleted unread; checking them would
add an O(tree-hash) stall inside the speculation window for trees that
influence nothing.

**Deferred** (documented follow-ups, not covered here): per-step process
snapshots with restore-to-best on discard (true process checkpoint/rollback
symmetry); non-default `--boundary` kinds, `state append --snapshot-base` delta
records, and `policy install`; the fs-postcondition narrowing from the original
benchmark review.

## Interpreting results

There are **two independent comparison axes** (see the
[design spec](../../docs/superpowers/specs/2026-07-07-speculative-ml-research-design.md),
"Metrics"):

1. **Speculative vs. sequential (the paper's axis).** Compare `runner_spec.py`
   against `runner_regular.py` at the same seeds and models. Yields prediction
   accuracy (`accuracy_any` / `prelaunched_hits`) and time saved
   `(T_seq − T_spec) / T_seq`, computed from `wall_s` in `totals.csv`.
2. **AgentVFS vs. native filesystem (the systems axis).** Compare
   `regular` against `regular-native`. This isolates the steady-state FUSE
   overhead of the AgentVFS mount on the training hot path from the
   per-branch/verb control cost (in `verbs.csv`). Success criterion: branch and
   checkpoint verbs are orders of magnitude cheaper than the speculation window
   (µs–ms vs. tens of seconds of Actor deliberation).

Correctness invariants to check after every run:

- **`verify_failures` must be `0`.** A non-zero count means a shadow-model
  postcondition failed (branch isolation, merge content-equivalence, rollback
  restoration). The run aborts on the first failure and dumps an expected-vs-
  actual diff plus `agentvfs-ctl status` / `branch list` into the results dir.
- **`orphaned_branches` is storage leakage, not a correctness failure.** It
  counts branches whose `branch delete` errored after a miss cleanup (the
  process group is always killed first). Losslessness only requires the
  *committed main trajectory* to match sequential replay — see `lossless.json`:
  `content_match` is AgentVFS's responsibility (filesystem-level losslessness);
  `trajectory_match` additionally checks bitwise `val_bpb` equality (expected on
  CPU with fixed seeds, but metric drift is environment noise, not an AgentVFS
  defect).
