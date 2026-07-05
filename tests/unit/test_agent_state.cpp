#include "agent_state.h"
#include "object_store.h"
#include "hash.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace cas;

#define REQUIRE(expr) do { if (!(expr)) { \
    std::fprintf(stderr, "REQUIRE failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
    std::abort(); } } while (0)

static std::string make_tmp_dir() {
    std::vector<char> templ{
        '/','t','m','p','/','a','g','e','n','t','v','f','s','-','a','s','-',
        'X','X','X','X','X','X','\0'};
    char* path = mkdtemp(templ.data());
    REQUIRE(path != nullptr);
    return std::string(path);
}

static void remove_dir_recursive(const std::string& path) {
    DIR* dir = opendir(path.c_str());
    if (!dir) return;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (std::strcmp(ent->d_name, ".") == 0 || std::strcmp(ent->d_name, "..") == 0) continue;
        std::string child = path + "/" + ent->d_name;
        struct stat st;
        REQUIRE(lstat(child.c_str(), &st) == 0);
        if (S_ISDIR(st.st_mode)) remove_dir_recursive(child);
        else REQUIRE(std::remove(child.c_str()) == 0);
    }
    closedir(dir);
    REQUIRE(rmdir(path.c_str()) == 0);
}

static Hash tagged_hash(uint8_t tag) {
    Hash h{};
    h[0] = tag;
    return h;
}

// Counts every regular object file under <root>/objects, walking the
// "<2-hex-shard>/<62-hex-name>" layout the ObjectStore uses. Used to lock in
// the ordering guarantee that a guard-rejected write leaves no state blob in
// the store.
static size_t count_objects(const std::string& root) {
    std::string objects = root + "/objects";
    size_t count = 0;
    DIR* dir = opendir(objects.c_str());
    if (!dir) return 0;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (std::strcmp(ent->d_name, ".") == 0 ||
            std::strcmp(ent->d_name, "..") == 0) continue;
        std::string shard = objects + "/" + ent->d_name;
        struct stat st;
        REQUIRE(lstat(shard.c_str(), &st) == 0);
        if (!S_ISDIR(st.st_mode)) continue;
        DIR* subdir = opendir(shard.c_str());
        if (!subdir) continue;
        struct dirent* sent;
        while ((sent = readdir(subdir)) != nullptr) {
            if (std::strcmp(sent->d_name, ".") == 0 ||
                std::strcmp(sent->d_name, "..") == 0) continue;
            ++count;
        }
        closedir(subdir);
    }
    closedir(dir);
    return count;
}

static AgentStateRecord sample_state() {
    AgentStateRecord s;
    s.record_version = 1;
    s.parent_state_id = std::string(64, 'a');
    s.snapshot_base_state_id = std::string(64, 'b');
    s.branch = "main";
    s.fs_commit = tagged_hash(0x41);
    s.union_state_id = std::string(64, 'c');
    s.runtime_id = "rt-0001";
    s.agent_id = "agent-007";
    s.sequence = 42;
    s.kind = AgentStateKind::ToolCall;
    s.payload_schema = "json+v1";
    // Carries every character the percent codec must escape to round-trip
    // through the key=value framing: '%' (escape introducer), '='
    // (key=value separator), '|' (kept escaped for safety), space and a
    // newline (line framing). Proves the encode/decode symmetry.
    s.payload_inline = "tool=git diff 100% done|ok\nline2";
    s.payload_ref = std::string(64, 'd');
    s.timestamp_ns = 1700000000123456789ULL;
    s.boundary = true;
    return s;
}

static void test_roundtrip_preserves_fields() {
    AgentStateRecord in = sample_state();
    std::vector<uint8_t> body = serialize_agent_state_record(in);
    REQUIRE(!body.empty());
    const std::string header = "agentvfs.agent_state.v1\n";
    REQUIRE(body.size() > header.size());
    REQUIRE(std::string(body.begin(), body.begin() + header.size()) == header);

    AgentStateRecord out;
    std::string error;
    REQUIRE(deserialize_agent_state_record(body, out, error));
    REQUIRE(error.empty());
    REQUIRE(out.record_version == 1);
    REQUIRE(out.parent_state_id == in.parent_state_id);
    REQUIRE(out.snapshot_base_state_id == in.snapshot_base_state_id);
    REQUIRE(out.branch == "main");
    REQUIRE(out.fs_commit == in.fs_commit);
    REQUIRE(out.union_state_id == in.union_state_id);
    REQUIRE(out.runtime_id == "rt-0001");
    REQUIRE(out.agent_id == "agent-007");
    REQUIRE(out.sequence == 42);
    REQUIRE(out.kind == AgentStateKind::ToolCall);
    REQUIRE(out.payload_schema == "json+v1");
    REQUIRE(out.payload_inline == in.payload_inline);
    REQUIRE(out.payload_inline == "tool=git diff 100% done|ok\nline2");
    REQUIRE(out.payload_ref == in.payload_ref);
    REQUIRE(out.timestamp_ns == in.timestamp_ns);
    REQUIRE(out.timestamp_ns == 1700000000123456789ULL);
    REQUIRE(out.boundary == true);
}

