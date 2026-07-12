#include "control_protocol.h"
#include "agent_state.h"
#include "agent_state_service.h"
#include "branch_name.h"
#include "cas_op_bits.h"
#include "cgroup_watch.h"
#include "daemon.h"
#include "hash.h"
#include "policy_installer.h"
#include "telemetry_registry.h"

// fnmatch / lstat are POSIX-only and only reached when a policy
// installer is wired up (Linux + eBPF). Skip on Windows where the
// installer is always null and we hit the early-return path.
#ifndef _WIN32
#include <fnmatch.h>
#include <sys/stat.h>
#endif

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>
#ifndef _WIN32
#include <signal.h>
#include <unistd.h>
#endif

namespace cas {
namespace control_protocol {
namespace {

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::string json_ok_str(const std::string& k, const std::string& v) {
    return "{\"ok\":true,\"" + k + "\":\"" + v + "\"}";
}
std::string json_err(const std::string& msg) {
    return "{\"ok\":false,\"error\":\"" + json_escape(msg) + "\"}";
}

std::string json_conflict_response(const std::vector<std::string>& conflicts) {
    std::string out = "{\"ok\":false,\"error\":\"merge conflicts\",\"conflicts\":[";
    for (size_t i = 0; i < conflicts.size(); i++) {
        if (i) out += ",";
        out += "\"";
        out += json_escape(conflicts[i]);
        out += "\"";
    }
    out += "]}";
    return out;
}

bool is_json_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

void skip_json_ws(const std::string& json, size_t& pos) {
    while (pos < json.size() && is_json_ws(json[pos])) pos++;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool append_utf8(uint32_t codepoint, std::string& out) {
    if (codepoint > 0x10ffff) return false;
    if (codepoint >= 0xd800 && codepoint <= 0xdfff) return false;
    if (codepoint <= 0x7f) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ff) {
        out.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0xffff) {
        out.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
    return true;
}

bool parse_json_hex4(const std::string& json, size_t& pos, uint32_t& value) {
    if (pos + 4 > json.size()) return false;
    value = 0;
    for (int i = 0; i < 4; i++) {
        int d = hex_digit(json[pos + i]);
        if (d < 0) return false;
        value = (value << 4) | static_cast<uint32_t>(d);
    }
    pos += 4;
    return true;
}

bool parse_json_string(const std::string& json,
                      size_t& pos,
                      std::string& out) {
    if (pos >= json.size() || json[pos] != '"') return false;
    pos++;
    out.clear();

    while (pos < json.size()) {
        char c = json[pos++];
        if (c == '"') return true;
        if (c == '\\') {
            if (pos >= json.size()) return false;
            char esc = json[pos++];
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    uint32_t value = 0;
                    if (!parse_json_hex4(json, pos, value)) return false;
                    if (value >= 0xd800 && value <= 0xdbff) {
                        if (pos + 2 > json.size() ||
                            json[pos] != '\\' || json[pos + 1] != 'u') {
                            return false;
                        }
                        pos += 2;
                        uint32_t low = 0;
                        if (!parse_json_hex4(json, pos, low)) return false;
                        if (low < 0xdc00 || low > 0xdfff) return false;
                        value = 0x10000 +
                            (((value - 0xd800) << 10) | (low - 0xdc00));
                    } else if (value >= 0xdc00 && value <= 0xdfff) {
                        return false;
                    }
                    if (!append_utf8(value, out)) return false;
                    break;
                }
                default:
                    return false;
            }
        } else {
            if ((unsigned char)c < 0x20) return false;
            out.push_back(c);
        }
    }
    return false;
}

enum class JsonStringLookup {
    Missing,
    Found,
    Malformed,
};

bool consume_json_literal(const std::string& json,
                         size_t& pos,
                         const char* literal) {
    size_t start = pos;
    while (*literal) {
        if (pos >= json.size() || json[pos] != *literal) {
            pos = start;
            return false;
        }
        pos++;
        literal++;
    }
    return true;
}

bool skip_json_number(const std::string& json, size_t& pos) {
    size_t start = pos;
    if (pos < json.size() && json[pos] == '-') pos++;

    if (pos >= json.size() || json[pos] < '0' || json[pos] > '9') {
        pos = start;
        return false;
    }
    if (json[pos] == '0') {
        pos++;
    } else {
        while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') pos++;
    }

    if (pos < json.size() && json[pos] == '.') {
        pos++;
        size_t frac_start = pos;
        while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') pos++;
        if (pos == frac_start) {
            pos = start;
            return false;
        }
    }

    if (pos < json.size() && (json[pos] == 'e' || json[pos] == 'E')) {
        pos++;
        if (pos < json.size() && (json[pos] == '+' || json[pos] == '-')) pos++;
        size_t exp_start = pos;
        while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') pos++;
        if (pos == exp_start) {
            pos = start;
            return false;
        }
    }

    return true;
}

bool skip_flat_json_value(const std::string& json, size_t& pos) {
    skip_json_ws(json, pos);
    if (pos >= json.size()) return false;
    if (json[pos] == '"') {
        std::string ignored;
        return parse_json_string(json, pos, ignored);
    }
    return consume_json_literal(json, pos, "true") ||
           consume_json_literal(json, pos, "false") ||
           consume_json_literal(json, pos, "null") ||
           skip_json_number(json, pos);
}

struct BranchMergeRequest {
    std::string source;
    std::string target;
    std::string label;
};

bool parse_branch_merge_request(const std::string& json,
                                BranchMergeRequest& request) {
    request = {};

    size_t pos = 0;
    skip_json_ws(json, pos);
    if (pos >= json.size() || json[pos] != '{') return false;
    pos++;

    skip_json_ws(json, pos);
    if (pos < json.size() && json[pos] == '}') {
        pos++;
        skip_json_ws(json, pos);
        return pos == json.size();
    }

    while (true) {
        std::string key;
        if (!parse_json_string(json, pos, key)) return false;

        skip_json_ws(json, pos);
        if (pos >= json.size() || json[pos] != ':') return false;
        pos++;
        skip_json_ws(json, pos);

        if (key == "source") {
            if (!parse_json_string(json, pos, request.source)) return false;
        } else if (key == "target") {
            if (!parse_json_string(json, pos, request.target)) return false;
        } else if (key == "label") {
            if (!parse_json_string(json, pos, request.label)) return false;
        } else if (!skip_flat_json_value(json, pos)) {
            return false;
        }

        skip_json_ws(json, pos);
        if (pos >= json.size()) return false;
        if (json[pos] == '}') {
            pos++;
            skip_json_ws(json, pos);
            return pos == json.size();
        }
        if (json[pos] != ',') return false;
        pos++;
        skip_json_ws(json, pos);
        if (pos >= json.size() || json[pos] == '}') return false;
    }
}

JsonStringLookup extract_str_checked(const std::string& json,
                                     const std::string& key,
                                     std::string& out) {
    out.clear();
    size_t pos = 0;
    while (pos < json.size()) {
        if (json[pos] != '"') {
            ++pos;
            continue;
        }
        size_t token_start = pos;
        std::string parsed_key;
        if (!parse_json_string(json, pos, parsed_key)) {
            pos = token_start + 1;
            continue;
        }
        skip_json_ws(json, pos);
        if (pos >= json.size() || json[pos] != ':') {
            continue;
        }
        ++pos;
        skip_json_ws(json, pos);
        if (parsed_key == key) {
            if (!parse_json_string(json, pos, out)) {
                out.clear();
                return JsonStringLookup::Malformed;
            }
            return JsonStringLookup::Found;
        }
        if (!skip_flat_json_value(json, pos)) {
            pos = token_start + 1;
        }
    }
    return JsonStringLookup::Missing;
}

std::string extract_str(const std::string& json, const std::string& key) {
    std::string out;
    if (extract_str_checked(json, key, out) == JsonStringLookup::Found) {
        return out;
    }
    return {};
}

long extract_int(const std::string& json, const std::string& key, long dflt) {
    auto p = json.find("\"" + key + "\"");
    if (p == std::string::npos) return dflt;
    p = json.find(':', p);
    if (p == std::string::npos) return dflt;
    p++;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) p++;
    char* end = nullptr;
    long v = std::strtol(json.c_str() + p, &end, 10);
    if (end == json.c_str() + p) return dflt;
    return v;
}

bool extract_bool(const std::string& json, const std::string& key, bool dflt) {
    auto p = json.find("\"" + key + "\"");
    if (p == std::string::npos) return dflt;
    p = json.find(':', p);
    if (p == std::string::npos) return dflt;
    p++;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) p++;
    if (json.compare(p, 4, "true") == 0) return true;
    if (json.compare(p, 5, "false") == 0) return false;
    return dflt;
}

