#include "runtime_state.h"

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace cas {

namespace {

// Percent-encodes the characters that would otherwise collide with the
// line-oriented key=value framing or the warning field separator: percent
// itself, newlines, '=', '|' and ASCII space. Every other byte is passed
// through verbatim, so hex hashes and ASCII identifiers round-trip cleanly.
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

bool assign_warning(UnionRuntimeState& state, const std::string& value, std::string& error) {
    // value = encoded_kind | flag | encoded_description
    size_t bar1 = value.find('|');
    if (bar1 == std::string::npos) {
        error = "warning line missing field separators";
        return false;
    }
    size_t bar2 = value.find('|', bar1 + 1);
    if (bar2 == std::string::npos) {
        error = "warning line missing description separator";
        return false;
    }
    ResourceWarning w;
    w.kind = percent_decode(value.substr(0, bar1), error);
    if (!error.empty()) return false;
    std::string flag = value.substr(bar1 + 1, bar2 - (bar1 + 1));
    if (flag == "1") {
        w.blocker = true;
    } else if (flag == "0") {
        w.blocker = false;
    } else {
        error = "warning blocker flag must be 0 or 1";
        return false;
    }
    w.description = percent_decode(value.substr(bar2 + 1), error);
    if (!error.empty()) return false;
    state.warnings.push_back(std::move(w));
    return true;
}

// Dispatches a single decoded key/value pair into `state`. Unknown keys are
// ignored so that future record versions can add fields without breaking older
// readers.
bool assign_field(UnionRuntimeState& state, const std::string& key,
                  const std::string& value, std::string& error) {
    if (key == "record_version") {
        return parse_u32(value, state.record_version, error);
    }
    if (key == "parent_union_state_id") {
        state.parent_union_state_id = percent_decode(value, error);
        return error.empty();
    }
    if (key == "branch") {
        state.branch = percent_decode(value, error);
        return error.empty();
    }
    if (key == "fs_commit") {
        if (!hex_to_hash(value.c_str(), state.fs_commit)) {
            error = "invalid fs_commit hash";
            return false;
        }
        return true;
    }
    if (key == "agent_state_id") {
        state.agent_state_id = percent_decode(value, error);
        return error.empty();
    }
    if (key == "runtime_id") {
        state.runtime_id = percent_decode(value, error);
        return error.empty();
    }
    if (key == "runtime_generation") {
        return parse_u64(value, state.runtime_generation, error);
    }
    if (key == "template_id") {
        state.template_id = percent_decode(value, error);
        return error.empty();
    }
    if (key == "template_kind") {
        state.template_kind = percent_decode(value, error);
        return error.empty();
    }
    if (key == "boundary_kind") {
        state.boundary_kind = percent_decode(value, error);
        return error.empty();
    }
    if (key == "command_ref") {
        state.command_ref = percent_decode(value, error);
        return error.empty();
    }
    if (key == "resource_manifest_ref") {
        state.resource_manifest_ref = percent_decode(value, error);
        return error.empty();
    }
    if (key == "timestamp_ns") {
        return parse_u64(value, state.timestamp_ns, error);
    }
    if (key == "warning") {
        return assign_warning(state, value, error);
    }
    // Unknown key: ignore for forward compatibility.
    return true;
}

} // namespace

std::string restore_eligibility_to_string(RestoreEligibility eligibility) {
    switch (eligibility) {
        case RestoreEligibility::LiveRuntimeRestorable: return "live_runtime_restorable";
        case RestoreEligibility::FsOnly: return "fs_only";
        case RestoreEligibility::MetadataOnly: return "metadata_only";
    }
    return "metadata_only";
}

bool restore_eligibility_from_string(const std::string& text, RestoreEligibility& out) {
    if (text == "live_runtime_restorable") {
        out = RestoreEligibility::LiveRuntimeRestorable;
        return true;
    }
    if (text == "fs_only") {
        out = RestoreEligibility::FsOnly;
        return true;
    }
    if (text == "metadata_only") {
        out = RestoreEligibility::MetadataOnly;
        return true;
    }
    return false;
}

std::vector<uint8_t> serialize_union_runtime_state(const UnionRuntimeState& state) {
    std::string s;
    s += "agentvfs.union_runtime_state.v1\n";
    s += "record_version=";
    s += std::to_string(state.record_version);
    s += "\n";
    s += "parent_union_state_id=";
    s += percent_encode(state.parent_union_state_id);
    s += "\n";
    s += "branch=";
    s += percent_encode(state.branch);
    s += "\n";
    s += "fs_commit=";
    s += hash_to_hex(state.fs_commit);
    s += "\n";
    s += "agent_state_id=";
    s += percent_encode(state.agent_state_id);
    s += "\n";
    s += "runtime_id=";
    s += percent_encode(state.runtime_id);
    s += "\n";
    s += "runtime_generation=";
    s += std::to_string(state.runtime_generation);
    s += "\n";
    s += "template_id=";
    s += percent_encode(state.template_id);
    s += "\n";
    s += "template_kind=";
    s += percent_encode(state.template_kind);
    s += "\n";
    s += "boundary_kind=";
    s += percent_encode(state.boundary_kind);
    s += "\n";
    s += "command_ref=";
    s += percent_encode(state.command_ref);
    s += "\n";
    s += "resource_manifest_ref=";
    s += percent_encode(state.resource_manifest_ref);
    s += "\n";
    s += "timestamp_ns=";
    s += std::to_string(state.timestamp_ns);
    s += "\n";
    for (const ResourceWarning& w : state.warnings) {
        s += "warning=";
        s += percent_encode(w.kind);
        s += "|";
        s += (w.blocker ? "1" : "0");
        s += "|";
        s += percent_encode(w.description);
        s += "\n";
    }
    return std::vector<uint8_t>(s.begin(), s.end());
}

bool deserialize_union_runtime_state(const std::vector<uint8_t>& body,
                                     UnionRuntimeState& out,
                                     std::string& error) {
    error.clear();
    if (body.empty()) {
        error = "empty union runtime state body";
        return false;
    }

    const std::string header = "agentvfs.union_runtime_state.v1\n";
    if (body.size() < header.size()) {
        error = "union runtime state body shorter than header";
        return false;
    }
    if (std::string(body.begin(), body.begin() + header.size()) != header) {
        error = "missing or incorrect union runtime state header";
        return false;
    }

    // Start from a freshly defaulted record so omitted fields keep their
    // defaults and warnings do not accumulate across calls.
    out = UnionRuntimeState{};

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

Hash write_union_runtime_state(ObjectStore& store,
                               UnionRuntimeState& state,
                               std::string& error) {
    error.clear();
    std::vector<uint8_t> body = serialize_union_runtime_state(state);
    Hash hash = store.write_blob(body);
    if (hash == ZERO_HASH) {
        error = store.last_error();
        if (error.empty()) {
            error = "object store write_blob failed";
        }
        return ZERO_HASH;
    }
    state.union_state_id = hash_to_hex(hash);
    return hash;
}

bool read_union_runtime_state(ObjectStore& store,
                              const Hash& id,
                              UnionRuntimeState& out,
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
    if (!deserialize_union_runtime_state(body, out, error)) {
        return false;
    }
    out.union_state_id = hash_to_hex(id);
    return true;
}

} // namespace cas
