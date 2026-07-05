# Agent State Journal And Runtime Descriptor Design

**Date:** 2026-06-15
**Revision:** 2026-07-03
**Status:** Revised for implementation after cooperative runtime snapshots
**Scope:** Lightweight, CAS-native storage of semantic agent continuation state
in AgentVFS. This design complements the implemented 2026-07-02 cooperative
runtime snapshot path instead of duplicating process-memory restore.

## Current Context

AgentVFS now has three distinct state layers:

1. Filesystem state: durable CAS commits and branch refs.
2. Runtime process state: live cooperative fork templates recorded through
   `UnionRuntimeState`.
3. Agent state: semantic continuation state that an adapter can inspect and use
   to reconstruct an agent run.

The 2026-07-02 live runtime snapshot implementation captures a narrow form of
process state. It can restore the in-memory continuation of an
AgentVFS-launched cooperative runtime while the live template remains alive.
That path is intentionally fast and opaque: it restores process bytes, but it
does not make session turns, tool calls, model settings, approvals, or external
handles durable, queryable, or portable.

This design keeps agent state as a separate first-class CAS graph and links it
to runtime snapshots through IDs:

```text
AgentStateRecord(state_id)
  -> fs_commit
  -> optional union_state_id
  -> optional runtime/external descriptors

UnionRuntimeState(union_state_id)
  -> fs_commit
  -> agent_state_id
  -> live template id
```

## Problem

AgentVFS can checkpoint files and, for cooperative runtimes, restore process
memory. Agents still need a semantic state history that survives process loss
and is useful outside the live host run. A useful continuation point includes
the current session, turn, active goal, plan/checklist position, tool-call
history, pending approvals, retry state, model/provider configuration,
filesystem linkage, runtime resources, and external handles.

Relying on process snapshots alone is insufficient because:

- process memory is opaque to AgentVFS;
- live fork templates are not crash durable;
- non-cooperative runtimes cannot be restored this way;
- process snapshots cannot be merged or queried as semantic history;
- adapters need portable records to resume or replay work after daemon restart,
  host reboot, or template loss.

The agent-state layer should therefore be small, explicit, and CAS-native. It
should avoid a separate database or heavyweight background materializer in the
first slice.

## Goals

1. Record semantic agent continuation state without forcing a filesystem
   checkpoint for every record.
2. Publish durable state records through the existing `ObjectStore` blob path
   and its `fsync_pending()` durability contract.
3. Keep hot-path records compact enough for frequent turn/tool/boundary updates.
4. Store large payloads as CAS blobs referenced by hash, not inline journal
   text.
5. Link agent state to `fs_commit` and optional `union_state_id`.
6. Use `UnionRuntimeState.agent_state_id` as the process-snapshot to
   semantic-state join point.
7. Bound restore cost with snapshot-style semantic records plus deltas.
8. Return payload-bearing records so an adapter can reconstruct session and
   execution context without inspecting process memory.
9. Preserve an honest restore-strength distinction: session-only,
   filesystem-linked, and runtime-linked.
10. Keep the first implementation dependency-free and aligned with existing
    line-oriented control protocol conventions.

## Non-Goals

- No CRIU backend or CRIU fallback.
- No raw arbitrary process-memory storage in the agent-state layer.
- No second object database, SQLite dependency, or parallel content store.
- No requirement that every agent-state append fsync immediately.
- No automatic subprocess launcher, socket reconnector, browser restorer, or
  external-service validator in the first slice.
- No attempt to merge runtime continuations.
- No storage of raw secrets. Payloads may store stable secret references, not
  provider keys, bearer tokens, private keys, session cookies, or passwords.
- No replacement for the cooperative runtime snapshot path.

## Architecture

### AgentStateRecord

`AgentStateRecord` is the durable semantic record. It is serialized as a small
line-oriented body and written through `ObjectStore::write_blob()`. Like
`UnionRuntimeState`, its ID is the CAS blob hash and is not serialized inside
the body.

Core fields:

```text
AgentStateRecord {
  record_version
  state_id                 # derived from CAS blob hash, not serialized
  parent_state_id          # previous semantic state
  snapshot_base_state_id   # optional bounded-restore base
  branch
  fs_commit
  union_state_id
  runtime_id
  agent_id
  sequence
  kind
  payload_schema
  payload_inline
  payload_ref              # CAS hash for large payloads
  timestamp_ns
  boundary
}
```

The `state_id` changes when any serialized field changes. Readers ignore
unknown keys for forward compatibility. `parent_state_id`,
`snapshot_base_state_id`, `payload_ref`, and `union_state_id` are optional
64-hex references; parsers must reject malformed values before writing a
record.

### Payload Storage

Small control payloads stay inline. Large message histories, tool outputs,
logs, terminal captures, screenshots, and binary artifacts are written as CAS
blobs and referenced through `payload_ref`. The first slice should avoid
copying large data into every state record.

