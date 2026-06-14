#include "control_protocol.h"
#include "branch_name.h"
#include "cas_op_bits.h"
#include "daemon.h"
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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

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
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (pos + 4 > json.size()) return false;
                    int value = 0;
                    for (int i = 0; i < 4; i++) {
                        int d = hex_digit(json[pos + i]);
                        if (d < 0) return false;
                        value = (value << 4) | d;
                    }
                    pos += 4;
                    if (value > 0xff) return false;
                    out.push_back((char)value);
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

std::string extract_str(const std::string& json, const std::string& key) {
    auto p = json.find("\"" + key + "\"");
    if (p == std::string::npos) return {};
    p = json.find(':', p);
    if (p == std::string::npos) return {};
    p = json.find('"', p);
    if (p == std::string::npos) return {};
    auto end = json.find('"', p + 1);
    if (end == std::string::npos) return {};
    return json.substr(p + 1, end - p - 1);
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

        std::unique_lock<std::mutex> checkpoint_lk;
        auto br = daemon.branch_by_name_locked(branch_name, checkpoint_lk);
        if (!br) return json_err("unknown branch");

        uint32_t bid = br->branch_id;
        auto r = daemon.checkpoint_mgr().rollback_locked(
            target, br->wt,
            [&daemon, bid] { daemon.invalidate_fhs_for_branch(bid); },
            branch_name);
        if (!r.ok) return json_err(r.error);
        return json_ok_str("rolled_back_to", hash_to_hex(r.rolled_back_to));
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

        if (!daemon.router().register_cgroup(cg, br->branch_id))
            return json_err("cgroup path not found or not a directory");

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

    return json_err("unknown command");
}

} // namespace control_protocol
} // namespace cas
