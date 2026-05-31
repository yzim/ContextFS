# AgentVFS capability roadmap

**Date:** 2026-05-31
**Status:** Approved (2026-05-31)
**Scope:** A dependency-ordered capability roadmap for evolving agentvfs from a checkpointable FUSE filesystem into a substrate for fleets of long-lived agents. This is a *roadmap*, not an implementation spec — each layer and item below later gets its own brainstorm → spec → plan cycle.

## What this document is

agentvfs today is a checkpointable, branchable, content-addressed FUSE filesystem: CAS object store, checkpoint/rollback, three FUSE adapters (libfuse3 / fuse-t / WinFsp), per-agent branches via cgroup v2 (Linux), a telemetry pipeline (Linux), and a quickstart Skill for Claude Code / Codex. This roadmap charts where it goes next.

It is the skeleton, not the build. No item here is implementation-ready as written; each becomes its own spec when it reaches the front of the queue.

## Organizing principle

Four layers, each enabling the next. **"Short-term vs. long-term" means depth in the dependency chain, not a calendar date** — there are no dates in this roadmap by design.

```
L0  Foundation        Long-lived agent durability                     (deepest dependency)
      │   bounded memory · crash recovery · CAS GC · checkpoint retention
      ▼
L1  Agent interface   Programmatic agent API                          (the hub)
      │   agent-facing verbs · portable branch-addressing token · client SDK · versioned protocol
      ▼
L2  Scale & breadth   (three parts, partly parallel)
      │   2a multi-subagent fan-out (~100)
      │   2b rest of agent-support (auto-checkpoint · harnesses · embedding)
      │   2c platform parity track  ← runs in parallel once L1's routing token exists
      ▼
L3  Frontier          tool-calling (3 options) · agent state & memory (options)   (decide later)
```

Two load-bearing claims justify this ordering:

1. **Durability is the floor.** The in-memory `WorkingTree`, in-memory `WriteBuffer`, and no-GC `ObjectStore` mean long sessions and 100-branch fan-out blow up memory and disk *multiplicatively*. L0 must precede L2.
2. **The agent API is the hub** that subagents, auto-checkpoint, harnesses, and embedding all sit on — *and* its explicit branch-addressing token is the portable replacement for Linux-only cgroup routing, which is what lets macOS/Windows have branches at all. That is why platform parity (2c) can only run in parallel *after* L1.

## Audience

Maintainers and contributors planning what to build next, and integrators (e.g. the Tencent workbuddy embedding request) who want to see where the agent-facing surface is heading. Each item is written to be picked up as its own design effort.

---

## L0 — Foundation: long-lived agent durability

