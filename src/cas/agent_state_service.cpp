#include "agent_state_service.h"

#include "hash.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace cas {

namespace {

// Creates a single directory if it does not already exist. Returns true on
// success or EEXIST. Used to materialize the <store>/state/latest/<agent_id>/
// hierarchy one component at a time.
bool mkdir_one(const std::string& path, std::string& error) {
    if (::mkdir(path.c_str(), 0777) == 0) return true;
    if (errno == EEXIST) {
        struct stat st;
        if (::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            return true;
        }
        error = "mkdir(" + path + ") found a non-directory";
        return false;
    }
    error = std::string("mkdir(") + path + ") failed: " + std::strerror(errno);
    return false;
}

// fsync an open directory file descriptor. Used to make the rename of a latest
// ref durable on the parent directory.
bool fsync_dir(int fd) {
    while (::fsync(fd) != 0) {
        if (errno == EINTR) continue;
        return false;
    }
    return true;
}

bool fsync_dir_at(const std::string& dir_path, std::string& error) {
    int fd = ::open(dir_path.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        error = std::string("open dir ") + dir_path + " failed: " +
                std::strerror(errno);
        return false;
    }
    bool ok = fsync_dir(fd);
    ::close(fd);
    if (!ok) {
        error = std::string("fsync dir ") + dir_path + " failed: " +
                std::strerror(errno);
        return false;
    }
    return true;
}

bool ensure_child_dir_durable(const std::string& parent,
                              const std::string& child_name,
                              std::string& error) {
    std::string child = parent + "/" + child_name;
    if (!mkdir_one(child, error)) return false;
    return fsync_dir_at(parent, error);
}

bool parse_optional_hash(const std::string& value,
                         const char* field_name,
                         Hash& out,
                         std::string& error) {
    out = ZERO_HASH;
    if (value.empty()) return true;
    if (!hex_to_hash_strict(value, out)) {
        error = std::string("invalid ") + field_name + " hash";
        return false;
    }
    return true;
}

// Atomically publishes the latest ref for an agent+branch. Writes the state id
// to a temp file in the ref's parent directory, fsyncs the file, renames it
// into place, then fsyncs the parent directory so the rename survives a crash.
bool publish_latest_ref(const std::string& store_root,
                        const std::string& agent_id,
                        const std::string& branch,
                        const std::string& state_id_hex,
                        std::string& error) {
    std::string latest_root = store_root + "/state/latest";
    std::string agent_dir = latest_root + "/" + agent_id;
    if (!ensure_child_dir_durable(store_root, "state", error)) return false;
    if (!ensure_child_dir_durable(store_root + "/state", "latest", error)) {
        return false;
    }
    if (!ensure_child_dir_durable(latest_root, agent_id, error)) return false;

    std::string target = agent_dir + "/" + branch;

    // Create a uniquely-named temp file inside the same directory so the rename
    // is guaranteed atomic on the same filesystem. Build the template in a
    // named string first so the vector's begin/end iterators refer to the SAME
    // object (two separate temporaries would yield iterators into different
    // strings and the distance — i.e. the vector size — would be undefined).
    std::string tmpl_str = agent_dir + "/.ref-" + branch + "-XXXXXX";
    std::vector<char> tmpl(tmpl_str.begin(), tmpl_str.end());
    tmpl.push_back('\0');
    int fd = ::mkstemp(tmpl.data());
    if (fd < 0) {
        error = std::string("mkstemp failed: ") + std::strerror(errno);
        return false;
    }
    std::string tmp_path = tmpl.data();

    const char* data = state_id_hex.c_str();
    size_t remaining = state_id_hex.size();
    ssize_t n = 0;
    bool write_ok = true;
    while (remaining > 0) {
        n = ::write(fd, data + (state_id_hex.size() - remaining), remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            write_ok = false;
            break;
        }
        remaining -= static_cast<size_t>(n);
    }
    if (write_ok) {
        while (::fsync(fd) != 0) {
            if (errno != EINTR) {
                write_ok = false;
                break;
            }
        }
    }
    ::close(fd);

    if (!write_ok) {
        ::unlink(tmp_path.c_str());
        error = std::string("write/fsync latest ref failed: ") +
                std::strerror(errno);
        return false;
    }

    if (::rename(tmp_path.c_str(), target.c_str()) != 0) {
        ::unlink(tmp_path.c_str());
        error = std::string("rename latest ref failed: ") +
                std::strerror(errno);
        return false;
    }

    // Make the rename durable on the parent directory. State blobs are already
    // fsync'd by write_agent_state_record; the ref file's parent dir fsync is
    // the last step that binds the ref to the durable state.
    if (!fsync_dir_at(agent_dir, error)) return false;

    return true;
}

// Appends a discovery line to <store>/state/index.log. Best-effort: any I/O
// failure is swallowed because state blobs (and the latest refs) remain the
// authoritative source of truth. Format: "<state_id> <agent_id> <branch> <ts>".
void append_index_log(const std::string& store_root,
                      const std::string& agent_id,
                      const std::string& branch,
                      const std::string& state_id_hex,
                      uint64_t timestamp_ns) {
    std::string state_dir = store_root + "/state";
    std::string err;
    mkdir_one(state_dir, err);  // ignore failure: the open() below will report
    std::string log_path = state_dir + "/index.log";
    FILE* fp = std::fopen(log_path.c_str(), "a");
    if (!fp) return;
    std::fprintf(fp, "%s %s %s %llu\n", state_id_hex.c_str(), agent_id.c_str(),
                 branch.c_str(),
                 static_cast<unsigned long long>(timestamp_ns));
    std::fclose(fp);
}

// Trims trailing whitespace (including a final newline) from a ref body.
std::string trim_trailing(const std::string& s) {
    size_t end = s.size();
    while (end > 0) {
        char c = s[end - 1];
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') {
            --end;
        } else {
            break;
        }
    }
    return s.substr(0, end);
}

} // namespace

