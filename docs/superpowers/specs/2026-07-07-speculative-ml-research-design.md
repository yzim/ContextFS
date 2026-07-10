# Speculative Actions On AgentVFS: ML-Research Environment Design

**Date:** 2026-07-07
**Status:** Approved design, pending implementation plan
**Scope:** A real-agent speculative-actions workload (per
`docs/SpeculativeActions.pdf`, arXiv:2510.04371) that doubles as a correctness
and performance test of AgentVFS checkpoint, branch, merge, and rollback.

## Purpose

Two goals, in priority order:

1. **Verify and evaluate AgentVFS.** Use the speculate-verify-commit/discard
   loop as a demanding, semantically meaningful workload that exercises every
   AgentVFS verb (`checkpoint`, `rollback`, `branch create/merge/delete`,
   cgroup session routing) with explicit correctness postconditions and
   per-verb latency measurement.
2. **Demonstrate speculative actions on a real agent workload.** Implement the
   paper's Algorithm 1 (Actor/Speculator, k-way single-step speculation,
   validate-then-commit) on the autoresearch ML-experimentation loop, with
   AgentVFS branches as the safety envelope that makes filesystem-mutating
   speculation lossless. Produce paper-comparable metrics (prediction
   accuracy, time saved, cost).

## Context

- `thirdparty/speculative-action/` — the paper's reference implementation.
  Four self-contained Python environments (chess, e-commerce, hotpotqa,
  os-tuning), each with a regular-vs-speculative workflow comparison. None of
  them has filesystem side effects; this design adds the missing environment
  where speculation needs a filesystem safety envelope.
- `thirdparty/autoresearch/` — Karpathy's autonomous ML-research loop: an
  agent edits `train.py`, runs a fixed-budget training run, reads `val_bpb`,
  keeps or discards, repeats. Closed world (no network side effects during
  actions), which makes the paper's Assumption 2 (reversible pre-launch)
  fully satisfiable with AgentVFS alone.
- `agentvfs-ctl` on the `process-criu` branch — provides `checkpoint`,
  `rollback`, `branch create/delete/list/merge`, and Linux cgroup-based
  branch routing via `session register --cgroup <p> --branch <name>`.
- `benchmarks/agent-sim/` — the existing benchmark harness whose CSV and
  results-directory conventions this design follows.

Constraints confirmed with the user:

- No GPU. Training runs CPU-only, scaled down per autoresearch's own
  small-platform guidance.
- Actor and Speculator are third-party models reached via OpenAI-compatible
  HTTP APIs (keys via environment variables).
- The clones in `thirdparty/` are not modified — and `thirdparty/` is
  git-ignored, so the harness must not depend on it at runtime or test time.
  The autoresearch files the harness needs (`train.py`, `prepare.py`,
  `program.md`, `pyproject.toml`) are vendored, unmodified and with MIT
  attribution, into `benchmarks/speculative-ml-research/vendor/autoresearch/`;
  everything seeds from that committed copy.

## Cast (mapping to the paper)

| Paper role | Instantiation |
|---|---|
| Environment / state `s_t` | autoresearch working tree (`train.py`) + experiment history (`val_bpb` per run) |
| Action `a_t` | "next experiment": an edit to `train.py` followed by a training run |
| Actor (slow, authoritative) | strong third-party model, high reasoning effort, decides the next experiment from history |
| Speculator (fast) | small/fast third-party model, predicts the Actor's next edit during Actor deliberation |
| Safety envelope | an AgentVFS branch isolates each pre-launched speculative action (v1: one per step); the speculative training process runs in a cgroup routed to that branch |
| Commit (prediction hit) | let the in-flight speculative training finish on its branch, then `branch merge spec-i --into main` and delete the branch; time saved = the head start |
| Discard (miss) | kill the speculative training process group, then `branch delete spec-i` |

**Speculation window.** After each training run completes, the Actor spends
its deliberation latency (tens of seconds to minutes) deciding the next edit.
The CPU is idle during that window. The Speculator predicts the next edit in
seconds, applies it on a fresh branch, and pre-launches training there. When
the Actor's real decision arrives, validation compares the speculated edit
with the actual edit (normalized diff match). Hit: merge, keep the in-flight
training. Miss: kill, delete, execute the real edit on main. The committed
trajectory is identical to sequential execution — lossless.

## Architecture

Location: `benchmarks/speculative-ml-research/` (keeps `thirdparty/` clones
pristine; follows the agent-sim precedent).