Durable publication treats the state record and any newly written payload blob
as one publish set. If `payload_ref` points to a blob created by the same append
operation, `ObjectStore::fsync_pending()` must receive both the payload hash and
the state hash before any latest ref is advanced. A durable state must not
reference a payload blob that is still only pending.

### Lightweight State Indexes

CAS blobs are the source of truth. A small state directory under the store is
only for discovery:

```text
<store>/state/latest/<agent_id>/<branch>
<store>/state/index.log
```

The latest ref is updated with atomic rename and directory fsync after a
durable append. `index.log` is append-only metadata for listing recent states;
losing it must not lose the state object itself. Rebuilding richer indexes from
known state IDs can be a later task.

Path components must be path-safe. Branch names already use
`is_valid_branch_name()` rules. `agent_id` must either use the same
`[A-Za-z0-9_-]{1,64}` validation rule or be percent-encoded before it appears
in a filesystem path. The first implementation should use validation rather
than adding a second encoding scheme.

The first slice does not create a separate materialized-state content store.
Snapshot-style agent-state records are the materialization mechanism.

### Runtime Linkage

The existing runtime snapshot API already accepts `agent_state_id` and stores
it in `UnionRuntimeState`. The agent-state layer should use a two-step linkage:

1. Append a semantic state record for the quiescent boundary.
2. Pass that `state_id` to `runtime.snapshot`.
3. Optionally append a follow-up `runtime_snapshot` record whose
   `parent_state_id` is the semantic state and whose `union_state_id` is the
   returned runtime snapshot.

This avoids mutating an existing content-addressed state object after the
runtime snapshot is created.

### Restore Coordinator

`state.restore` restores semantic state first. It then optionally restores the
filesystem and runtime layers:

- `mode=session`: return the bounded semantic record chain and payload refs.
- `mode=full`: require `fs_commit` and roll the branch back to it.
- `mode=runtime`: require `union_state_id`, restore the runtime through the
  existing runtime restore path, and report the filesystem/runtime outcome.

If runtime restore fails because the live template is gone, the response must
still return the semantic records and report degraded restore strength.

### Adapter Boundary

AgentVFS owns the common envelope, persistence, restore-strength reporting, and
runtime/filesystem linkage. Adapters own the meaning of payload schemas and the
logic for rebuilding an agent prompt, planner state, or tool replay decision.

## Canonical Kinds And Schemas

Initial `kind` values:

- `session`: conversation/session state.
- `execution`: active goal, plan/checklist, phase, retry state, pending
  approval gate, and routing state.
- `tool_call`: tool name, normalized arguments, status, result summary,
  side-effect classification, replay policy, and output refs.
- `runtime_resource`: semantic descriptor for a process, server, socket, or
  runtime dependency, plus restore policy.
- `external_handle`: remote sandbox, browser session, MCP server, database,
  service, API conversation, terminal, or other external dependency handle.
- `fs_link`: filesystem commit or pending filesystem context.
- `runtime_snapshot`: link from semantic state to a `UnionRuntimeState`.

Canonical payload schemas:

- `agentvfs.session.v1`
- `agentvfs.execution.v1`
- `agentvfs.tool_call.v1`
- `agentvfs.runtime_resource.v1`
- `agentvfs.external_handle.v1`
- `agentvfs.fs_link.v1`
- `agentvfs.runtime_snapshot.v1`

Adapters may add specific schemas, but the first reference adapter should emit
the canonical schemas wherever possible.

## Capture Flows

### Semantic Event

1. Adapter emits a compact state record.
2. AgentVFS writes the record body through `ObjectStore::write_blob()`.
3. If the caller requests durability, AgentVFS calls
   `ObjectStore::fsync_pending({payload_hashes..., state_hash}, error)`.
4. If the record has `parent_state_id` or `snapshot_base_state_id`, AgentVFS
   verifies those state objects exist before publishing a durable latest ref.
5. AgentVFS updates the latest ref and index metadata after the state object and
   its payload dependencies are durable.

### Filesystem Boundary

1. Adapter emits a semantic boundary record.
2. AgentVFS checkpoints the branch and obtains `fs_commit`.
3. Adapter emits or updates the next state record with `fs_commit`.
4. `state.restore --mode full` can restore this record.

### Runtime Snapshot Boundary

1. Adapter emits a semantic boundary record and receives `state_id`.
2. Caller runs `runtime.snapshot` with `agent_state_id=state_id`.
3. Runtime snapshot creates `UnionRuntimeState` with the same `fs_commit` and
   `agent_state_id`.
4. Adapter may append `runtime_snapshot` with `union_state_id`.
5. `state.restore --mode runtime` uses that `union_state_id` when present.

### High-Frequency Capture

High-frequency records should be deltas. Adapters must periodically emit
snapshot-style session/execution records so restore walks a bounded parent
chain. The first implementation should enforce a restore walk cap and return a
clear error if a requested state exceeds it without a snapshot base.