bool is_valid_state_ref_component(const std::string& name) {
    // Plain character rule [A-Za-z0-9_-]{1,64}. Used for BOTH agent_id and the
    // agent-state `branch` label: each becomes a path component under
    // <store>/state/latest/<agent_id>/<branch>, and `branch` here is a LABEL
    // referencing an existing VFS branch (not a branch being created), so the
    // branch-name reserved-word list deliberately does NOT apply. The
    // path-safety guarantee comes from rejecting '/', '.', space and any
    // character outside [A-Za-z0-9_-].
    if (name.empty() || name.size() > 64) return false;
    for (char c : name) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' ||
              c == '_')) {
            return false;
        }
    }
    return true;
}

AgentStateService::AgentStateService(ObjectStore& store) : store_(store) {}

AgentStateAppendResult
AgentStateService::append(const AgentStateAppendRequest& req) {
    AgentStateAppendResult result;

    // Validate path components BEFORE any file is created under
    // <store>/state/latest. Both agent_id and branch are state-ref LABEL
    // components validated with the plain character rule [A-Za-z0-9_-]{1,64};
    // in particular the branch-name reserved-word list does NOT apply (the
    // agent-state `branch` references an existing VFS branch such as "main",
    // it does not create one).
    if (!is_valid_state_ref_component(req.record.agent_id)) {
        result.error = "invalid agent_id";
        return result;
    }
    if (!is_valid_state_ref_component(req.record.branch)) {
        result.error = "invalid branch name";
        return result;
    }

    Hash parent_hash;
    Hash base_hash;
    Hash payload_hash;
    Hash union_hash;
    if (!parse_optional_hash(req.record.parent_state_id, "parent_state_id",
                             parent_hash, result.error)) {
        return result;
    }
    if (!parse_optional_hash(req.record.snapshot_base_state_id,
                             "snapshot_base_state_id", base_hash,
                             result.error)) {
        return result;
    }
    if (!parse_optional_hash(req.record.payload_ref, "payload_ref",
                             payload_hash, result.error)) {
        return result;
    }
    if (!parse_optional_hash(req.record.union_state_id, "union_state_id",
                             union_hash, result.error)) {
        return result;
    }

    // sync=true anchors: both ids must be present and reference readable state
    // objects. sync=false skips this so a brand-new snapshot base (which has no
    // parent of its own) can still be written logical-only.
    if (req.sync) {
        if (req.record.parent_state_id.empty()) {
            result.error = "sync=true requires a non-empty parent_state_id";
            return result;
        }
        if (req.record.snapshot_base_state_id.empty()) {
            result.error =
                "sync=true requires a non-empty snapshot_base_state_id";
            return result;
        }
        AgentStateRecord anchor;
        std::string read_err;
        if (!read_agent_state_record(store_, parent_hash, anchor, read_err)) {
            result.error =
                "parent_state_id does not reference a readable state: " +
                read_err;
            return result;
        }
        if (!read_agent_state_record(store_, base_hash, anchor, read_err)) {
            result.error =
                "snapshot_base_state_id does not reference a readable state: " +
                read_err;
            return result;
        }
        if (!req.record.payload_ref.empty()) {
            std::vector<uint8_t> payload_body;
            if (!store_.read_blob(payload_hash, payload_body)) {
                result.error =
                    "payload_ref does not reference a readable blob";
                return result;
            }
        }
    }

    std::vector<Hash> dependency_hashes = req.dependency_hashes;
    if (req.sync && !req.record.payload_ref.empty() &&
        std::find(dependency_hashes.begin(), dependency_hashes.end(),
                  payload_hash) == dependency_hashes.end()) {
        dependency_hashes.push_back(payload_hash);
    }

    // Publish the state blob (and, for sync=true, fsync deps+state together)
    // via Task 1's helper. On failure state_id is left unmodified by contract.
    AgentStateRecord rec = req.record;
    std::string write_err;
    Hash state_hash = write_agent_state_record(store_, rec,
                                               dependency_hashes, req.sync,
                                               write_err);
    if (state_hash == ZERO_HASH) {
        result.error =
            write_err.empty() ? "write_agent_state_record failed" : write_err;
        return result;
    }
    result.state_id = hash_to_hex(state_hash);
    result.durability = req.sync ? "durable" : "logical_only";

    // Publish the latest ref only for durable appends. If publishing fails the
    // state is already durable in the CAS, so we surface the partial-success
    // (state_id set, ok=false) rather than silently dropping the ref.
    if (req.sync) {
        std::string ref_err;
        if (!publish_latest_ref(store_.root(), req.record.agent_id,
                                req.record.branch, result.state_id, ref_err)) {
            result.error =
                "state published but latest ref failed: " + ref_err;
            return result;
        }
    }

    // Best-effort discovery metadata. Never fails the append.
    append_index_log(store_.root(), req.record.agent_id, req.record.branch,
                     result.state_id, rec.timestamp_ns);

    result.ok = true;
    return result;
}

