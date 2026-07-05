# AgentVFS Live Runtime Snapshot Design

**Date:** 2026-07-02
**Status:** Draft, approved for design/specification
**Scope:** Non-CRIU process execution-state rollback for AgentVFS-managed
runtime scopes. This design adds live fork-template snapshots that couple
process execution state with AgentVFS filesystem commits through CAS union
state objects.

> **Implementation status:** First slice implemented for cooperative
> AgentVFS-launched Linux runtimes. Runtime rollback requires the target process
> to call `agentvfs_runtime_boundary()`; unmanaged PIDs and non-cooperative
> commands remain filesystem-only or metadata-only.

## Problem

AgentVFS already checkpoints and restores filesystem state through CAS-backed
commits. The agent-state journal design records semantic continuation state, but
it does not restore real process execution state: heap contents, interpreter
state, open file descriptors, active subprocesses, or suspended execution
contexts.

Crab and DeltaBox show why filesystem-only or semantic-only restore is weaker
than true sandbox rollback. For workloads with long-lived shells, interpreters,
test harnesses, local servers, or speculative branch execution, rollback must
restore a consistent pair:

- the AgentVFS filesystem commit visible to the process; and
- the process execution state that was running against that commit.

CRIU can capture generic Linux process state, but it is too heavy and broad for
AgentVFS's first process-state path. It serializes arbitrary kernel-visible
state, depends on many kernel and privilege details, and is not a clean fit for
fine-grained speculative rollback. AgentVFS should instead own a narrower
runtime boundary and provide a fast non-CRIU path.

## Goals

1. Provide true process execution-state rollback for AgentVFS-owned runtime
   scopes without CRIU.
2. Couple runtime snapshots with AgentVFS filesystem commits so restore never
   pairs a process image with the wrong file tree.
3. Make rollback fast enough for speculative execution, branch fanout, and
   high-risk tool experimentation inside one active host run.
4. Keep durable metadata in the existing CAS graph through a union runtime state
   object.
5. Preserve an honest durability distinction: live template snapshots are fast
   but not crash-durable process-memory artifacts.
6. Require explicit quiescence boundaries for the first implementation.
7. Support a narrow, testable Linux contract before adding durable memory-page
   snapshots or broader kernel-resource support.

## Non-Goals

- No CRIU backend or CRIU fallback.
- No arbitrary host-process checkpointing.
- No durable process-memory snapshot in the first slice.
- No restore across host reboot, daemon crash, or template-process death in the
  first slice.
- No mid-syscall or asynchronous arbitrary instruction-point checkpointing.
- No automatic restore of TCP sockets, browser sessions, GPU/device state,
  remote sandboxes, or external API side effects.
- No process-state merge. Filesystem merge remains separate from runtime
  continuation selection.
- No support for unmanaged processes that bypass the AgentVFS runtime
  supervisor.

## Selected Approach

AgentVFS should add a managed runtime launcher, tentatively `agentvfs-run`, that
starts an agent or tool process under an AgentVFS-owned runtime scope. The
runtime supervisor owns the process tree, cgroup/session identity, workspace
branch, and lifecycle operations.

The live-template path requires a managed trampoline. A normal Linux process
cannot be forced by another process to `fork()` itself later. Therefore the
first slice must launch the runtime through a small AgentVFS runtime wrapper
that can enter a quiescent checkpoint routine, call `fork()`, park the template
child in a control loop, and later ask that template child to fork/resume a new
active runtime generation. This is a narrower contract than arbitrary process
checkpointing, but it is what makes the non-CRIU path feasible and testable.

At a checkpoint boundary, AgentVFS creates a live fork-template snapshot:

1. Arm a pending snapshot for the managed runtime.
2. Wait until the runtime reaches the requested cooperative boundary and parks
   inside the boundary request.
3. Flush AgentVFS dirty file handles.
4. Write a normal AgentVFS filesystem checkpoint and get an `fs_commit`.
5. Release the parked runtime with a snapshot action so it forks a parked
   template from the process image that was quiescent for that `fs_commit`.
6. Register the template in a process group that is separate from the active
   generation's process group.
7. Write a CAS union runtime state object that links the `fs_commit`, semantic
   agent state, runtime scope, and live template identifier.
8. Resume the active runtime.

