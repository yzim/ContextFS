#pragma once

#include "hash.h"
#include "object_store.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cas {

enum class RestoreEligibility {
    LiveRuntimeRestorable,
    FsOnly,
    MetadataOnly,
};

struct ResourceWarning {
    std::string kind;
    std::string description;
    bool blocker = false;
};

struct UnionRuntimeState {
    uint32_t record_version = 1;
    // Derived from the CAS blob hash. It is not serialized into the blob.
    std::string union_state_id;
    std::string parent_union_state_id;
    std::string branch = "main";
    Hash fs_commit = ZERO_HASH;
    std::string agent_state_id;
    std::string runtime_id;
    uint64_t runtime_generation = 0;
    std::string template_id;
    std::string template_kind = "live_fork";
    std::string boundary_kind;
    std::string command_ref;
    std::string resource_manifest_ref;
    uint64_t timestamp_ns = 0;
    std::vector<ResourceWarning> warnings;
};

std::string restore_eligibility_to_string(RestoreEligibility eligibility);
bool restore_eligibility_from_string(const std::string& text,
                                     RestoreEligibility& out);
std::vector<uint8_t> serialize_union_runtime_state(const UnionRuntimeState& state);
bool deserialize_union_runtime_state(const std::vector<uint8_t>& body,
                                     UnionRuntimeState& out,
                                     std::string& error);
Hash write_union_runtime_state(ObjectStore& store,
                               UnionRuntimeState& state,
                               std::string& error);
bool read_union_runtime_state(ObjectStore& store,
                              const Hash& id,
                              UnionRuntimeState& out,
                              std::string& error);

} // namespace cas