The floor. Today committed state is durable (tree/commit objects fsync'd, ref advanced atomically), but the working state is in-memory, there is no crash recovery for uncheckpointed work, and the `ObjectStore` never reclaims objects (`pending_` drains to disk only at checkpoint). A single agent running for hours strains this; 100 of them breaks it.

### L0.1 — Working-tree persistence & crash recovery
Committed state survives; *recovery* and *in-flight work* do not. On daemon restart, automatically recover the latest committed state per branch — the rollback path already replays a commit into a fresh `WorkingTree`, so generalize it to startup. Optionally journal dirty `WriteBuffer` state so a crash mid-session does not silently lose uncheckpointed work.
**Deliverable:** a written durability contract stating exactly what survives a crash and what does not.

### L0.2 — Bounded memory / cold-state spill
Keep RSS bounded regardless of tree size or session length. Spill cold dirty buffers and rarely-touched tree subtrees to CAS-backed on-disk structures; keep a hot working set resident. This is the item that makes both *long-lived* sessions and *100-branch* fan-out feasible — without it, fan-out cost is roughly 100× full in-memory trees.
**Open design fork (for L0.2's own spec):** whether spill reuses the CAS object format directly or introduces a separate on-disk working-set structure.

### L0.3 — CAS garbage collection
`ObjectStore` never reclaims. After rollbacks, branch deletes, and checkpoint pruning, unreferenced objects accumulate forever. Add mark-and-sweep GC rooted at all live refs (branch HEADs + retained checkpoints), runnable online without quiescing the daemon. The `pending_`/fsync contract in `object_store.h` is the integrity boundary GC must respect.

### L0.4 — Checkpoint retention & history compaction
Long sessions produce many checkpoints. Add a retention policy (keep-N / time-decay / keep-labeled) and compact ref history so it does not grow unbounded. Retention defines GC roots, so L0.3 and L0.4 are designed together.

**Why this layer is first:** L2's 100-branch fan-out multiplies every one of these costs. Building scale before durability guarantees rework.

---

## L1 — Agent interface: the programmatic agent API

Today the control protocol (`control_protocol.cpp`) is driven by humans via `agentvfs-ctl` / `agentvfs workspace`, and branch routing is implicit through cgroup v2 (Linux-only). A running agent cannot cleanly drive its own lifecycle, sibling subagents cannot address each other's branches, and off-Linux has no routing at all. This layer is the hub everything in L2/L3 builds on.

### L1.1 — Agent-facing control verbs
Curate an agent-ergonomic surface over the protocol: self-checkpoint (with label + structured metadata such as turn number / tool-call id), rollback-self, branch-create/switch to spawn a child workspace, and query (my branch, my HEAD, my dirty set, diff-since-label). Most plumbing exists (`checkpoint`, `rollback`, `branch.create/delete/list/merge`, `status`); the work is shaping it for a *running agent* as the caller and threading metadata through `commit.cpp` so checkpoints carry agent context.

### L1.2 — Portable branch-addressing token *(the unlock)*
Introduce an explicit branch handle an agent attaches to its session (or presents per-op), so routing no longer requires cgroup v2. On Linux, cgroup routing stays the zero-config default; the token is (a) the portable fallback that gives macOS/Windows branches at all, and (b) the mechanism a subagent uses to address a sibling/child branch. `BranchRouter::resolve` returning 0 off-Linux today becomes "resolve via token, else main." **This single item is what makes platform parity (2c) possible.**

### L1.3 — Client SDK
A thin library wrapping the line-protocol so harnesses and embedders do not hand-roll socket/named-pipe JSON. Start with the language(s) the target harnesses need (likely Python and/or TypeScript for agent frameworks) plus the existing C/CLI client. The wire protocol stays the contract; the SDK gives integrators ergonomics.

### L1.4 — Versioned, stable protocol
Once external embedders (workbuddy) and the SDK depend on the wire format, declare a protocol version and a compatibility contract. Cheap to add now, expensive to retrofit after embedders ship.

**Sequencing within the layer:** L1.1 + L1.2 are the core (verbs + routing token); L1.3 wraps them; L1.4 is a small gate to land *before* the first external embedder, not after.

**Dependency callout:** L1.2's token is consumed by 2a (subagent addressing), 2c (cross-platform routing), and auto-checkpoint in 2b (which tags checkpoints with the metadata from L1.1). This is why L1 sits above the whole L2 layer.

---

## L2 — Scale & breadth

Three parts, built on L0 (bounded memory / GC) and L1 (API + routing token). 2a and 2b are the depth continuation; 2c is the parallel breadth track that becomes possible the moment L1.2's token exists.

### 2a — Multi-subagent fan-out (~100)
- **Lazy branch instantiation.** Branch *creation* is already near-free in CAS (shared objects), but each branch today instantiates a full in-memory `WorkingTree`. Make instantiation lazy / copy-on-reference so 100 branches do not mean 100 full resident trees. Directly consumes L0.2 spill.
- **Routing at scale.** Validate `BranchRouter` cost and the L1.2 token path under 100 concurrent subagents — lookup cost, contention, correctness of sibling addressing.
- **Merge orchestration / fan-in.** `branch_merge.cpp` does pairwise 3-way merge. 100 branches need orchestrated fan-in: tournament/sequential merge, conflict aggregation across branches, and partial-merge reporting (merge the clean ones, surface the conflicted set). Decide the N-way semantics.
- **Daemon concurrency & fairness.** Per-branch `checkpoint_mu` exists; stress the daemon's concurrency model at 100 branches — FUSE op throughput, lock contention, and per-branch resource caps so one runaway subagent cannot starve the rest.

### 2b — Rest of agent-support
- **Auto-checkpoint policy.** FS-driven checkpoints on triggers: per turn, per tool-call boundary (via L1.1 metadata), or on risky ops flagged by the telemetry pipeline. The FS decides when to snapshot instead of waiting for explicit commands.
- **More harnesses.** Adapters/Skills beyond Claude Code / Codex — Cursor, Cline, Aider, OpenHands, LangGraph — riding on the L1.3 SDK.
- **Platform embedding.** Package the SDK + stable protocol (L1.4) for embedders like Tencent workbuddy; document the embedding contract (lifecycle, ownership of mount/store, isolation guarantees).

### 2c — Platform parity track *(parallel breadth)*
- **macOS/Windows branches.** Wire L1.2 token-based routing into the `fuse_t_adapter` and `winfsp_adapter` (cgroup v2 is unavailable off-Linux — the token *is* the routing mechanism there).
- **Telemetry off-Linux.** Today Linux-only (eBPF / fanotify / ptrace / `LD_PRELOAD`). Scope a feasible reduced backend — FUSE/WinFsp op-level capture, or FSEvents/ETW — and set expectations that off-Linux telemetry is weaker by nature.
- **`agentvfs workspace` CLI off-Linux.** Port `workspace_cli` state management + control plane (the Windows named-pipe channel already exists; macOS uses the AF_UNIX path).

**Parallelism note:** 2a/2b are the dependency-spine continuation; 2c is genuinely independent once the token lands, so a second contributor can drive parity while the first drives scale — but 2c *cannot start its branch work before L1.2*. That is the one hard cross-track edge.

---

## L3 — Frontier (options laid out, not committed)

Captured at coarse grain deliberately: these are directional, and the user chose to keep the options open rather than commit now.

### Tool-calling support — three interpretations to choose between later
- **Observe & version.** Extend telemetry up from syscalls to tool-call granularity (MCP / function calls), record calls in checkpoint history, correlate each call with the file changes it caused. agentvfs never blocks. *Lowest risk; pure extension of the telemetry pipeline + L1.1 metadata.*
- **Mediate & gate.** agentvfs sits in the tool-call path: policy allow/deny/sandbox, tool side-effects made checkpointable. *Active runtime layer — the biggest identity shift.*
- **Transact & roll back.** A tool call becomes an atomic checkpoint unit (files + state) that rollback undoes wholesale. *Focus on atomicity, not gating.*

All three share a substrate — telemetry + L1.1 metadata + transactional checkpoints — so that substrate is the real near-frontier work; the depth choice can wait for usage signal.

### Agent state & memory — options to choose between later
- **Versioned memory objects.** Agent memory/scratchpad stored as CAS objects, branched and rolled back alongside the tree. *Natural extension of what exists.*
- **Structured state namespace.** A non-file namespace (KV / records / vectors) in the daemon, versioned with the tree.
- **Selective rollback semantics.** The key design question regardless of representation: when files roll back, does memory co-roll-back or persist? Define this explicitly.

---

## Non-goals (initial)

- Not a full agent runtime/orchestrator — agentvfs provides substrate + API, not the agent loop.
- Not a distributed/multi-host filesystem — the single-host daemon stays the model.
- Wasm/Lua processor execution stays optional/stubbed until a concrete need pulls it in.
- Not a git replacement — CAS is internal, not a git remote.

## Key risks

- **L0 touches the most correctness-critical code** (`ObjectStore` / `refs` / `checkpoint`). Durability and GC need heavy testing; a GC bug corrupts everyone's history.
- **Off-Linux telemetry is fundamentally weaker** — manage expectations rather than promise parity.
- **Protocol-stability vs. velocity tension** once embedders depend on L1.4.
- **The 100-branch memory blowup** is the entire reason L0 precedes L2 — violate the ordering and you rework scale.

## Sequencing spine

The whole roadmap in one line:

**L0 durability → L1 agent API + routing token → L2 scale (2a/2b) with platform parity (2c) in parallel → L3 frontier.**

Hard edges: parity branch-work (2c) is gated on L1.2; everything in L3 is gated on telemetry + L1 + transactional checkpoints.

## How to use this roadmap

Pick the item at the front of the dependency chain (start with an L0 item), run it through brainstorming → writing-plans → implementation as its own cycle, and revisit this document as a living artifact when layers complete or priorities shift.
