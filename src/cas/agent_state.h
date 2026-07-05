#pragma once

#include "hash.h"
#include "object_store.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cas {

// Classifies what kind of agent-visible state a record captures. Serialized
// as a lowercase snake_case token (see agent_state.cpp); unknown tokens are
// rejected by the parser so a future kind cannot be silently misread as a
// default.
enum class AgentStateKind {
    Session,
    Execution,
    ToolCall,
    RuntimeResource,
    ExternalHandle,
    FsLink,
    RuntimeSnapshot,
};

// Restore strength is an out-of-band annotation used by higher layers
// (Tasks 2+); it is not serialized into the v1 record body. Declared here so
// downstream code can depend on a single canonical enum.
enum class AgentRestoreStrength {
    SessionOnly,
    FullFsLinked,
    RuntimeLinked,
    LiveRuntimeRestorable,
    DegradedRuntime,
};

// A CAS-native agent state record. Mirrors UnionRuntimeState's pattern: the
// body is a line-oriented key=value document with a versioned first-line
// marker, and `state_id` is NEVER serialized — it is derived from the
// content-addressed blob hash on write and re-derived on read.
struct AgentStateRecord {
    uint32_t record_version = 1;
    std::string state_id;              // Derived from CAS blob hash.
    std::string parent_state_id;
    std::string snapshot_base_state_id;
    std::string branch = "main";
    Hash fs_commit = ZERO_HASH;
    std::string union_state_id;
    std::string runtime_id;
    std::string agent_id;
    uint64_t sequence = 0;
    AgentStateKind kind = AgentStateKind::Session;
    std::string payload_schema;
    std::string payload_inline;
    std::string payload_ref;
    uint64_t timestamp_ns = 0;
    bool boundary = false;
};

std::vector<uint8_t> serialize_agent_state_record(const AgentStateRecord& state);
bool deserialize_agent_state_record(const std::vector<uint8_t>& body,
                                    AgentStateRecord& out,
                                    std::string& error);

// Canonical AgentStateKind vocabulary. `parse_agent_state_kind` is strict: it
// returns false for an empty or unknown token (callers that want a default,
// e.g. the control protocol's optional `kind` field, default to Session
// themselves BEFORE calling). `agent_state_kind_label` is the inverse mapping
// used both to serialize a record body and to render the JSON label.
bool parse_agent_state_kind(const std::string& text, AgentStateKind& out);
const char* agent_state_kind_label(AgentStateKind kind);

// Writes the serialized record to the CAS as a blob, assigns
// `state.state_id` from the returned blob hash, and (when `sync` is true)
// publishes the dependency hashes together with the state blob via
// `fsync_pending` so the state and its referenced objects become durable as
// one set. Returns ZERO_HASH and sets `error` if any dependency hash is
// ZERO_HASH, if the blob write fails, or if the synced publish fails.
Hash write_agent_state_record(ObjectStore& store,
                              AgentStateRecord& state,
                              const std::vector<Hash>& dependency_hashes,
                              bool sync,
                              std::string& error);

// Reads and deserializes the record identified by `id`, then re-derives
// `out.state_id` from `id`. Returns false and sets `error` on read or parse
// failure.
bool read_agent_state_record(ObjectStore& store,
                             const Hash& id,
                             AgentStateRecord& out,
                             std::string& error);

} // namespace cas