## Restore Semantics

Restore strength is computed from durable data and live runtime status:

- `session_only`: semantic records can be returned, but no filesystem commit is
  linked.
- `full_fs_linked`: semantic records and filesystem commit can be restored.
- `runtime_linked`: a `union_state_id` exists.
- `live_runtime_restorable`: the linked runtime template is currently alive.
- `degraded_runtime`: semantic and filesystem state are available, but the live
  runtime template is gone.

Runtime and external descriptors are policy reports in the first slice:

- `required`: fail runtime/external restore when no executor exists.
- `optional`: return warnings.
- `replay_only`: return records for adapter replay without automatic restart.

Durability is reported separately from restore strength:

- `logical_only`: the record was written but not explicitly fsynced and must not
  advance latest refs.
- `durable`: the record and all payload dependencies were fsynced through
  `ObjectStore::fsync_pending()`.
- `degraded_chain`: the record is durable, but a referenced parent or snapshot
  base is missing, so restore can return the record but not a complete bounded
  chain.

Unsynced states are describable by direct `state_id` while the object exists,
but they cannot be used as durable parents, latest refs, or full/runtime restore
roots until a later publish operation fsyncs and validates them.

## Control API

Initial commands:

- `state.append { ... }`: append one semantic record; returns `state_id`.
- `state.describe {"state_id":"..."}`: read one state record and report
  restore strength.
- `state.latest {"agent_id":"...","branch":"..."}`: read the latest durable
  state ref.
- `state.restore {"state_id":"...","mode":"session|full|runtime"}`: return a
  bounded restore chain and perform filesystem/runtime restore when requested.

The first slice may expose `state.list` as a best-effort scan over `index.log`,
but correctness must not depend on it.

`agentvfs-ctl runtime snapshot` should expose `--agent-state <state-id>` so the
CLI can populate the already-implemented `agent_state_id` field.
The control-socket `runtime.snapshot` handler must also validate any supplied
`agent_state_id` as empty or 64-hex; CLI validation alone is insufficient.

## Performance Rules

- One small state append should not force a filesystem checkpoint.
- Durable state publication must use `ObjectStore::fsync_pending()` for the
  state blob and newly created payload blobs, not a separate durability
  mechanism.
- Multiple state records should be batchable in one future control call and one
  fsync set.
- Large payloads must be stored once as CAS blobs and referenced by hash.
- Restore must walk a bounded chain: latest semantic snapshot plus deltas.
- Runtime restore must delegate to the existing runtime restore path rather
  than reimplement process handling.

## Testing Plan

1. Record serialization tests:
   - deterministic body serialization;
   - `state_id` derived from the object hash, not serialized;
   - unknown field tolerance;
   - invalid hash rejection for `fs_commit`, `union_state_id`, and parent IDs.
2. ObjectStore integration tests:
   - write/read state blob;
   - `fsync_pending()` failure is reported;
   - large payload refs round-trip;
   - durable append fsyncs both payload and state hashes before latest-ref
     publication.
3. Service tests:
   - append/describe/latest;
   - path-unsafe `agent_id` values are rejected;
   - parent chain and snapshot base handling;
   - durable append rejects missing parent/base references;
   - restore walk cap;
   - no filesystem checkpoint for simple semantic append;
   - unsynced states are describable by direct ID but do not advance latest.
4. Runtime linkage tests:
   - `runtime.snapshot` stores supplied `agent_state_id`;
   - invalid direct control-socket `agent_state_id` is rejected;
   - `runtime_snapshot` record links back to `union_state_id`;
   - runtime restore degrades cleanly when template is unavailable.
5. Control/CLI tests:
   - `state append`, `state describe`, `state latest`, and `state restore`;
   - `runtime snapshot --agent-state`;
   - full restore rejects states without `fs_commit`;
   - runtime restore rejects states without `union_state_id`.

## Acceptance Criteria

- Agent-state records are stored as CAS blobs through `ObjectStore`.
- `state_id` is the CAS blob hash and is not serialized inside the record body.
- Large payload bodies can be stored as CAS blobs and referenced by hash.
- Durable state publication fsyncs state and payload blobs as a single publish
  set.
- The latest durable state can be recovered after daemon restart.
- Latest-ref path components cannot escape `<store>/state/latest`.
- Durable states cannot silently depend on missing parent/base records.
- `state.restore --mode session` returns payload-bearing semantic records.
- `state.restore --mode full` rolls back only when an `fs_commit` is present.
- `state.restore --mode runtime` delegates to existing runtime restore when a
  `union_state_id` is present.
- Runtime snapshot responses and union states preserve `agent_state_id`.
- The implementation does not add a database, CRIU dependency, or parallel
  object store.
- Documentation clearly distinguishes semantic agent state from process state.