// Sanitize a client-supplied timeout before it is cast to uint64_t. A negative
// value would wrap to a near-infinite block; an absurdly large value (over an
// hour) likewise pins a handler thread indefinitely. Clamp both to the default
// so the rendezvous times out within a sane bound.
uint64_t sanitize_timeout_ms(long parsed, uint64_t dflt) {
    if (parsed < 0 || parsed > 3600000L) return dflt;
    return (uint64_t)parsed;
}

// Serialize an AgentStateRecord into a flat JSON object. state_id is the
// re-derived CAS hash; fs_commit is rendered as hex (ZERO_HASH -> 64 zeros).
// Used by state.describe / state.latest / state.restore responses.
std::string agent_state_record_to_json(const AgentStateRecord& r) {
    std::string out = "{";
    out += "\"state_id\":\"" + json_escape(r.state_id) + "\"";
    out += ",\"parent_state_id\":\"" + json_escape(r.parent_state_id) + "\"";
    out += ",\"snapshot_base_state_id\":\"" + json_escape(r.snapshot_base_state_id) + "\"";
    out += ",\"branch\":\"" + json_escape(r.branch) + "\"";
    out += ",\"fs_commit\":\"" + json_escape(hash_to_hex(r.fs_commit)) + "\"";
    out += ",\"union_state_id\":\"" + json_escape(r.union_state_id) + "\"";
    out += ",\"runtime_id\":\"" + json_escape(r.runtime_id) + "\"";
    out += ",\"agent_id\":\"" + json_escape(r.agent_id) + "\"";
    out += ",\"sequence\":" + std::to_string(r.sequence);
    out += ",\"kind\":\"";
    out += agent_state_kind_label(r.kind);
    out += "\"";
    out += ",\"payload_schema\":\"" + json_escape(r.payload_schema) + "\"";
    out += ",\"payload_inline\":\"" + json_escape(r.payload_inline) + "\"";
    out += ",\"payload_ref\":\"" + json_escape(r.payload_ref) + "\"";
    out += ",\"timestamp_ns\":" + std::to_string(r.timestamp_ns);
    out += ",\"boundary\":" + std::string(r.boundary ? "true" : "false");
    out += "}";
    return out;
}

bool process_live(long pid) {
#ifndef _WIN32
    if (pid <= 0) return false;
    if (kill((pid_t)pid, 0) == 0) return true;
    return errno == EPERM;
#else
    (void)pid;
    return true;
#endif
}

bool process_group_for_pid(long pid, long& pgid) {
#ifndef _WIN32
    if (pid <= 0) return false;
    errno = 0;
    pid_t value = getpgid((pid_t)pid);
    if (value < 0) return false;
    pgid = static_cast<long>(value);
    return true;
#else
    pgid = pid;
    return pid > 0;
#endif
}

bool parent_for_pid(long pid, long& ppid) {
#if defined(__linux__)
    if (pid <= 0) return false;
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%ld/stat", pid);
    FILE* f = std::fopen(path, "r");
    if (!f) return false;
    char buf[4096];
    bool ok = std::fgets(buf, sizeof(buf), f) != nullptr;
    std::fclose(f);
    if (!ok) return false;
    const char* rp = std::strrchr(buf, ')');
    if (!rp) return false;
    char state = '\0';
    long parsed_ppid = -1;
    if (std::sscanf(rp + 1, " %c %ld", &state, &parsed_ppid) != 2) {
        return false;
    }
    ppid = parsed_ppid;
    return true;
#else
    (void)pid;
    (void)ppid;
    return false;
#endif
}

bool peer_available_or_trusted(const PeerCredentials& peer,
                               std::string& error) {
    if (peer.trusted) return true;
    if (peer.available && peer.pid > 0) return true;
    error = "peer credentials unavailable";
    return false;
}

bool require_peer_pid(const PeerCredentials& peer,
                      long claimed_pid,
                      std::string& error) {
    if (peer.trusted) return true;
    if (!peer_available_or_trusted(peer, error)) return false;
    if (peer.pid != claimed_pid) {
        error = "peer pid mismatch";
        return false;
    }
    return true;
}

bool require_process_group(long pid,
                           long claimed_pgid,
                           std::string& error) {
    if (pid <= 0 || claimed_pgid <= 0) {
        error = "invalid process group";
        return false;
    }
    long actual_pgid = -1;
    if (!process_group_for_pid(pid, actual_pgid)) {
        error = "runtime process not live";
        return false;
    }
    if (actual_pgid != claimed_pgid) {
        error = "process group mismatch";
        return false;
    }
    return true;
}

bool require_launcher_peer(const PeerCredentials& peer,
                           long root_pid,
                           long process_group_id,
                           std::string& error) {
    if (!require_process_group(root_pid, process_group_id, error)) {
        return false;
    }
    if (peer.trusted) return true;
    if (!peer_available_or_trusted(peer, error)) return false;
    long ppid = -1;
    if (!parent_for_pid(root_pid, ppid)) {
        error = "runtime parent not verifiable";
        return false;
    }
    if (ppid != peer.pid) {
        error = "runtime launcher mismatch";
        return false;
    }
    return true;
}