```
benchmarks/speculative-ml-research/
├── README.md             # setup, env vars, run commands
├── config.yml            # model ids, endpoint env-var names, k, TRAIN_BUDGET_S, paths
├── vendor/autoresearch/  # committed copy of the four autoresearch files (MIT, attributed)
├── src/
│   ├── environment.py    # autoresearch loop: apply edit, launch/await training, parse val_bpb
│   ├── agentvfs.py       # subprocess wrappers over agentvfs-ctl + cgroup session routing
│   ├── verifier.py       # shadow model + per-verb postcondition checks
│   ├── llm_client.py     # Actor + Speculator via OpenAI-compatible APIs
│   ├── runner_regular.py # sequential baseline (no speculation)
│   ├── runner_spec.py    # k-way speculative runner (Algorithm 1)
│   └── metrics.py        # accuracy, time saved, losslessness check, CSV + summary.md
└── results/<run-id>/     # env.txt, perstep.csv, verbs.csv, totals.csv, summary.md, logs
```

Component boundaries:

- `environment.py` knows nothing about AgentVFS or LLMs: it applies a given
  edit to a given directory, runs training with a wall-clock budget, and
  returns the parsed metric.
- `agentvfs.py` knows nothing about ML or LLMs: it wraps `agentvfs-ctl`
  (`--json`), creates a child cgroup for a command, registers it to a branch,
  and reports per-verb latencies.
- `verifier.py` owns correctness: it maintains the shadow model (expected
  file state per branch, derived from the edits the harness itself applied)
  and asserts postconditions after every verb.
- Runners compose the three; `llm_client.py` isolates provider access.

### CPU-scaled training

The harness seeds the AgentVFS source tree from its committed
`vendor/autoresearch/` copy, then applies a CPU overlay of exact anchored
string replacements, which covers:

- device selection changed from CUDA to CPU;
- autoresearch's own small-platform knobs: reduced `DEPTH`, `MAX_SEQ_LEN`,
  `EVAL_TOKENS`, `TOTAL_BATCH_SIZE`;
- a wall-clock training budget `TRAIN_BUDGET_S` (default 60–90 s) replacing
  the 5-minute default;
- fixed RNG seeds so identical edits produce identical `val_bpb` on the same
  host (CPU training is deterministic; this makes the losslessness check
  exact).

The overlay is a set of files copied over the seeded tree (not patches to
`thirdparty/`). Python dependencies (CPU torch) are installed once into a
venv outside the mount and reused by every training run.

### Speculation protocol (v1 decisions)