static void test_kind_enum_roundtrip() {
    const AgentStateKind kinds[] = {
        AgentStateKind::Session,
        AgentStateKind::Execution,
        AgentStateKind::ToolCall,
        AgentStateKind::RuntimeResource,
        AgentStateKind::ExternalHandle,
        AgentStateKind::FsLink,
        AgentStateKind::RuntimeSnapshot,
    };
    for (AgentStateKind k : kinds) {
        AgentStateRecord s;
        s.kind = k;
        std::vector<uint8_t> body = serialize_agent_state_record(s);
        AgentStateRecord out;
        std::string error;
        REQUIRE(deserialize_agent_state_record(body, out, error));
        REQUIRE(error.empty());
        REQUIRE(out.kind == k);
    }
}

static void test_serialization_omits_state_id() {
    AgentStateRecord in = sample_state();
    // Pretend a state_id was assigned upstream; it must never be serialized
    // into the blob body (it is derived from the blob hash on write/read).
    in.state_id = std::string(64, 'f');
    std::vector<uint8_t> body = serialize_agent_state_record(in);
    std::string text(body.begin(), body.end());
    // No line may begin with "state_id=".
    REQUIRE(text.find("\nstate_id=") == std::string::npos);

    AgentStateRecord out;
    std::string error;
    REQUIRE(deserialize_agent_state_record(body, out, error));
    // Because state_id is never serialized, the reader leaves it empty; only
    // read_agent_state_record() re-derives it from the CAS blob hash.
    REQUIRE(out.state_id.empty());
}

static void test_cas_write_assigns_state_id_from_blob_hash() {
    std::string root = make_tmp_dir();
    ObjectStore store(root);
    REQUIRE(store.init_layout());

    AgentStateRecord s = sample_state();
    std::vector<Hash> deps;  // no dependencies
    std::string error;
    Hash id = write_agent_state_record(store, s, deps, /*sync=*/false, error);
    REQUIRE(id != ZERO_HASH);
    REQUIRE(error.empty());
    REQUIRE(s.state_id == hash_to_hex(id));
    REQUIRE(!s.state_id.empty());

    AgentStateRecord out;
    REQUIRE(read_agent_state_record(store, id, out, error));
    REQUIRE(out.state_id == hash_to_hex(id));
    REQUIRE(out.fs_commit == s.fs_commit);
    REQUIRE(out.payload_inline == s.payload_inline);
    remove_dir_recursive(root);
}

static void test_changing_payload_inline_changes_hash() {
    std::string root = make_tmp_dir();
    ObjectStore store(root);
    REQUIRE(store.init_layout());

    AgentStateRecord a = sample_state();
    AgentStateRecord b = sample_state();
    b.payload_inline = "tool=git diff 50% done|ok\nline2";  // different content
    std::vector<Hash> deps;
    std::string error;
    Hash ha = write_agent_state_record(store, a, deps, /*sync=*/false, error);
    Hash hb = write_agent_state_record(store, b, deps, /*sync=*/false, error);
    REQUIRE(ha != ZERO_HASH);
    REQUIRE(hb != ZERO_HASH);
    REQUIRE(ha != hb);
    remove_dir_recursive(root);
}

static void test_invalid_hash_fields_rejected() {
    const std::string header = "agentvfs.agent_state.v1\n";
    auto try_parse = [](const std::string& body) {
        AgentStateRecord out;
        std::string error;
        bool ok = deserialize_agent_state_record(
            std::vector<uint8_t>(body.begin(), body.end()), out, error);
        return std::make_pair(ok, error);
    };

    // Invalid fs_commit (not 64 hex chars).
    {
        std::string body = header + "fs_commit=not-a-hash\n";
        auto r = try_parse(body);
        REQUIRE(!r.first);
        REQUIRE(!r.second.empty());
    }
    // Invalid parent_state_id (non-empty, not a hash).
    {
        std::string body = header + "parent_state_id=nope\n";
        auto r = try_parse(body);
        REQUIRE(!r.first);
        REQUIRE(!r.second.empty());
    }
    // Invalid union_state_id (non-hex characters).
    {
        std::string body = header + "union_state_id=ZZZZ\n";
        auto r = try_parse(body);
        REQUIRE(!r.first);
        REQUIRE(!r.second.empty());
    }
    // Invalid payload_ref (too short).
    {
        std::string body = header + "payload_ref=short\n";
        auto r = try_parse(body);
        REQUIRE(!r.first);
        REQUIRE(!r.second.empty());
    }
    // Invalid snapshot_base_state_id.
    {
        std::string body = header + "snapshot_base_state_id=bad\n";
        auto r = try_parse(body);
        REQUIRE(!r.first);
        REQUIRE(!r.second.empty());
    }
    // Overlong hashes are invalid; accepting the first 64 chars would silently
    // truncate a caller-supplied id.
    {
        std::string body = header + "parent_state_id=" + std::string(65, 'a') + "\n";
        auto r = try_parse(body);
        REQUIRE(!r.first);
        REQUIRE(!r.second.empty());
    }
    // sscanf("%2x") accepts a single hex digit before a non-hex byte; require
    // every one of the 64 characters to be hex.
    {
        std::string almost_hex = std::string(63, 'a') + "fZ";
        std::string body = header + "payload_ref=" + almost_hex + "\n";
        auto r = try_parse(body);
        REQUIRE(!r.first);
        REQUIRE(!r.second.empty());
    }
    // Empty parent_state_id is valid (root state has no parent).
    {
        std::string body = header + "parent_state_id=\n";
        auto r = try_parse(body);
        REQUIRE(r.first);
        REQUIRE(r.second.empty());
    }
}