Restore validates the live template, records the current filesystem commit as a
recovery point, freezes the active runtime generation without killing the parked
template, rolls the branch back to the recorded `fs_commit`, asks the template
to fork a fresh active generation, waits for that generation to report ready,
and only then force-retires the old frozen active generation. Because the old
generation is stopped during restore, retirement uses forced process-group
termination rather than a graceful SIGTERM wait. If restore fails before the new
generation is ready, AgentVFS attempts to roll the branch back to the recovery
commit and resume the old generation. If recovery also fails, AgentVFS reports a
precise partial state and leaves the affected runtime stopped.

This design uses the kernel's normal copy-on-write fork semantics as the first
process snapshot mechanism. It avoids serializing memory on the hot path and
keeps the first implementation much smaller than a CRIU-like generic process
restorer.

## Why Live Templates Instead Of CRIU

CRIU's genericity is the source of its cost. It must discover and serialize
process topology, memory, file descriptors, namespaces, sockets, signal state,
timers, and many resource-specific corner cases. AgentVFS does not need that
for the first non-CRIU path.

AgentVFS can impose a narrower contract:

- the process is launched by AgentVFS;
- the process uses an AgentVFS workspace branch as its working tree;
- checkpointing happens only at explicit quiescent boundaries;
- live templates only need to survive within the current host run;
- unsupported external resources are declared rather than silently restored.

Within that contract, `fork()` is the lightweight checkpoint primitive. The
template process remains a suspended in-kernel copy-on-write image running only
the AgentVFS template-control loop. Rollback can reuse the template without
reading process memory from storage.

## Architecture

### RuntimeSupervisor

`RuntimeSupervisor` launches and owns managed runtime scopes. A scope contains:

- runtime identifier;
- root process PID and process group;
- optional cgroup path;
- AgentVFS branch name and mountpoint;
- command, argv, cwd, and selected environment references;
- current runtime generation;
- live template IDs reachable from the scope.

The supervisor is responsible for process creation, quiescence, snapshot,
restore, termination, and status reporting. It is also the boundary that
prevents AgentVFS from trying to checkpoint arbitrary host processes.

### Runtime Trampoline

`agentvfs-run` starts the target command through a trampoline process. The
trampoline is the code that makes fork-template snapshots possible without CRIU.
It must:

- establish the runtime scope before executing user work;
- expose a local control channel to the `RuntimeSupervisor`;
- enter checkpoint code only at explicit quiescence boundaries;
- create parked template children with `fork()`;
- keep template children from performing user-visible work while parked;
- put parked template children in a process group that is not killed when the
  active generation is retired;
- put each restored active generation in its own active process group;
- let a parked template fork a new active generation during restore;
- exit or park old active generations according to restore policy.

The first implementation should support a single active root process. Child
processes can be managed through the same process group for termination and
resource reporting, but full process-tree cloning should be a later extension
unless each child is also runtime-aware. This keeps the first slice honest: it
restores the execution state of the managed root runtime, not an arbitrary
uncooperative process forest.

### Quiescence Protocol

The first slice should require explicit quiescence. Supported boundaries:

- before speculative branch fanout;
- before high-risk tool calls;
- after tool-call completion;
- before user-requested rollback points;
- when an adapter requests a runtime snapshot.

The implementation can start with a supervisor-controlled stop-the-world mode
for the managed process tree. Later adapters can add cooperative quiescence,
where an agent runtime acknowledges that no tool call or external side effect is
in flight.

A snapshot must fail precisely if the runtime cannot quiesce.

### LiveTemplateStore

`LiveTemplateStore` tracks suspended template processes. Templates are runtime
objects, not durable CAS blobs. Each template has:

- `template_id`;
- owning `runtime_id`;
- parent union state, if any;
- associated `fs_commit`;
- process/template PID metadata;
- creation time and generation;
- liveness status;
- resource warnings;
- retention pin count.

Templates are reachable from CAS union state metadata, but their memory lives in
the kernel. If the template dies, the union state remains inspectable but is no
longer runtime-restorable.

### UnionRuntimeState

The durable CAS object records the consistent pair of filesystem and runtime
state:

```text
UnionRuntimeState {
  record_version
  union_state_id
  parent_union_state_id
  branch
  fs_commit
  agent_state_id
  runtime_id
  runtime_generation
  template_id
  template_kind = "live_fork"
  boundary_kind
  command_ref
  resource_manifest_ref
  timestamp_ns
}
```

`union_state_id` is the CAS blob hash for the serialized object. It is a
derived identifier and is not serialized inside the object body.

Effective restore eligibility is explicit in API responses but is not a durable
CAS field, because template liveness can change after the union object is
written:

- `live_runtime_restorable`: template is alive and `fs_commit` is present;
- `fs_only`: filesystem can be restored but the live template is unavailable;
- `metadata_only`: the state can be inspected but cannot be restored.

The object should be written through the existing `ObjectStore` as a new
CAS-backed state body or as a typed blob until object typing is extended.

### Resource Manifest

The resource manifest describes what the first slice supports and what it does
not. Initial support should be conservative:

- supported: regular files under the AgentVFS mount, cwd under the mount,
  managed root process memory via live template, and process-group descendants
  for termination/resource reporting;
- reported but not restored: TCP sockets, browser sessions, remote services,
  external API calls, device/GPU state;
- rejected: unmanaged processes, unknown mount dependencies, unquiesced
  in-flight external side effects.

Unsupported resources should not be hidden. Snapshot and restore reports must
make them visible.

### Agent-State Journal Integration

The live runtime snapshot design complements the existing agent-state journal.
The journal records semantic continuation state. The union runtime state records
the coupled `fs_commit` and live template identity.

An agent-state record can reference a union runtime state through a future
payload schema such as `agentvfs.runtime_snapshot.v1`. Conversely, the
`UnionRuntimeState` stores `agent_state_id` when a semantic continuation state
exists.

This preserves both views:

- adapters can reconstruct agent intent from journal payloads;
- AgentVFS can restore process execution state from the live template.

## Snapshot Flow

1. Caller requests `runtime.snapshot` with a `runtime_id`, boundary kind, and
   optional agent-state ID.
2. `RuntimeSupervisor` verifies that it owns the runtime and that the runtime is
   restorable.
3. The runtime reaches the requested cooperative boundary and blocks in its
   `runtime.boundary` request.
4. AgentVFS flushes dirty file handles for the runtime branch.
5. `CheckpointManager` writes an AgentVFS filesystem commit.
6. AgentVFS releases the blocked boundary request with a snapshot action.
7. The runtime forks a parked template from the quiescent process image.
8. `LiveTemplateStore` registers the template, its separate process group, and
   pins it.
9. AgentVFS writes `UnionRuntimeState` to CAS.
10. Runtime is resumed.
11. API returns the `union_state_id`, `fs_commit`, `template_id`, and effective
    eligibility.

Failure handling:

- if quiescence fails, no filesystem checkpoint or template is published;
- if filesystem checkpoint fails, runtime is resumed and no template is
  published;
- if template creation fails, runtime is resumed and no union state is
  published;
- if CAS union-state write fails, the new template is dropped and runtime is
  resumed.

## Restore Flow

1. Caller requests `runtime.restore` with a `union_state_id`.
2. AgentVFS reads the `UnionRuntimeState`.
3. AgentVFS verifies that the referenced live template is still alive.
4. AgentVFS resolves the runtime branch and validates both the current recovery
   commit and the target `fs_commit` before stopping any process.
5. AgentVFS records the runtime branch's current commit as the recovery commit.
6. AgentVFS freezes the active generation's process group without killing the
   parked template process group.
7. AgentVFS rolls the branch back to `fs_commit` and invalidates stale file
   handles for that branch.
8. Only after rollback succeeds, the supervisor releases the parked template's
   poll request so it can fork and resume a new active runtime generation in a
   fresh active process group.
9. The new generation reports ready; runtime generation advances.
10. AgentVFS force-retires the old frozen active generation's process group.
11. API returns restored process metadata, filesystem outcome, warnings, and
    effective restore eligibility.

Restore should be all-or-fail from the caller's perspective when recovery is
possible. If rollback to `fs_commit` fails, AgentVFS resumes the old generation
and reports failure. If rollback succeeds but template restore fails, AgentVFS
attempts to roll back to the recovery commit and resume the old generation. If
that recovery fails, AgentVFS must report the partial state precisely and leave
the runtime stopped unless a caller explicitly asks for filesystem-only
degradation.