- **k guesses, one pre-launch slot.** The Speculator returns up to k
  candidate edits ordered by confidence. Only the top-confidence candidate
  gets a branch + pre-launched training run; the remaining candidates are
  retained for validation by diff only (a hit on candidate 2..k still counts
  for accuracy metrics and still merges the branch if — and only if — that
  candidate was the one pre-launched; otherwise it is a "predicted but not
  pre-launched" hit, recorded separately). Rationale: one CPU means a second
  concurrent training run would slow the committed path and break the
  lossless guarantee.
- **Validation** is a normalized-diff match: both the speculated and actual
  edits are applied to identical scratch copies of the current tree, and the
  resulting `train.py` contents are compared after whitespace/comment
  normalization. Exact-content match after normalization = hit.
- **Speculation is best-effort.** Any Speculator error, timeout, or malformed
  edit degrades that step to sequential execution. The Actor path is never
  blocked by speculation.
- **Discard-regression path uses rollback.** When the Actor decides an
  experiment regressed, the harness restores the best-so-far state with
  `agentvfs-ctl rollback <best-checkpoint>` on main — not by rewriting files.
  This puts `rollback` in the natural loop so every verb is exercised with
  workload semantics.

### Correctness layer (shadow model)

`verifier.py` maintains, per branch, the expected content of every file the
harness has touched (it applied the edits, so it knows). Postconditions:

| Verb / event | Postcondition |
|---|---|
| `branch create spec-i` | spec-i's view is content-identical to the parent commit's tree |
| speculative execution on spec-i | isolation: main's tree is untouched while spec-i mutates (checked during and after the speculative run) |
| `branch merge` (hit) | main's tree is content-identical to spec-i's tree; training artifacts present |
| `branch delete` (miss) | main unchanged; the deleted branch's effects unreachable |
| `checkpoint` + later `rollback` | tree after rollback is content-identical to the checkpointed state; open file handles on rolled-back files are invalidated per AgentVFS semantics |

Verification method: file walk + content hash compared against the shadow
model, scoped to the paths the workload touches (`train.py`, logs, artifact
dirs) plus a spot-check of untouched paths. A postcondition violation aborts
the run with exit code ≠ 0 and dumps expected-vs-actual diff, `agentvfs-ctl
status`, and `branch list` output into the results directory.

### Losslessness check

After a speculative run, replay the committed action sequence sequentially
from the same seed and base tree, and assert:

1. final `train.py` content hash matches the speculative run's main branch;
2. the `val_bpb` trajectory matches bitwise (expected on CPU with fixed
   seeds).

If (2) fails while (1) holds, report metric drift separately as environment
noise — filesystem-level losslessness (AgentVFS's responsibility) is claim
(1).

## Metrics

Line-oriented CSVs in the agent-sim style, one results directory per run.

- `perstep.csv`:
  `step, actor_latency_s, spec_latency_s, predicted_hit, prelaunched_hit,
  train_time_s, val_bpb, decision(keep|discard)`
- `verbs.csv`:
  `verb(checkpoint|rollback|branch_create|merge|delete), step, latency_us,
  status, store_bytes_after`
- `totals.csv`: wall time, prediction accuracy (any-of-k and pre-launched),
  time saved `(T_seq − T_spec)/T_seq`, Actor/Speculator token counts and
  cost, peak store bytes, orphaned branch count, verification failures
  (must be 0 for success).
- `summary.md`: human-readable, generated per run.

Two comparison axes:

1. **Speculative vs. sequential** (paper axis): `runner_spec.py` vs.
   `runner_regular.py`, same seeds, same models. Yields accuracy and time
   saved.
2. **AgentVFS vs. native filesystem** (systems axis): `runner_regular.py`
   on the AgentVFS mount vs. on a plain directory quantifies FUSE
   steady-state overhead on the training hot path. Per-verb latencies and
   store growth come from `verbs.csv`. Success criterion: branch/checkpoint
   verbs are orders of magnitude smaller than the speculation window
   (µs–ms vs. tens of seconds).

## Error handling

- **Speculator failure/timeout:** degrade to sequential for that step; count
  it; never block the Actor.
- **Miss cleanup ordering:** kill the speculative training's process group
  first, then `branch delete`. A failed delete marks the branch orphaned,
  logs it, and continues — orphans are storage leakage, not correctness
  failures; the count lands in `totals.csv`.
- **Hit with in-flight training:** the run continues to completion on its
  speculative branch (it started during Actor deliberation, so it finishes
  early by that head start), then one merge lands edit, artifacts, and metric
  on main and the branch is deleted. Merge happens exactly once, after the
  process exits — no process is ever re-routed between branches.
- **Daemon/socket loss mid-run:** fail closed — abort with a clear error;
  never continue without isolation.
- **Startup preconditions:** verify daemon reachable, mount alive, cgroup
  delegation available (create/route/destroy a probe cgroup), and branch
  routing functional (write a probe file on a probe branch, confirm main
  does not see it). Abort with a clear message if any precondition fails.

## Testing

1. **Unit (no CPU-training, no API):** `agentvfs.py` against a scratch
   daemon — branch create/route/merge/delete lifecycle and latency capture;
   `verifier.py` shadow-model logic with synthetic states; edit
   normalization/matching with recorded edit pairs.
2. **Integration (no API, CI-runnable):** scripted fake Actor/Speculator
   (canned responses, injected latencies) + stub trainer (sleeps a few
   seconds, emits a deterministic metric) — exercises hit, miss,
   discard-regression rollback, cleanup ordering, and the losslessness check
   end-to-end in under a minute.
3. **Real run (CPU training + API keys):** k ∈ {1, 2, 3}, ~20 steps, real
   Actor/Speculator models — produces the experiment numbers on both
   comparison axes.

## Non-goals (v1)

- GPU tier (design-compatible; future work).
- Parallel pre-launch of more than one speculative training run.
- pi extension integration — phase 2, separate spec: port the proven branch
  orchestration core into pi's extension API (`before_provider_request`,
  `tool_call`) so pi gains speculative tool execution.
- Fault injection (daemon killed mid-merge, socket loss during rollback) —
  future hardening.
- Runtime process snapshots (`runtime snapshot/restore`) — the speculative
  training process is killed or kept, never memory-snapshotted, in v1.
- Multi-step (depth) speculation and confidence-adaptive k.

## Open questions

None blocking. One implementation-time verification item: confirm cgroup
branch routing semantics (`session register --cgroup ... --branch`) behave as
assumed for child processes spawned inside the registered cgroup; the
implementation plan's first task must include this smoke test before any
runner code is written.