bool require_runtime_peer(const PeerCredentials& peer,
                          long pid,
                          const RuntimeStatus& st,
                          uint64_t generation,
                          std::string& error) {
    if (!require_peer_pid(peer, pid, error)) return false;
    if (peer.trusted) return true;
    if (st.root_pid != pid) {
        error = "runtime peer mismatch";
        return false;
    }
    if (st.generation != generation) {
        error = "runtime generation mismatch";
        return false;
    }
    long actual_pgid = -1;
    if (!process_group_for_pid(pid, actual_pgid)) {
        error = "runtime process not live";
        return false;
    }
    if (actual_pgid != st.active_process_group_id) {
        error = "process group mismatch";
        return false;
    }
    return true;
}

bool require_template_peer(const PeerCredentials& peer,
                           long template_pid,
                           long template_pgid,
                           std::string& error) {
    if (!require_peer_pid(peer, template_pid, error)) return false;
    if (peer.trusted) return true;
    return require_process_group(template_pid, template_pgid, error);
}

bool require_generation_peer(const PeerCredentials& peer,
                             long pid,
                             long pgid,
                             const TemplateStatus& restore_template,
                             std::string& error) {
    if (!require_peer_pid(peer, pid, error)) return false;
    if (!require_process_group(pid, pgid, error)) return false;
    if (peer.trusted) return true;
    long ppid = -1;
    if (!parent_for_pid(pid, ppid)) {
        error = "generation parent not verifiable";
        return false;
    }
    if (ppid != restore_template.template_pid) {
        error = "generation template mismatch";
        return false;
    }
    return true;
}

OpMask op_bit(OpType op) {
    return 1u << static_cast<unsigned>(op);
}

OpMask soft_watch_mask_from_flags(const std::string& flags) {
    OpMask bits = 0;
    if (flags == "all") {
        return op_bit(OpType::Read) |
               op_bit(OpType::Write) |
               op_bit(OpType::Exec) |
               op_bit(OpType::Unlink) |
               op_bit(OpType::Rename) |
               op_bit(OpType::Truncate) |
               op_bit(OpType::Create);
    }
    if (flags.find("read") != std::string::npos) bits |= op_bit(OpType::Read);
    if (flags.find("write") != std::string::npos) bits |= op_bit(OpType::Write);
    if (flags.find("exec") != std::string::npos) bits |= op_bit(OpType::Exec);
    if (flags.find("unlink") != std::string::npos) bits |= op_bit(OpType::Unlink);
    if (flags.find("rename") != std::string::npos) bits |= op_bit(OpType::Rename);
    if (flags.find("truncate") != std::string::npos) bits |= op_bit(OpType::Truncate);
    if (flags.find("create") != std::string::npos) bits |= op_bit(OpType::Create);
    return bits;
}

uint8_t bpf_soft_watch_bits(OpMask mask) {
    uint8_t bits = 0;
    if (mask & op_bit(OpType::Read)) bits |= CAS_OP_READ;
    if (mask & op_bit(OpType::Write)) bits |= CAS_OP_WRITE;
    if (mask & op_bit(OpType::Exec)) bits |= CAS_OP_EXEC;
    if (mask & op_bit(OpType::Unlink)) bits |= CAS_OP_UNLINK;
    if (mask & op_bit(OpType::Rename)) bits |= CAS_OP_RENAME;
    if (mask & op_bit(OpType::Truncate)) bits |= CAS_OP_TRUNCATE;
    if (mask & op_bit(OpType::Create)) bits |= CAS_OP_CREATE;
    return bits;
}

struct ParsedPolicyRule {
    std::string path_pattern;
    OpMask soft_watch = 0;
    uint8_t bpf_bits = 0;
};

bool parse_policy_rules(const std::string& rest,
                        std::vector<ParsedPolicyRule>& parsed,
                        PolicyRules& rules) {
    parsed.clear();
    rules.rules.clear();

    size_t pos = 0;
    while ((pos = rest.find("\"path_pattern\"", pos)) != std::string::npos) {
        size_t end_of_obj = rest.find('}', pos);
        if (end_of_obj == std::string::npos) return false;

        std::string obj = rest.substr(pos, end_of_obj - pos);
        std::string pattern = extract_str(obj, "path_pattern");
        std::string flags = extract_str(obj, "soft_watch");
        OpMask mask = soft_watch_mask_from_flags(flags);

        ParsedPolicyRule parsed_rule{};
        parsed_rule.path_pattern = pattern;
        parsed_rule.soft_watch = mask;
        parsed_rule.bpf_bits = (flags == "all") ? 0xFF : bpf_soft_watch_bits(mask);
        parsed.push_back(parsed_rule);
        rules.rules.push_back({pattern, mask});

        pos = end_of_obj + 1;
    }

    return true;
}

bool parse_rest_with_branch(const std::string& rest_in,
                            std::string& label_out,
                            std::string& branch_out) {
    std::string rest = rest_in;
    size_t start = 0;
    while (start < rest.size() && (rest[start] == ' ' || rest[start] == '\t')) start++;
    rest.erase(0, start);

    std::vector<std::string> toks;
    std::istringstream ts(rest);
    std::string t;
    while (ts >> t) toks.push_back(t);

    branch_out = "main";
    if (!toks.empty() && toks.back().compare(0, 7, "branch=") == 0) {
        branch_out = toks.back().substr(7);
        toks.pop_back();
    }
    for (const auto& tok : toks) {
        if (tok.compare(0, 7, "branch=") == 0) return false;
    }
    label_out.clear();
    for (size_t i = 0; i < toks.size(); i++) {
        if (i) label_out.push_back(' ');
        label_out += toks[i];
    }
    return true;
}

} // anonymous namespace

std::string dispatch(Daemon& daemon, std::string_view line_sv) {
    PeerCredentials trusted;
    trusted.trusted = true;
    trusted.available = true;
    return dispatch(daemon, line_sv, trusted);
}