## Retention And GC

Live templates consume memory. The first slice should use explicit retention:

- templates are pinned by union states;
- callers can drop templates explicitly;
- a per-runtime template limit evicts old unpinned templates;
- eviction changes restore eligibility from `live_runtime_restorable` to
  `fs_only`.

CAS union state objects remain durable metadata even after template eviction.
They are part of graph retention, but they do not keep process memory alive
after eviction.

## Correctness Invariants

1. A live runtime snapshot must always reference exactly one `fs_commit`.
2. A live template is restorable only with the `fs_commit` captured at the same
   boundary.
3. AgentVFS must never report a template-evicted union state as process
   restorable.
4. Runtime restore must invalidate stale file handles for the restored branch.
5. Runtime snapshots must be scoped to AgentVFS-launched runtimes.
6. Unsupported resources must be reported as warnings or restore blockers.
7. Runtime state is selected, not merged.
8. A failed snapshot must not advance a runtime-state ref.

## Control API

Initial commands:

- `runtime.create`: internal launcher registration for `agentvfs-run`; it
  validates branch/process state and stores a launcher-issued control token.
- `runtime.snapshot`: create `fs_commit` plus live fork-template union state.
- `runtime.restore`: restore filesystem and process execution state from a live
  template.
- `runtime.drop`: release one live template or all templates for a runtime.
- `runtime.status`: report runtime process, template, and restore eligibility
  state.
- `runtime.list`: list managed runtimes and live templates.

The CLI can expose these as:

```text
agentvfs-run --branch <branch> -- <command> [args...]
agentvfs-ctl runtime snapshot <runtime-id> --boundary <kind>
agentvfs-ctl runtime restore <union-state-id>
agentvfs-ctl runtime status <runtime-id>
agentvfs-ctl runtime drop <template-id>
```

Lifecycle control messages from the runtime (`runtime.boundary`,
`runtime.template.ready`, `runtime.template.poll`, and
`runtime.generation.ready`) carry the launcher-issued control token and are
validated against Unix-domain peer credentials when the production socket
transport supplies them. `runtime.create` is accepted only from the launcher
parent of the registered child process, and lifecycle messages must come from
the process they claim to represent. Manual cooperative registration through
`agentvfs-ctl runtime create` is not exposed.

## Testing Plan

Start with small Linux-only integration tests.

1. Runtime launch:
   - `agentvfs-run` starts a simple process through the runtime trampoline;
   - status reports runtime ID, branch, PID, and generation.
2. Snapshot and restore:
   - a cooperative C++ fixture linked with the AgentVFS runtime client mutates
     memory and files;
   - snapshot captures both the memory continuation and `fs_commit`;
   - later mutations are undone by `runtime.restore`.
3. Filesystem coupling:
   - restoring a template without the matching `fs_commit` is rejected by tests;
   - stale file handles are invalidated after restore.
4. Template liveness:
   - dropping a template changes restore eligibility to `fs_only`;
   - restore fails precisely when the template is unavailable.
5. Boundary enforcement:
   - snapshot fails if the runtime cannot quiesce;
   - unmanaged host PIDs are rejected.
6. Resource reporting:
   - unsupported sockets or external handles appear in warnings or blockers.

## Later Work

After live templates are useful, durable memory snapshots can be designed as a
separate layer:

- dirty-page tracking through Linux soft-dirty or explicit runtime cooperation;
- CAS-backed memory page blobs;
- VMA/page-index manifests;
- lazy page restore with `userfaultfd`;
- optional hot-template recreation from durable memory snapshots.

That later work should not change the first-slice contract: union runtime states
must keep filesystem state and process execution state coupled, and restore
eligibility must remain explicit.

## Acceptance Criteria

The design is ready for implementation planning when:

- AgentVFS has a clear managed-runtime boundary;
- the runtime trampoline contract is explicit;
- live fork-template snapshots are the selected non-CRIU fast path;
- union runtime state objects couple `fs_commit`, semantic state, and template
  identity;
- restore eligibility distinguishes live process restore from filesystem-only
  degradation;
- unsupported resources are explicit;
- all snapshot and restore failure paths avoid silent partial publication.