static void test_unknown_keys_ignored() {
    std::string body = "agentvfs.agent_state.v1\n"
        "record_version=1\n"
        "branch=main\n"
        "future_field=ignored-value\n"
        "another_unknown=42\n"
        "agent_id=agent-x\n";
    AgentStateRecord out;
    std::string error;
    REQUIRE(deserialize_agent_state_record(
        std::vector<uint8_t>(body.begin(), body.end()), out, error));
    REQUIRE(error.empty());
    REQUIRE(out.agent_id == "agent-x");
    REQUIRE(out.branch == "main");
}

static void test_invalid_kind_and_boundary_rejected() {
    const std::string header = "agentvfs.agent_state.v1\n";
    auto try_parse = [](const std::string& body) {
        AgentStateRecord out;
        std::string error;
        bool ok = deserialize_agent_state_record(
            std::vector<uint8_t>(body.begin(), body.end()), out, error);
        return std::make_pair(ok, error);
    };
    // Unknown kind string.
    {
        std::string body = header + "kind=not_a_real_kind\n";
        auto r = try_parse(body);
        REQUIRE(!r.first);
        REQUIRE(!r.second.empty());
    }
    // Bad boundary flag (must be 0 or 1).
    {
        std::string body = header + "boundary=maybe\n";
        auto r = try_parse(body);
        REQUIRE(!r.first);
        REQUIRE(!r.second.empty());
    }
}

static void test_write_fails_when_dependency_is_zero_hash() {
    std::string root = make_tmp_dir();
    ObjectStore store(root);
    REQUIRE(store.init_layout());

    AgentStateRecord s = sample_state();
    std::vector<Hash> deps;
    deps.push_back(tagged_hash(0x01));
    deps.push_back(ZERO_HASH);  // a dependency was never materialized
    std::string error;
    std::string state_id_before = s.state_id;
    Hash id = write_agent_state_record(store, s, deps, /*sync=*/true, error);
    REQUIRE(id == ZERO_HASH);
    REQUIRE(!error.empty());
    // The ZERO_HASH-dependency guard fires before write_blob, so no state blob
    // may be left in the store, and the caller's record must not carry a
    // half-published state_id from the rejected attempt.
    REQUIRE(count_objects(root) == 0);
    REQUIRE(s.state_id == state_id_before);
    remove_dir_recursive(root);
}

static void test_write_rejects_record_with_malformed_hash_field() {
    std::string root = make_tmp_dir();
    ObjectStore store(root);
    REQUIRE(store.init_layout());

    AgentStateRecord s = sample_state();
    s.payload_ref = "not-a-hex-ref";
    std::vector<Hash> deps;
    std::string error;
    Hash id = write_agent_state_record(store, s, deps, /*sync=*/false, error);
    REQUIRE(id == ZERO_HASH);
    REQUIRE(!error.empty());
    REQUIRE(s.state_id.empty());
    REQUIRE(count_objects(root) == 0);

    remove_dir_recursive(root);
}

static void test_sync_write_publishes_without_error() {
    std::string root = make_tmp_dir();
    ObjectStore store(root);
    REQUIRE(store.init_layout());

    // fsync_pending() opens each dependency's object file, so the dependency
    // must be a real object already materialized in the store. Write a small
    // blob and use its returned hash as the dependency.
    std::vector<uint8_t> dep_body{'p','a','y','l','o','a','d'};
    Hash dep_hash = store.write_blob(dep_body);
    REQUIRE(dep_hash != ZERO_HASH);

    AgentStateRecord s = sample_state();
    s.payload_ref = hash_to_hex(dep_hash);  // state references the dependency
    std::vector<Hash> deps;
    deps.push_back(dep_hash);
    std::string error;
    Hash id = write_agent_state_record(store, s, deps, /*sync=*/true, error);
    REQUIRE(id != ZERO_HASH);
    REQUIRE(error.empty());
    REQUIRE(s.state_id == hash_to_hex(id));
    remove_dir_recursive(root);
}

int main() {
    std::printf("test_agent_state:\n");
    test_roundtrip_preserves_fields();
    test_kind_enum_roundtrip();
    test_serialization_omits_state_id();
    test_cas_write_assigns_state_id_from_blob_hash();
    test_changing_payload_inline_changes_hash();
    test_invalid_hash_fields_rejected();
    test_unknown_keys_ignored();
    test_invalid_kind_and_boundary_rejected();
    test_write_fails_when_dependency_is_zero_hash();
    test_write_rejects_record_with_malformed_hash_field();
    test_sync_write_publishes_without_error();
    std::printf("PASS cas_test_agent_state\n");
    return 0;
}
