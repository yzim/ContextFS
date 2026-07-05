#include "agent_state.h"

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace cas {

namespace {

// Percent-encodes the characters that would otherwise collide with the
// line-oriented key=value framing: percent itself, newlines, '=', '|' and
// ASCII space. Every other byte passes through verbatim, so hex hashes and
// ASCII identifiers round-trip cleanly. Same codec as runtime_state.cpp.
std::string percent_encode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (unsigned char c : in) {
        switch (c) {
            case '%': out += "%25"; break;
            case '\n': out += "%0A"; break;
            case '\r': out += "%0D"; break;
            case '=': out += "%3D"; break;
            case '|': out += "%7C"; break;
            case ' ': out += "%20"; break;
            default: out.push_back(static_cast<char>(c)); break;
        }
    }
    return out;
}

// Decodes any %XX escape (case-insensitive hex). Returns an empty string and
// sets `error` on a truncated or malformed escape; on success `error` stays
// empty. Non-'%' bytes pass through unchanged.
std::string percent_decode(const std::string& in, std::string& error) {
    auto hex_val = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '%') {
            if (i + 2 >= in.size()) {
                error = "percent-decode: truncated escape";
                return {};
            }
            int hi = hex_val(in[i + 1]);
            int lo = hex_val(in[i + 2]);
            if (hi < 0 || lo < 0) {
                error = "percent-decode: invalid hex digit";
                return {};
            }
            out.push_back(static_cast<char>((hi << 4) | lo));
            i += 2;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

bool parse_u32(const std::string& s, uint32_t& out, std::string& error) {
    if (s.empty()) { error = "empty integer field"; return false; }
    for (char c : s) {
        if (c < '0' || c > '9') { error = "non-digit in integer field"; return false; }
    }
    errno = 0;
    char* end = nullptr;
    unsigned long long v = std::strtoull(s.c_str(), &end, 10);
    if (errno != 0 || end == s.c_str() || *end != '\0') {
        error = "integer field parse failed";
        return false;
    }
    if (v > UINT32_MAX) { error = "integer field out of u32 range"; return false; }
    out = static_cast<uint32_t>(v);
    return true;
}

bool parse_u64(const std::string& s, uint64_t& out, std::string& error) {
    if (s.empty()) { error = "empty integer field"; return false; }
    for (char c : s) {
        if (c < '0' || c > '9') { error = "non-digit in integer field"; return false; }
    }
    errno = 0;
    char* end = nullptr;
    unsigned long long v = std::strtoull(s.c_str(), &end, 10);
    if (errno != 0 || end == s.c_str() || *end != '\0') {
        error = "integer field parse failed";
        return false;
    }
    out = static_cast<uint64_t>(v);
    return true;
}

// A hash-valued state identifier is valid if it is empty (meaning "unset",
// e.g. a root state's parent) or a full 64-character hex hash. Anything else
// means the writer referenced an object that does not exist, so the reader
// must refuse it rather than silently store a dangling id.
bool is_valid_state_id(const std::string& s) {
    if (s.empty()) return true;
    return is_hex_hash(s);
}

// Decodes the field value and, if non-empty, validates it as a hex hash. The
// `field_name` is folded into the error message so callers can tell which
// identifier was rejected.
bool assign_state_id_field(std::string& dst, const std::string& value,
                           const char* field_name, std::string& error) {
    std::string decoded = percent_decode(value, error);
    if (!error.empty()) return false;
    if (!is_valid_state_id(decoded)) {
        error = std::string("invalid ") + field_name + " hash";
        return false;
    }
    dst = decoded;
    return true;
}

// Dispatches a single decoded key/value pair into `state`. Unknown keys are
// ignored so that future record versions can add fields without breaking
// older readers.
bool assign_field(AgentStateRecord& state, const std::string& key,
                  const std::string& value, std::string& error) {
    if (key == "record_version") {
        return parse_u32(value, state.record_version, error);
    }
    if (key == "parent_state_id") {
        return assign_state_id_field(state.parent_state_id, value,
                                     "parent_state_id", error);
    }
    if (key == "snapshot_base_state_id") {
        return assign_state_id_field(state.snapshot_base_state_id, value,
                                     "snapshot_base_state_id", error);
    }
    if (key == "branch") {
        state.branch = percent_decode(value, error);
        return error.empty();
    }
    if (key == "fs_commit") {
        if (!hex_to_hash_strict(value, state.fs_commit)) {
            error = "invalid fs_commit hash";
            return false;
        }
        return true;
    }
    if (key == "union_state_id") {
        return assign_state_id_field(state.union_state_id, value,
                                     "union_state_id", error);
    }
    if (key == "runtime_id") {
        state.runtime_id = percent_decode(value, error);
        return error.empty();
    }
    if (key == "agent_id") {
        state.agent_id = percent_decode(value, error);
        return error.empty();
    }
    if (key == "sequence") {
        return parse_u64(value, state.sequence, error);
    }
    if (key == "kind") {
        // Controlled enum vocabulary; written without percent-encoding so it
        // is read back verbatim.
        if (!parse_agent_state_kind(value, state.kind)) {
            error = "invalid agent state kind";
            return false;
        }
        return true;
    }
    if (key == "payload_schema") {
        state.payload_schema = percent_decode(value, error);
        return error.empty();
    }
    if (key == "payload_inline") {
        state.payload_inline = percent_decode(value, error);
        return error.empty();
    }
    if (key == "payload_ref") {
        return assign_state_id_field(state.payload_ref, value,
                                     "payload_ref", error);
    }
    if (key == "timestamp_ns") {
        return parse_u64(value, state.timestamp_ns, error);
    }
    if (key == "boundary") {
        if (value == "1") { state.boundary = true; return true; }
        if (value == "0") { state.boundary = false; return true; }
        error = "boundary flag must be 0 or 1";
        return false;
    }
    // Unknown key: ignore for forward compatibility.
    return true;
}

} // namespace

bool parse_agent_state_kind(const std::string& text, AgentStateKind& out) {
    if (text == "session") { out = AgentStateKind::Session; return true; }
    if (text == "execution") { out = AgentStateKind::Execution; return true; }
    if (text == "tool_call") { out = AgentStateKind::ToolCall; return true; }
    if (text == "runtime_resource") { out = AgentStateKind::RuntimeResource; return true; }
    if (text == "external_handle") { out = AgentStateKind::ExternalHandle; return true; }
    if (text == "fs_link") { out = AgentStateKind::FsLink; return true; }
    if (text == "runtime_snapshot") { out = AgentStateKind::RuntimeSnapshot; return true; }
    return false;
}

const char* agent_state_kind_label(AgentStateKind kind) {
    switch (kind) {
        case AgentStateKind::Session: return "session";
        case AgentStateKind::Execution: return "execution";
        case AgentStateKind::ToolCall: return "tool_call";
        case AgentStateKind::RuntimeResource: return "runtime_resource";
        case AgentStateKind::ExternalHandle: return "external_handle";
        case AgentStateKind::FsLink: return "fs_link";
        case AgentStateKind::RuntimeSnapshot: return "runtime_snapshot";
    }
    return "session";
}

std::vector<uint8_t> serialize_agent_state_record(const AgentStateRecord& state) {
    std::string s;
    s += "agentvfs.agent_state.v1\n";
    s += "record_version=";
    s += std::to_string(state.record_version);
    s += "\n";
    s += "parent_state_id=";
    s += percent_encode(state.parent_state_id);
    s += "\n";
    s += "snapshot_base_state_id=";
    s += percent_encode(state.snapshot_base_state_id);
    s += "\n";
    s += "branch=";
    s += percent_encode(state.branch);
    s += "\n";
    s += "fs_commit=";
    s += hash_to_hex(state.fs_commit);
    s += "\n";
    s += "union_state_id=";
    s += percent_encode(state.union_state_id);
    s += "\n";
    s += "runtime_id=";
    s += percent_encode(state.runtime_id);
    s += "\n";
    s += "agent_id=";
    s += percent_encode(state.agent_id);
    s += "\n";
    s += "sequence=";
    s += std::to_string(state.sequence);
    s += "\n";
    s += "kind=";
    s += agent_state_kind_label(state.kind);
    s += "\n";
    s += "payload_schema=";
    s += percent_encode(state.payload_schema);
    s += "\n";
    s += "payload_inline=";
    s += percent_encode(state.payload_inline);
    s += "\n";
    s += "payload_ref=";
    s += percent_encode(state.payload_ref);
    s += "\n";
    s += "timestamp_ns=";
    s += std::to_string(state.timestamp_ns);
    s += "\n";
    s += "boundary=";
    s += (state.boundary ? "1" : "0");
    s += "\n";
    return std::vector<uint8_t>(s.begin(), s.end());
}

bool deserialize_agent_state_record(const std::vector<uint8_t>& body,
                                    AgentStateRecord& out,
                                    std::string& error) {
    error.clear();
    if (body.empty()) {
        error = "empty agent state body";
        return false;
    }

    const std::string header = "agentvfs.agent_state.v1\n";
    if (body.size() < header.size()) {
        error = "agent state body shorter than header";
        return false;
    }
    if (std::string(body.begin(), body.begin() + header.size()) != header) {
        error = "missing or incorrect agent state header";
        return false;
    }

    // Start from a freshly defaulted record so omitted fields keep their
    // defaults and state_id stays empty (it is re-derived by the caller).
    out = AgentStateRecord{};

    size_t pos = header.size();
    while (pos < body.size()) {
        size_t nl = pos;
        while (nl < body.size() && body[nl] != '\n') ++nl;
        std::string line(body.begin() + pos, body.begin() + nl);
        if (!line.empty()) {
            size_t eq = line.find('=');
            if (eq == std::string::npos) {
                error = "line missing '=' separator";
                return false;
            }
            std::string key = line.substr(0, eq);
            std::string value = line.substr(eq + 1);
            if (!assign_field(out, key, value, error)) {
                return false;
            }
        }
        pos = (nl < body.size()) ? nl + 1 : nl;
    }
    return true;
}

Hash write_agent_state_record(ObjectStore& store,
                              AgentStateRecord& state,
                              const std::vector<Hash>& dependency_hashes,
                              bool sync,
                              std::string& error) {
    error.clear();
    // A ZERO_HASH dependency means the caller failed to materialize a
    // referenced object. Refuse to publish a state record that points at
    // nothing, and check before writing the blob so no orphaned state object
    // is left in the store.
    for (const Hash& dep : dependency_hashes) {
        if (dep == ZERO_HASH) {
            error = "agent state dependency hash is ZERO_HASH";
            return ZERO_HASH;
        }
    }
    std::vector<uint8_t> body = serialize_agent_state_record(state);
    AgentStateRecord checked;
    if (!deserialize_agent_state_record(body, checked, error)) {
        error = "invalid agent state record: " + error;
        return ZERO_HASH;
    }
    Hash state_hash = store.write_blob(body);
    if (state_hash == ZERO_HASH) {
        error = store.last_error();
        if (error.empty()) {
            error = "object store write_blob failed";
        }
        return ZERO_HASH;
    }
    if (sync) {
        std::vector<Hash> publish = dependency_hashes;
        publish.push_back(state_hash);
        if (!store.fsync_pending(publish, error)) {
            // state_id is deliberately left unmodified: a ZERO_HASH return
            // signals failure, so the caller's `state` must not carry a
            // half-published identifier that a naive retry would propagate as
            // a stale reference. The blob may still be physically present in
            // the store (an orphan); that is the caller's transactional
            // concern, not this layer's.
            return ZERO_HASH;
        }
    }
    // Only stamp state_id once the entire write (blob + optional sync) has
    // succeeded, so "returns ZERO_HASH" stays equivalent to "`state` is
    // effectively unmodified w.r.t. state_id".
    state.state_id = hash_to_hex(state_hash);
    return state_hash;
}

bool read_agent_state_record(ObjectStore& store,
                             const Hash& id,
                             AgentStateRecord& out,
                             std::string& error) {
    error.clear();
    std::vector<uint8_t> body;
    if (!store.read_blob(id, body)) {
        error = store.last_error();
        if (error.empty()) {
            error = "object store read_blob failed";
        }
        return false;
    }
    if (!deserialize_agent_state_record(body, out, error)) {
        return false;
    }
    out.state_id = hash_to_hex(id);
    return true;
}

} // namespace cas