std::string dispatch(Daemon& daemon,
                     std::string_view line_sv,
                     const PeerCredentials& peer) {
    std::string line(line_sv);
    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;

    if (cmd == "status") {
        Hash cur = daemon.checkpoint_mgr().current_commit();
        PolicyInstaller* pi = daemon.policy_installer();
        bool ebpf_available = pi && pi->available();
        uint64_t drops = ebpf_available ? pi->total_drops() : 0;
        char buf[1024];
        std::snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"version\":\"cas-minimal-0.1\",\"branch\":\"main\","
            "\"commit\":\"%s\",\"telemetry_drops_total\":%llu,"
            "\"session_active\":%s,\"policy_version\":%u,"
            "\"bootstrap_pending\":%s,\"ebpf_available\":%s}",
            hash_to_hex(cur).c_str(), (unsigned long long)drops,
            ebpf_available ? "true" : "false",
            daemon.policy_version(),
            (daemon.bootstrap() && daemon.bootstrap()->pending()) ? "true" : "false",
            ebpf_available ? "true" : "false");
        return buf;
    }

    if (cmd == "telemetry.status") {
        std::string json = "{\"backends\":[";
        TelemetryRegistry* registry = daemon.registry();
        if (registry) {
            bool first = true;
            for (const auto& status : registry->backend_statuses()) {
                if (!first) json += ",";
                first = false;
                json += "{\"name\":\"" + json_escape(status.name) + "\","
                        "\"supported_ops\":" + std::to_string(status.capabilities.supported_ops) + ","
                        "\"pre_op_verdicts\":" +
                        (status.capabilities.pre_op_verdicts ? "true" : "false") + ","
                        "\"started\":" + (status.started ? "true" : "false") + ","
                        "\"status\":\"" + json_escape(status.status) + "\"}";
            }
        }
        json += "]}";
        return json;
    }

    if (cmd == "checkpoint") {
        std::string rest;
        std::getline(iss, rest);

        std::string branch_name, label;
        if (!parse_rest_with_branch(rest, label, branch_name))
            return json_err("ambiguous 'branch=' token in label");

        auto r = daemon.checkpoint_branch_by_name(branch_name, label);
        if (!r.ok) return json_err(r.error);
        return json_ok_str("commit", hash_to_hex(r.commit_hash));
    }

    if (cmd == "rollback") {
        std::string target;
        iss >> target;
        if (target.empty()) return json_err("missing target");

        std::string branch_name = "main";
        std::string tok, extra;
        iss >> tok;
        iss >> extra;
        if (!extra.empty()) return json_err("rollback: unexpected extra token");
        if (!tok.empty()) {
            if (tok.compare(0, 7, "branch=") != 0)
                return json_err("rollback: expected 'branch=<name>' suffix");
            branch_name = tok.substr(7);
        }

        // Branch existence is checked BEFORE target resolution so a missing
        // branch surfaces as "unknown branch" — the original, pre-refactor
        // contract. resolve_target cannot distinguish a bad target from a
        // branch that has no ref at all (both yield ZERO_HASH), so the branch
        // lookup must come first. This single lookup is best-effort and takes
        // only the shared branches_mu_; Daemon::rollback_branch_to_commit then
        // re-resolves the branch under its own exclusive lock (branches_mu_ ->
        // checkpoint_mu) and calls rollback_locked, so there is no double-hold
        // and a concurrent delete_branch between the two lookups still yields a
        // correct "unknown branch" from the helper. The label/hash resolution
        // and "target commit not found" string are NOT preserved for a missing
        // branch: that case is "unknown branch" by historical contract.
        if (!daemon.branch_by_name(branch_name))
            return json_err("unknown branch");

        Hash target_hash = daemon.checkpoint_mgr().resolve_target(target, branch_name);
        if (target_hash == ZERO_HASH) return json_err("target commit not found");

        auto r = daemon.rollback_branch_to_commit(branch_name, target_hash);
        if (!r.ok) return json_err(r.error);
        return json_ok_str("rolled_back_to", hash_to_hex(r.rolled_back_to));
    }

    if (cmd == "state.append") {
        std::string rest; std::getline(iss, rest);
        AgentStateAppendRequest req;
        auto parse_state_string = [&](const std::string& key,
                                      std::string& out,
                                      std::string& error) {
            JsonStringLookup status = extract_str_checked(rest, key, out);
            if (status == JsonStringLookup::Malformed) {
                error = "malformed " +
                    (key == "payload" ? std::string("payload") : key);
                return false;
            }
            return true;
        };
        std::string parse_error;
        if (!parse_state_string("agent_id", req.record.agent_id, parse_error))
            return json_err(parse_error);
        if (req.record.agent_id.empty()) return json_err("missing agent_id");
        // The agent-state `branch` is a LABEL referencing an existing VFS
        // branch (it becomes the leaf of <store>/state/latest/<agent>/<branch>).
        // The service validates it via the [A-Za-z0-9_-]{1,64} rule, so "main"
        // (the default VFS branch) is legal here. Do NOT re-introduce
        // is_valid_branch_name — its reserved-word list does not apply.
        std::string branch;
        if (!parse_state_string("branch", branch, parse_error))
            return json_err(parse_error);
        req.record.branch = branch.empty() ? std::string("main") : branch;
        std::string kind_text;
        if (!parse_state_string("kind", kind_text, parse_error))
            return json_err(parse_error);
        if (kind_text.empty()) {
            // `kind` is optional in the dispatch contract; an omitted kind
            // defaults to Session. The canonical parser is strict (rejects
            // empty), so default here before delegating.
            req.record.kind = AgentStateKind::Session;
        } else if (!parse_agent_state_kind(kind_text, req.record.kind)) {
            return json_err("invalid kind");
        }
        if (!parse_state_string("payload_schema", req.record.payload_schema,
                                parse_error))
            return json_err(parse_error);
        // `payload` is the inline payload alias used by the dispatch contract.
        if (!parse_state_string("payload", req.record.payload_inline,
                                parse_error))
            return json_err(parse_error);
        if (!parse_state_string("payload_ref", req.record.payload_ref,
                                parse_error))
            return json_err(parse_error);
        std::string parent;
        if (!parse_state_string("parent_state_id", parent, parse_error))
            return json_err(parse_error);
        if (!parent.empty()) req.record.parent_state_id = parent;
        std::string snap;
        if (!parse_state_string("snapshot_base_state_id", snap, parse_error))
            return json_err(parse_error);
        if (!snap.empty()) req.record.snapshot_base_state_id = snap;
        std::string fs_hex;
        if (!parse_state_string("fs_commit", fs_hex, parse_error))
            return json_err(parse_error);
        if (!fs_hex.empty()) {
            if (!hex_to_hash_strict(fs_hex, req.record.fs_commit))
                return json_err("invalid fs_commit");
        }
        std::string uid;
        if (!parse_state_string("union_state_id", uid, parse_error))
            return json_err(parse_error);
        if (!uid.empty()) req.record.union_state_id = uid;
        if (!parse_state_string("runtime_id", req.record.runtime_id,
                                parse_error))
            return json_err(parse_error);
        long seq = extract_int(rest, "sequence", 0);
        if (seq < 0) seq = 0;
        req.record.sequence = static_cast<uint64_t>(seq);
        long tns = extract_int(rest, "timestamp_ns", 0);
        if (tns < 0) tns = 0;
        req.record.timestamp_ns = static_cast<uint64_t>(tns);
        if (extract_bool(rest, "boundary", false)) req.record.boundary = true;
        req.sync = extract_bool(rest, "sync", false);
        auto res = daemon.agent_state().append(req);
        if (!res.ok) {
            // Partial success: the state blob was made durable (fsync'd by
            // write_agent_state_record) but the latest-ref publish failed. The
            // service populates state_id + durability in that case; surface
            // them so a controller can record/explain the orphan. Retry is
            // content-addressed, so re-appending the same record yields the
            // same state_id. When state_id is empty (e.g. a validation guard
            // fired before any write), keep the simple error form.
            if (!res.state_id.empty()) {
                std::string out = "{\"ok\":false,\"error\":\"";
                out += json_escape(res.error);
                out += "\",\"state_id\":\"";
                out += json_escape(res.state_id);
                out += "\",\"durability\":\"";
                out += json_escape(res.durability);
                out += "\"}";
                return out;
            }
            return json_err(res.error);
        }
        std::string out = "{\"ok\":true,\"state_id\":\"";
        out += json_escape(res.state_id);
        out += "\",\"durability\":\"";
        out += json_escape(res.durability);
        out += "\"}";
        return out;
    }

    if (cmd == "state.describe") {
        std::string rest; std::getline(iss, rest);
        std::string sid = extract_str(rest, "state_id");
        if (sid.empty()) return json_err("missing state_id");
        auto res = daemon.agent_state().describe(sid);
        if (!res.ok) return json_err(res.error);
        std::string out = "{\"ok\":true,\"state\":";
        out += agent_state_record_to_json(res.record);
        out += "}";
        return out;
    }

    if (cmd == "state.latest") {
        std::string rest; std::getline(iss, rest);
        std::string agent = extract_str(rest, "agent_id");
        if (agent.empty()) return json_err("missing agent_id");
        std::string branch = extract_str(rest, "branch");
        if (branch.empty()) branch = "main";
        auto res = daemon.agent_state().latest(agent, branch);
        if (!res.ok) return json_err(res.error);
        std::string out = "{\"ok\":true,\"state\":";
        out += agent_state_record_to_json(res.record);
        out += "}";
        return out;
    }

    if (cmd == "state.restore") {
        std::string rest; std::getline(iss, rest);
        std::string sid = extract_str(rest, "state_id");
        if (sid.empty()) return json_err("missing state_id");
        std::string mode = extract_str(rest, "mode");
        if (mode.empty()) mode = "session";
        // Shared across full/runtime modes; harmless for session.
        uint64_t timeout_ms = sanitize_timeout_ms(
            extract_int(rest, "timeout_ms", 2000), 2000);

        if (mode == "session") {
            // Bounded semantic chain walk: newest-first back to the snapshot
            // base (or session root). max_depth bounds the walk so a runaway
            // or pathological chain cannot pin the handler thread.
            const size_t max_depth = 256;
            auto res = daemon.agent_state().restore_session(sid, max_depth);
            if (!res.ok) return json_err(res.error);
            std::string out = "{\"ok\":true,\"mode\":\"session\",\"chain\":[";
            for (size_t i = 0; i < res.chain.size(); ++i) {
                if (i) out += ",";
                out += agent_state_record_to_json(res.chain[i]);
            }
            out += "]}";
            return out;
        }

        // full / runtime both need the semantic record first.
        auto d = daemon.agent_state().describe(sid);
        if (!d.ok) return json_err(d.error);

        if (mode == "full") {
            // Full restore = semantic state + FS rollback to fs_commit. The
            // FS anchor is mandatory: a state without fs_commit cannot roll
            // a branch back to anything.
            if (d.record.fs_commit == ZERO_HASH)
                return json_err("state has no fs_commit");
            auto rb = daemon.rollback_branch_to_commit(
                d.record.branch, d.record.fs_commit);
            if (!rb.ok) return json_err(rb.error);
            std::string out = "{\"ok\":true,\"mode\":\"full\",\"state\":";
            out += agent_state_record_to_json(d.record);
            out += ",\"rolled_back_to\":\"";
            out += json_escape(hash_to_hex(rb.rolled_back_to));
            out += "\"}";
            return out;
        }

        if (mode == "runtime") {
            // Runtime restore = semantic state + live-runtime restore. The
            // union_state_id anchor is mandatory. The semantic record is
            // ALWAYS surfaced (even on degraded/partial runtime restore) so
            // the controller never loses the semantic result.
            if (d.record.union_state_id.empty())
                return json_err("state has no union_state_id");
            auto rr = daemon.restore_runtime(d.record.union_state_id, timeout_ms);
            std::string out = "{\"ok\":";
            out += (rr.ok ? "true" : "false");
            out += ",\"mode\":\"runtime\",\"state\":";
            out += agent_state_record_to_json(d.record);
            if (rr.ok) {
                out += ",\"runtime\":{\"ok\":true,\"template_id\":\"";
                out += json_escape(rr.template_id);
                out += "\",\"target_generation\":";
                out += std::to_string(rr.target_generation);
                out += ",\"fs_commit\":\"";
                out += json_escape(hash_to_hex(rr.fs_commit));
                out += "\",\"runtime_id\":\"";
                out += json_escape(rr.runtime_id);
                out += "\",\"restore_eligibility\":\"";
                out += restore_eligibility_to_string(rr.restore_eligibility);
                out += "\"}";
            } else if (!rr.partial.empty()) {
                // Degraded recovery: surface the daemon's partial label.
                out += ",\"runtime\":{\"ok\":false,\"partial\":\"";
                out += json_escape(rr.partial);
                out += "\"}";
            }
            if (!rr.ok) {
                out += ",\"error\":\"";
                out += json_escape(rr.error);
                out += "\"}";
            } else {
                out += "}";
            }
            return out;
        }

        return json_err("invalid mode");
    }

    if (cmd == "session.register") {
        std::string rest; std::getline(iss, rest);
        std::string cg = extract_str(rest, "cgroup_path");
        std::string branch_name = extract_str(rest, "branch");
        if (branch_name.empty()) branch_name = "main";
        long sess   = extract_int(rest, "session_id", (long)daemon.session_id());
        long verbose = extract_int(rest, "telemetry_verbosity", 1);
        if (cg.empty()) return json_err("missing cgroup_path");

        auto br = daemon.branch_by_name(branch_name);
        if (!br) return json_err("unknown branch");

        // Watch BEFORE registering: a deletion racing this handler then
        // fires the eviction after the map insert instead of vanishing
        // between the two calls.
        CgroupWatch* watch = daemon.cgroup_watch();
        bool watched = watch && watch->watch(cg);
        if (!daemon.router().register_cgroup(cg, br->branch_id)) {
            if (watched) watch->unwatch(cg);
            return json_err("cgroup path not found or not a directory");
        }
        if (watch && !watched) {
            // Registered but unwatchable (deleted mid-race, inotify watch
            // limit): fall back to per-resolve leaf revalidation so
            // delete/recreate detection never silently weakens.
            daemon.router().set_leaf_revalidation(true);
            std::fprintf(stderr,
                "agentvfs: cgroup delete watch failed for %s; "
                "per-resolve revalidation re-enabled\n", cg.c_str());
        }

        SessionInfo info{};
        info.cgroup_path = cg;
        info.session_id = (uint64_t)sess;
        info.branch_id = br->branch_id;
        info.policy_version = daemon.policy_version();
        info.verbosity = (uint8_t)verbose;

        TelemetryRegistry* registry = daemon.registry();
        bool registry_ok = true;
        if (registry) {
            registry_ok = registry->register_session(info);
        }

        PolicyInstaller* pi = daemon.policy_installer();
        bool pi_available = pi && pi->available();
        if (!pi_available) return "{\"ok\":true,\"ebpf_available\":false}";
        if (registry) {
            return registry_ok ? "{\"ok\":true}" : json_err("register_session failed");
        }
        bool ok = pi->register_session(cg, (uint64_t)sess, br->branch_id,
                                        daemon.policy_version(), (uint8_t)verbose);
        return ok ? "{\"ok\":true}" : json_err("register_session failed");
    }

    if (cmd == "session.unregister") {
        std::string rest; std::getline(iss, rest);
        std::string cg = extract_str(rest, "cgroup_path");
        if (cg.empty()) return json_err("missing cgroup_path");

        daemon.router().unregister_cgroup(cg);
        if (CgroupWatch* watch = daemon.cgroup_watch()) watch->unwatch(cg);

        TelemetryRegistry* registry = daemon.registry();
        bool registry_ok = true;
        if (registry) {
            registry_ok = registry->unregister_session(cg);
        }

        PolicyInstaller* pi = daemon.policy_installer();
        bool pi_available = pi && pi->available();
        if (!pi_available) return "{\"ok\":true,\"ebpf_available\":false}";
        if (registry) {
            return registry_ok ? "{\"ok\":true}" : json_err("unregister_session failed");
        }
        bool ok = pi->unregister_session(cg);
        return ok ? "{\"ok\":true}" : json_err("unregister_session failed");
    }

    if (cmd == "policy.install") {
        std::string rest; std::getline(iss, rest);
        std::vector<ParsedPolicyRule> parsed_rules;
        PolicyRules rules;
        if (!parse_policy_rules(rest, parsed_rules, rules)) {
            return json_err("malformed policy");
        }

        TelemetryRegistry* registry = daemon.registry();
        if (registry) {
            registry->install_policy(rules);
        }

        PolicyInstaller* pi = daemon.policy_installer();
        bool pi_available = pi && pi->available();
        if (!pi_available) {
            daemon.bump_policy_version();
            return "{\"ok\":true,\"ebpf_available\":false,\"entries_installed\":0}";
        }
        std::vector<PolicyInstaller::Entry> entries;
#ifndef _WIN32
        for (const ParsedPolicyRule& rule : parsed_rules) {
            const std::string& pattern = rule.path_pattern;
            auto star_star = pattern.rfind("/**");
            bool is_recursive = (star_star != std::string::npos &&
                                 star_star + 3 == pattern.size());
            std::string rec_prefix = is_recursive ? pattern.substr(0, star_star) : "";

            std::vector<std::string> matched_vpaths;
            daemon.working_tree().for_each(
                [&](const std::string& vpath, const WorkingTreeEntry& e) {
                    (void)e;
                    std::string rel = vpath.substr(1);
                    bool matched;
                    if (is_recursive) {
                        matched = (rel == rec_prefix)
                               || (rel.rfind(rec_prefix + "/", 0) == 0);
                    } else {
                        matched = (fnmatch(pattern.c_str(), rel.c_str(), FNM_PATHNAME) == 0);
                    }
                    if (matched) matched_vpaths.push_back(vpath);
                });

            for (const auto& vpath : matched_vpaths) {
                std::string mount_abs = daemon.mount_point() + vpath;
                struct stat st;
                if (lstat(mount_abs.c_str(), &st) != 0) continue;
                PolicyInstaller::Entry entry{};
                entry.dev = (uint32_t)st.st_dev;
                entry.ino = (uint64_t)st.st_ino;
                entry.soft_watch_bits = rule.bpf_bits;
                entries.push_back(entry);
                cas::InodeKey ik{(uint64_t)st.st_dev, (uint64_t)st.st_ino, 0};
                daemon.inode_map().set(ik, vpath);
            }
        }
#else
        (void)parsed_rules;
#endif

        size_t installed = pi->install_policy(entries);
        daemon.bump_policy_version();
        char buf[128];
        std::snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"policy_version\":%u,\"entries_installed\":%zu}",
            daemon.policy_version(), installed);
        return buf;
    }

    if (cmd == "branch.create") {
        std::string rest; std::getline(iss, rest);
        std::string name = extract_str(rest, "name");
        std::string from = extract_str(rest, "from");
        if (from.empty()) from = "main";
        if (!is_valid_branch_name(name)) return json_err("invalid branch name");
        if (daemon.branch_by_name(name)) return json_err("branch exists");
        if (!daemon.branch_by_name(from)) return json_err("source branch not found");

        uint32_t id = daemon.create_branch(name, from);
        if (id == UINT32_MAX) return json_err("create_branch failed");

        char buf[96];
        std::snprintf(buf, sizeof(buf), "{\"ok\":true,\"branch_id\":%u}", id);
        return buf;
    }

    if (cmd == "branch.delete") {
        std::string rest; std::getline(iss, rest);
        std::string name = extract_str(rest, "name");
        if (name.empty()) return json_err("missing name");
        if (name == "main") return json_err("cannot delete main");
        auto br = daemon.branch_by_name(name);
        if (!br) return json_err("branch not found");
        if (daemon.router().has_cgroup_for_branch(br->branch_id))
            return json_err("branch has active sessions");
        if (!daemon.delete_branch(name)) return json_err("delete_branch failed");
        return "{\"ok\":true}";
    }

    if (cmd == "branch.merge") {
        std::string rest; std::getline(iss, rest);
        BranchMergeRequest request;
        if (!parse_branch_merge_request(rest, request))
            return json_err("malformed request");
        if (request.source.empty()) return json_err("missing source");
        if (request.target.empty()) return json_err("missing target");

        auto r = daemon.merge_branch(request.source, request.target, request.label);
        if (!r.ok) {
            if (r.error == "merge conflicts") return json_conflict_response(r.conflicts);
            return json_err(r.error);
        }

        std::string out = "{\"ok\":true,\"commit\":\"";
        out += hash_to_hex(r.commit_hash);
        out += "\",\"target\":\"";
        out += json_escape(request.target);
        out += "\",\"source\":\"";
        out += json_escape(request.source);
        out += "\"}";
        return out;
    }

    if (cmd == "branch.list") {
        std::string out = "{\"ok\":true,\"branches\":[";
        auto branches = daemon.list_branches();
        for (size_t i = 0; i < branches.size(); i++) {
            auto& br = branches[i];
            Hash commit = ZERO_HASH;
            daemon.refs().read_ref(br->name, commit);
            char entry[256];
            std::snprintf(entry, sizeof(entry),
                "%s{\"name\":\"%s\",\"branch_id\":%u,\"commit\":\"%s\"}",
                i == 0 ? "" : ",",
                br->name.c_str(), br->branch_id,
                hash_to_hex(commit).c_str());
            out += entry;
        }
        out += "]}";
        return out;
    }

    // -----------------------------------------------------------------
    // Cooperative runtime control commands. The blocking rendezvous
    // (runtime.snapshot / runtime.boundary / runtime.template.ready /
    // runtime.generation.ready) park the per-connection handler thread on
    // the supervisor while Daemon::snapshot_runtime / restore_runtime drive
    // the matching side.
    // -----------------------------------------------------------------
    if (cmd == "runtime.create") {
        std::string rest; std::getline(iss, rest);
        RuntimeCreateRequest req;
        req.runtime_id = extract_str(rest, "runtime_id");
        req.branch = extract_str(rest, "branch");
        if (req.branch.empty()) req.branch = "main";
        req.root_pid = extract_int(rest, "root_pid", -1);
        req.process_group_id = extract_int(rest, "process_group_id", -1);
        req.command_ref = extract_str(rest, "command_ref");
        req.cwd = extract_str(rest, "cwd");
        req.cooperative = extract_bool(rest, "cooperative", false);
        req.control_token = extract_str(rest, "control_token");
        if (req.runtime_id.empty()) return json_err("missing runtime_id");
        if (!daemon.branch_by_name(req.branch)) return json_err("unknown branch");
        if (req.control_token.empty()) return json_err("missing control token");
        if (!process_live(req.root_pid)) return json_err("runtime process not live");
        if (req.process_group_id <= 0) return json_err("invalid process group");
        std::string error;
        if (!require_launcher_peer(peer, req.root_pid, req.process_group_id, error))
            return json_err(error);
        if (!daemon.runtime_supervisor().register_runtime(req, error))
            return json_err(error);
        char buf[160];
        std::snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"runtime_id\":\"%s\",\"generation\":1}",
            json_escape(req.runtime_id).c_str());
        return buf;
    }

    if (cmd == "runtime.status") {
        std::string rest; std::getline(iss, rest);
        std::string rid = extract_str(rest, "runtime_id");
        if (rid.empty()) return json_err("missing runtime_id");
        RuntimeStatus st;
        std::string error;
        if (!daemon.runtime_supervisor().status(rid, st, error))
            return json_err(error);
        std::string out = "{\"ok\":true,\"runtime_id\":\"";
        out += json_escape(st.runtime_id);
        out += "\",\"branch\":\"";
        out += json_escape(st.branch);
        out += "\",\"root_pid\":";
        out += std::to_string(st.root_pid);
        out += ",\"active_process_group_id\":";
        out += std::to_string(st.active_process_group_id);
        out += ",\"generation\":";
        out += std::to_string(st.generation);
        out += ",\"cooperative\":";
        out += st.cooperative ? "true" : "false";
        out += ",\"command_ref\":\"";
        out += json_escape(st.command_ref);
        out += "\",\"restore_eligibility\":\"";
        out += restore_eligibility_to_string(st.restore_eligibility);
        out += "\",\"templates\":[";
        for (size_t i = 0; i < st.templates.size(); i++) {
            if (i) out += ",";
            out += "{\"template_id\":\"";
            out += json_escape(st.templates[i].template_id);
            out += "\",\"alive\":";
            out += st.templates[i].alive ? "true" : "false";
            out += "}";
        }
        out += "]}";
        return out;
    }

    if (cmd == "runtime.list") {
        auto runtimes = daemon.runtime_supervisor().list();
        std::string out = "{\"ok\":true,\"runtimes\":[";
        for (size_t i = 0; i < runtimes.size(); i++) {
            if (i) out += ",";
            const RuntimeStatus& st = runtimes[i];
            out += "{\"runtime_id\":\"";
            out += json_escape(st.runtime_id);
            out += "\",\"branch\":\"";
            out += json_escape(st.branch);
            out += "\",\"generation\":";
            out += std::to_string(st.generation);
            out += ",\"restore_eligibility\":\"";
            out += restore_eligibility_to_string(st.restore_eligibility);
            out += "\"}";
        }
        out += "]}";
        return out;
    }

    if (cmd == "runtime.snapshot") {
        std::string rest; std::getline(iss, rest);
        RuntimeSnapshotRequest req;
        req.runtime_id = extract_str(rest, "runtime_id");
        if (req.runtime_id.empty()) return json_err("missing runtime_id");
        req.boundary_kind = extract_str(rest, "boundary_kind");
        if (req.boundary_kind.empty()) req.boundary_kind = "manual";
        JsonStringLookup agent_state_lookup =
            extract_str_checked(rest, "agent_state_id", req.agent_state_id);
        if (agent_state_lookup == JsonStringLookup::Malformed) {
            return json_err("invalid agent_state_id");
        }
        // Validate a directly-supplied agent_state_id BEFORE any snapshot
        // orchestration starts: it must be empty (meaning "unlinked") or a
        // full 64-character lowercase/uppercase hex hash. Anything else means
        // the controller referenced a state that cannot exist, and snapshot
        // must refuse rather than record a dangling id into the union state.
        if (!req.agent_state_id.empty()) {
            if (!is_hex_hash(req.agent_state_id))
                return json_err("invalid agent_state_id");
        }
        req.timeout_ms = sanitize_timeout_ms(extract_int(rest, "timeout_ms", 1000), 1000);
        auto r = daemon.snapshot_runtime(req);
        if (!r.ok) return json_err(r.error);
        std::string out = "{\"ok\":true,\"union_state_id\":\"";
        out += json_escape(r.union_state_id);
        out += "\",\"fs_commit\":\"";
        out += json_escape(hash_to_hex(r.fs_commit));
        out += "\",\"template_id\":\"";
        out += json_escape(r.template_id);
        out += "\",\"runtime_id\":\"";
        out += json_escape(r.runtime_id);
        out += "\",\"generation\":";
        out += std::to_string(r.generation);
        out += ",\"restore_eligibility\":\"";
        out += restore_eligibility_to_string(r.restore_eligibility);
        out += "\"}";
        return out;
    }

    if (cmd == "runtime.restore") {
        std::string rest; std::getline(iss, rest);
        std::string uid;
        JsonStringLookup uid_lookup =
            extract_str_checked(rest, "union_state_id", uid);
        if (uid_lookup == JsonStringLookup::Malformed)
            return json_err("invalid union_state_id");
        if (uid.empty()) return json_err("missing union_state_id");
        if (!is_hex_hash(uid)) return json_err("invalid union_state_id");
        // Optional restore timeout (default 5000); clamp negatives/absurd values
        // before the uint64_t cast (see sanitize_timeout_ms).
        uint64_t restore_timeout_ms = sanitize_timeout_ms(
            extract_int(rest, "timeout_ms", 5000), 5000);
        auto r = daemon.restore_runtime(uid, restore_timeout_ms);
        if (!r.ok && r.partial.empty()) return json_err(r.error);
        std::string out;
        if (!r.ok) {
            // Partial recovery: surface the daemon's partial label verbatim
            // (e.g. recovery_failed_runtime_resumed / retire_unknown_prior_frozen)
            // so the operator sees the actual end state.
            out = "{\"ok\":false,\"partial\":\"" + json_escape(r.partial) +
                  "\",\"error\":\"";
            out += json_escape(r.error);
            out += "\"}";
            return out;
        }
        out = "{\"ok\":true,\"template_id\":\"";
        out += json_escape(r.template_id);
        out += "\",\"target_generation\":";
        out += std::to_string(r.target_generation);
        out += ",\"fs_commit\":\"";
        out += json_escape(hash_to_hex(r.fs_commit));
        out += "\",\"runtime_id\":\"";
        out += json_escape(r.runtime_id);
        out += "\",\"restore_eligibility\":\"";
        out += restore_eligibility_to_string(r.restore_eligibility);
        out += "\"}";
        return out;
    }

    if (cmd == "runtime.drop") {
        std::string rest; std::getline(iss, rest);
        std::string tid = extract_str(rest, "template_id");
        if (tid.empty()) return json_err("missing template_id");
        std::string error;
        if (!daemon.runtime_supervisor().drop_template(tid, error))
            return json_err(error);
        return "{\"ok\":true}";
    }

    if (cmd == "runtime.boundary") {
        std::string rest; std::getline(iss, rest);
        std::string rid = extract_str(rest, "runtime_id");
        if (rid.empty()) return json_err("missing runtime_id");
        std::string token = extract_str(rest, "control_token");
        int64_t pid = extract_int(rest, "pid", -1);
        uint64_t generation = (uint64_t)extract_int(rest, "generation", 0);
        std::string bkind = extract_str(rest, "boundary_kind");
        if (bkind.empty()) bkind = "manual";
        RuntimeStatus st;
        std::string error;
        if (!daemon.runtime_supervisor().status(rid, st, error))
            return json_err(error);
        if (!require_runtime_peer(peer, pid, st, generation, error))
            return json_err(error);
        std::string boundary_id;
        if (!daemon.runtime_supervisor().observe_boundary(
                rid, token, pid, generation, bkind, boundary_id, error)) {
            // No pending snapshot matches (or runtime unknown). Unknown
            // runtime is a real error; "no pending snapshot" lets the
            // runtime continue without forking a template.
            if (error == "unknown runtime") return json_err(error);
            if (error == "no pending snapshot")
                return "{\"ok\":true,\"action\":\"continue\"}";
            return json_err(error);
        }
        // Block until the daemon's snapshot_runtime releases the boundary.
        RuntimeBoundaryAction action;
        if (!daemon.runtime_supervisor().wait_boundary_action(
                boundary_id, sanitize_timeout_ms(extract_int(rest, "timeout_ms", 5000), 5000),
                action, error))
            return json_err(error);
        if (action.action == "error") return json_err(action.error);
        std::string out = "{\"ok\":true,\"action\":\"snapshot\",\"operation_id\":\"";
        out += json_escape(action.operation_id);
        out += "\",\"template_id\":\"";
        out += json_escape(action.template_id);
        out += "\"}";
        return out;
    }

    if (cmd == "runtime.template.ready") {
        std::string rest; std::getline(iss, rest);
        std::string rid = extract_str(rest, "runtime_id");
        std::string tid = extract_str(rest, "template_id");
        std::string token = extract_str(rest, "control_token");
        if (tid.empty()) return json_err("missing template_id");
        int64_t tpid = extract_int(rest, "template_pid", -1);
        int64_t tpgid = extract_int(rest, "template_process_group_id", -1);
        uint64_t generation = (uint64_t)extract_int(rest, "generation", 0);
        std::string error;
        if (!require_template_peer(peer, tpid, tpgid, error))
            return json_err(error);
        if (!daemon.runtime_supervisor().template_ready(
                rid, token, tid, tpid, tpgid, generation, error))
            return json_err(error);
        // Block until the daemon publishes (or fails) the union state for
        // this template.
        if (!daemon.runtime_supervisor().wait_template_published(
                tid, sanitize_timeout_ms(extract_int(rest, "timeout_ms", 5000), 5000), error))
            return json_err(error);
        return "{\"ok\":true}";
    }

    if (cmd == "runtime.template.poll") {
        std::string rest; std::getline(iss, rest);
        std::string tid = extract_str(rest, "template_id");
        std::string token = extract_str(rest, "control_token");
        if (tid.empty()) return json_err("missing template_id");
        TemplateStatus tmpl;
        std::string error;
        if (!daemon.runtime_supervisor().template_status(tid, tmpl, error))
            return json_err(error);
        if (!require_peer_pid(peer, tmpl.template_pid, error))
            return json_err(error);
        RuntimeTemplateAction action;
        if (!daemon.runtime_supervisor().template_poll(tid, token, action, error))
            return json_err(error);
        std::string out = "{\"ok\":true,\"action\":\"";
        out += json_escape(action.action);
        out += "\"";
        if (action.action == "restore") {
            out += ",\"target_generation\":";
            out += std::to_string(action.target_generation);
        }
        out += "}";
        return out;
    }

    if (cmd == "runtime.generation.ready") {
        std::string rest; std::getline(iss, rest);
        std::string rid = extract_str(rest, "runtime_id");
        if (rid.empty()) return json_err("missing runtime_id");
        std::string token = extract_str(rest, "control_token");
        int64_t pid = extract_int(rest, "pid", -1);
        int64_t apgid = extract_int(rest, "active_process_group_id", -1);
        uint64_t generation = (uint64_t)extract_int(rest, "generation", 0);
        std::string error;
        TemplateStatus restore_template;
        if (!daemon.runtime_supervisor().restore_template_status(
                rid, restore_template, error))
            return json_err(error);
        if (!restore_template.alive) return json_err("no live restore template");
        if (!require_generation_peer(peer, pid, apgid, restore_template, error))
            return json_err(error);
        if (!daemon.runtime_supervisor().generation_ready(
                rid, token, pid, apgid, generation, error))
            return json_err(error);
        return "{\"ok\":true}";
    }

    return json_err("unknown command");
}

} // namespace control_protocol
} // namespace cas