AgentStateDescribeResult
AgentStateService::describe(const std::string& state_id) const {
    AgentStateDescribeResult result;
    Hash id;
    if (!hex_to_hash_strict(state_id, id)) {
        result.error = "invalid state_id";
        return result;
    }
    std::string read_err;
    if (!read_agent_state_record(store_, id, result.record, read_err)) {
        result.error =
            read_err.empty() ? "read_agent_state_record failed" : read_err;
        return result;
    }
    result.ok = true;
    return result;
}

AgentStateDescribeResult
AgentStateService::latest(const std::string& agent_id,
                          const std::string& branch) const {
    AgentStateDescribeResult result;
    // Validate before touching the filesystem: a malformed agent_id or branch
    // must never be concatenated into a path. Both use the same plain character
    // rule (see append()).
    if (!is_valid_state_ref_component(agent_id)) {
        result.error = "invalid agent_id";
        return result;
    }
    if (!is_valid_state_ref_component(branch)) {
        result.error = "invalid branch name";
        return result;
    }
    std::string ref_path =
        store_.root() + "/state/latest/" + agent_id + "/" + branch;
    FILE* fp = std::fopen(ref_path.c_str(), "r");
    if (!fp) {
        result.error =
            "no latest ref for agent_id='" + agent_id + "' branch='" + branch +
            "'";
        return result;
    }
    std::string body;
    char buf[128];
    size_t got = 0;
    while ((got = std::fread(buf, 1, sizeof(buf), fp)) > 0) {
        body.append(buf, got);
    }
    std::fclose(fp);

    std::string state_id_hex = trim_trailing(body);
    Hash id;
    if (!hex_to_hash_strict(state_id_hex, id)) {
        result.error = "latest ref contents are not a valid state_id";
        return result;
    }
    std::string read_err;
    if (!read_agent_state_record(store_, id, result.record, read_err)) {
        result.error =
            read_err.empty() ? "read_agent_state_record failed" : read_err;
        return result;
    }
    result.ok = true;
    return result;
}

AgentStateRestoreResult
AgentStateService::restore_session(const std::string& state_id,
                                   size_t max_depth) const {
    AgentStateRestoreResult result;

    Hash start;
    if (!hex_to_hash_strict(state_id, start)) {
        result.error = "invalid state_id";
        return result;
    }

    // The anchor is the target record's snapshot_base_state_id. The walk
    // terminates successfully when it reaches the record whose id equals the
    // anchor (the snapshot base), or when it reaches a record with an empty
    // parent (session root). max_depth bounds the number of records visited;
    // exceeding it fails clearly so an unbounded or cyclic chain can never hang
    // a caller.
    std::string anchor;
    Hash cur = start;
    size_t visited = 0;
    while (true) {
        if (visited >= max_depth) {
            result.error =
                "session chain exceeds max_depth before reaching a snapshot "
                "base or root";
            return result;
        }
        AgentStateRecord rec;
        std::string read_err;
        if (!read_agent_state_record(store_, cur, rec, read_err)) {
            result.error = read_err.empty() ? "parent_state_id not readable"
                                            : read_err;
            return result;
        }
        if (visited == 0) {
            anchor = rec.snapshot_base_state_id;
        }
        result.chain.push_back(rec);
        ++visited;

        bool reached_snapshot_base =
            !anchor.empty() && rec.state_id == anchor;
        bool reached_root = rec.parent_state_id.empty();
        if (reached_snapshot_base || reached_root) {
            result.ok = true;
            return result;
        }

        Hash next;
        if (!hex_to_hash_strict(rec.parent_state_id, next)) {
            result.error = "invalid parent_state_id hash in chain";
            return result;
        }
        cur = next;
    }
}

} // namespace cas
