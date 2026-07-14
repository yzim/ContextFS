#include "agent_state_service.h"
#include "agent_state.h"
#include "object_store.h"
#include "hash.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
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
        '/','t','m','p','/','a','g','e','n','t','v','f','s','-','s','v','c','-',
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
        if (std::strcmp(ent->d_name, ".") == 0 ||
            std::strcmp(ent->d_name, "..") == 0) continue;
        std::string child = path + "/" + ent->d_name;
        struct stat st;
        REQUIRE(lstat(child.c_str(), &st) == 0);
        if (S_ISDIR(st.st_mode)) remove_dir_recursive(child);
        else REQUIRE(std::remove(child.c_str()) == 0);
    }
    closedir(dir);
    REQUIRE(rmdir(path.c_str()) == 0);
}

// True if a regular file exists at `path`. Used to assert the latest ref was
// (or was not) published under <root>/state/latest/... .
static bool file_exists(const std::string& path) {
    struct stat st;
    return lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static bool dir_open_succeeds(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) return false;
    close(fd);
    return true;
}

// A minimal session record: valid agent_id/branch, content-addressable payload,
// and an assignable kind. parent/snapshot_base are set per-test.
static AgentStateRecord base_session_record() {
    AgentStateRecord s;
    s.agent_id = "agent-007";
    s.branch = "dev";  // not "main" to exercise a non-default branch path
    s.kind = AgentStateKind::Session;
    s.payload_inline = "hello";
    s.timestamp_ns = 1700000000123456789ULL;
    return s;
}

// ── append(sync=true) writes a state object and publishes the latest ref ──
static void test_sync_append_publishes_latest_ref() {
    std::string root = make_tmp_dir();
    ObjectStore store(root);
    REQUIRE(store.init_layout());
    AgentStateService svc(store);

    // First establish a durable parent so the sync=true validation guard has a
    // readable parent_state_id and snapshot_base_state_id to anchor against.
    AgentStateRecord anchor = base_session_record();
    anchor.agent_id = "agent-007";
    anchor.payload_inline = "anchor-base";
    // The anchor is itself a snapshot base: its parent/snapshot are empty, so it
    // must be written logical-only.
    AgentStateAppendRequest ar;
    ar.record = anchor;
    ar.sync = false;
    AgentStateAppendResult rr = svc.append(ar);
    REQUIRE(rr.ok);
    REQUIRE(rr.durability == "logical_only");
    std::string anchor_id = rr.state_id;
    REQUIRE(!anchor_id.empty());

    AgentStateRecord rec = base_session_record();
    rec.parent_state_id = anchor_id;
    rec.snapshot_base_state_id = anchor_id;
    rec.payload_inline = "session-state";

    AgentStateAppendRequest req;
    req.record = rec;
    req.sync = true;
    AgentStateAppendResult res = svc.append(req);
    REQUIRE(res.ok);
    REQUIRE(res.error.empty());
    REQUIRE(!res.state_id.empty());
    REQUIRE(res.durability == "durable");

    // The latest ref file exists at <root>/state/latest/<agent>/<branch> and
    // describe-by-id recovers the same payload.
    std::string ref_path =
        root + "/state/latest/" + rec.agent_id + "/" + rec.branch;
    REQUIRE(file_exists(ref_path));

    AgentStateDescribeResult d = svc.describe(res.state_id);
    REQUIRE(d.ok);
    REQUIRE(d.record.state_id == res.state_id);
    REQUIRE(d.record.payload_inline == "session-state");
    REQUIRE(d.record.agent_id == rec.agent_id);

    // latest(agent_id, branch) resolves to the just-published state id.
    AgentStateDescribeResult l = svc.latest(rec.agent_id, rec.branch);
    REQUIRE(l.ok);
    REQUIRE(l.record.state_id == res.state_id);

    remove_dir_recursive(root);
}

// ── durable latest publication repairs a pre-existing state/ directory ──
static void test_durable_append_requires_fsync_of_preexisting_state_parent() {
    std::string root = make_tmp_dir();
    ObjectStore store(root);
    REQUIRE(store.init_layout());
    AgentStateService svc(store);

    // Logical-only append creates <store>/state via append_index_log without
    // publishing a durable latest ref. The later sync=true append must still
    // fsync <store> after observing the pre-existing state/ component.
    AgentStateRecord anchor = base_session_record();
    anchor.payload_inline = "anchor";
    AgentStateAppendRequest ar;
    ar.record = anchor;
    ar.sync = false;
    AgentStateAppendResult rr = svc.append(ar);
    REQUIRE(rr.ok);

    REQUIRE(chmod(root.c_str(), 0111) == 0);
    if (dir_open_succeeds(root)) {
        REQUIRE(chmod(root.c_str(), 0755) == 0);
        remove_dir_recursive(root);
        return;
    }

    AgentStateRecord rec = base_session_record();
    rec.parent_state_id = rr.state_id;
    rec.snapshot_base_state_id = rr.state_id;
    rec.payload_inline = "durable";
    AgentStateAppendRequest req;
    req.record = rec;
    req.sync = true;
    AgentStateAppendResult res = svc.append(req);

    REQUIRE(chmod(root.c_str(), 0755) == 0);
    REQUIRE(!res.ok);
    REQUIRE(!res.error.empty());

    remove_dir_recursive(root);
}

// ── branch="main" is accepted (it is a state-ref LABEL, not a VFS-branch
//    creation) and publishes the latest ref under .../<agent>/main ──
//    The old validator wrongly rejected "main" via is_valid_branch_name's
//    reserved-word list; the plain [A-Za-z0-9_-]{1,64} rule accepts it.
static void test_main_branch_append_accepted() {
    std::string root = make_tmp_dir();
    ObjectStore store(root);
    REQUIRE(store.init_layout());
    AgentStateService svc(store);

    // Snapshot base (logical-only) so the sync=true guard has a readable anchor.
    AgentStateRecord anchor = base_session_record();
    anchor.branch = "main";
    anchor.payload_inline = "main-base";
    AgentStateAppendRequest ar;
    ar.record = anchor;
    ar.sync = false;
    AgentStateAppendResult arr = svc.append(ar);
    REQUIRE(arr.ok);
    std::string anchor_id = arr.state_id;
    REQUIRE(!anchor_id.empty());

    AgentStateRecord rec = base_session_record();
    rec.branch = "main";
    rec.parent_state_id = anchor_id;
    rec.snapshot_base_state_id = anchor_id;
    rec.payload_inline = "main-session";

    AgentStateAppendRequest req;
    req.record = rec;
    req.sync = true;
    AgentStateAppendResult res = svc.append(req);
    REQUIRE(res.ok);
    REQUIRE(res.error.empty());
    REQUIRE(!res.state_id.empty());
    REQUIRE(res.durability == "durable");

    // The latest ref is published exactly at <root>/state/latest/<agent>/main.
    std::string ref_path =
        root + "/state/latest/" + rec.agent_id + "/main";
    REQUIRE(file_exists(ref_path));

    // latest(agent_id, "main") resolves to the just-published state.
    AgentStateDescribeResult l = svc.latest(rec.agent_id, "main");
    REQUIRE(l.ok);
    REQUIRE(l.record.state_id == res.state_id);
    REQUIRE(l.record.branch == "main");
    REQUIRE(l.record.payload_inline == "main-session");

    remove_dir_recursive(root);
}

// ── describe loads the state by state_id ──
static void test_describe_loads_state_by_id() {
    std::string root = make_tmp_dir();
    ObjectStore store(root);
    REQUIRE(store.init_layout());
    AgentStateService svc(store);

    AgentStateRecord rec = base_session_record();
    rec.payload_inline = "describe-me";
    rec.payload_schema = "json+v1";
    rec.sequence = 7;

    AgentStateAppendRequest req;
    req.record = rec;
    req.sync = false;  // logical-only is still describable by id
    AgentStateAppendResult res = svc.append(req);
    REQUIRE(res.ok);
    REQUIRE(!res.state_id.empty());

    AgentStateDescribeResult d = svc.describe(res.state_id);
    REQUIRE(d.ok);
    REQUIRE(d.record.state_id == res.state_id);
    REQUIRE(d.record.payload_inline == "describe-me");
    REQUIRE(d.record.payload_schema == "json+v1");
    REQUIRE(d.record.sequence == 7);

    // Unknown state_id reports a clear error instead of crashing.
    AgentStateDescribeResult miss = svc.describe(std::string(64, '0'));
    REQUIRE(!miss.ok);
    REQUIRE(!miss.error.empty());

    remove_dir_recursive(root);
}

// ── latest returns the last durable state for agent_id and branch ──
static void test_latest_returns_last_durable() {
    std::string root = make_tmp_dir();
    ObjectStore store(root);
    REQUIRE(store.init_layout());
    AgentStateService svc(store);

    // Establish a snapshot base for the chain (logical-only).
    AgentStateRecord base = base_session_record();
    base.payload_inline = "base";
    AgentStateAppendRequest br;
    br.record = base;
    br.sync = false;
    auto br_res = svc.append(br);
    REQUIRE(br_res.ok);

    AgentStateRecord first = base_session_record();
    first.parent_state_id = br_res.state_id;
    first.snapshot_base_state_id = br_res.state_id;
    first.payload_inline = "first";
    AgentStateAppendRequest r1;
    r1.record = first;
    r1.sync = true;
    AgentStateAppendResult res1 = svc.append(r1);
    REQUIRE(res1.ok);

    AgentStateRecord second = base_session_record();
    second.parent_state_id = res1.state_id;
    second.snapshot_base_state_id = br_res.state_id;
    second.payload_inline = "second";
    AgentStateAppendRequest r2;
    r2.record = second;
    r2.sync = true;
    AgentStateAppendResult res2 = svc.append(r2);
    REQUIRE(res2.ok);

    // latest points to the most recently published durable state.
    AgentStateDescribeResult l = svc.latest(first.agent_id, first.branch);
    REQUIRE(l.ok);
    REQUIRE(l.record.state_id == res2.state_id);
    REQUIRE(l.record.payload_inline == "second");

    // A different branch with no durable publish reports not-found.
    AgentStateDescribeResult other = svc.latest(first.agent_id, "nope");
    REQUIRE(!other.ok);

    remove_dir_recursive(root);
}

// ── large payload ref round-trips through payload_ref ──
static void test_large_payload_ref_round_trips() {
    std::string root = make_tmp_dir();
    ObjectStore store(root);
    REQUIRE(store.init_layout());
    AgentStateService svc(store);

    // A "large" payload written through the CAS the same way upper layers will:
    // write_blob, then hand the hash to the service as a payload_ref dependency.
    std::vector<uint8_t> payload(64 * 1024);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>(i % 251);
    }
    Hash payload_hash = store.write_blob(payload);
    REQUIRE(payload_hash != ZERO_HASH);

    // Establish a snapshot base for the sync=true guard.
    AgentStateRecord base = base_session_record();
    base.payload_inline = "base";
    AgentStateAppendRequest br;
    br.record = base;
    br.sync = false;
    auto br_res = svc.append(br);
    REQUIRE(br_res.ok);

    AgentStateRecord rec = base_session_record();
    rec.parent_state_id = br_res.state_id;
    rec.snapshot_base_state_id = br_res.state_id;
    rec.payload_ref = hash_to_hex(payload_hash);

    AgentStateAppendRequest req;
    req.record = rec;
    req.dependency_hashes.push_back(payload_hash);
    req.sync = true;
    AgentStateAppendResult res = svc.append(req);
    REQUIRE(res.ok);
    REQUIRE(res.durability == "durable");

    // describe() recovers payload_ref; the CAS body behind it still matches.
    AgentStateDescribeResult d = svc.describe(res.state_id);
    REQUIRE(d.ok);
    REQUIRE(d.record.payload_ref == hash_to_hex(payload_hash));

    std::vector<uint8_t> read_back;
    Hash pref;
    REQUIRE(hex_to_hash(d.record.payload_ref.c_str(), pref));
    REQUIRE(store.read_blob(pref, read_back));
    REQUIRE(read_back == payload);

    remove_dir_recursive(root);
}

// ── durable append with a new payload ref fsyncs payload and state together ──
static void test_durable_append_fsyncs_payload_and_state() {
    std::string root = make_tmp_dir();
    ObjectStore store(root);
    REQUIRE(store.init_layout());
    AgentStateService svc(store);

    // A fresh payload blob (pending, not yet fsync'd by the caller).
    std::vector<uint8_t> payload{'s','e','c','r','e','t','-','r','e','f'};
    Hash payload_hash = store.write_blob(payload);
    REQUIRE(payload_hash != ZERO_HASH);

    // Snapshot base for the guard.
    AgentStateRecord base = base_session_record();
    base.payload_inline = "base";
    AgentStateAppendRequest br;
    br.record = base;
    br.sync = false;
    auto br_res = svc.append(br);
    REQUIRE(br_res.ok);

    AgentStateRecord rec = base_session_record();
    rec.parent_state_id = br_res.state_id;
    rec.snapshot_base_state_id = br_res.state_id;
    rec.payload_ref = hash_to_hex(payload_hash);

    // sync=true must publish payload+state together via fsync_pending inside
    // write_agent_state_record. The service derives record.payload_ref as a
    // dependency even when the control layer does not pass dependency_hashes.
    // The observable contract: the payload's pending entry is drained by the
    // append, while the pre-existing logical-only anchor remains pending.
    size_t pending_before = store.pending_count();
    REQUIRE(pending_before >= 2);
    AgentStateAppendRequest req;
    req.record = rec;
    req.sync = true;
    AgentStateAppendResult res = svc.append(req);
    REQUIRE(res.ok);
    REQUIRE(res.durability == "durable");
    REQUIRE(!res.state_id.empty());
    REQUIRE(store.pending_count() + 1 == pending_before);

    // State body readable.
    AgentStateDescribeResult d = svc.describe(res.state_id);
    REQUIRE(d.ok);
    // Payload body readable.
    std::vector<uint8_t> read_back;
    REQUIRE(store.read_blob(payload_hash, read_back));
    REQUIRE(read_back == payload);

    // The logical-only path does NOT mark itself durable.
    AgentStateRecord rec2 = base_session_record();
    rec2.payload_inline = "logical";
    AgentStateAppendRequest req2;
    req2.record = rec2;
    req2.sync = false;
    AgentStateAppendResult res2 = svc.append(req2);
    REQUIRE(res2.ok);
    REQUIRE(res2.durability == "logical_only");

    remove_dir_recursive(root);
}

// The persisted dependency list is the same stable, de-duplicated set used
// for durable publication: explicit request dependencies first, followed by a
// derived payload_ref only when it was not already explicit.
static void test_append_persists_effective_dependency_set() {
    std::string root = make_tmp_dir();
    ObjectStore store(root);
    REQUIRE(store.init_layout());
    AgentStateService svc(store);

    Hash explicit_dep = store.write_blob(
        std::vector<uint8_t>{'e','x','p','l','i','c','i','t'});
    Hash payload_hash = store.write_blob(
        std::vector<uint8_t>{'p','a','y','l','o','a','d'});
    REQUIRE(explicit_dep != ZERO_HASH);
    REQUIRE(payload_hash != ZERO_HASH);

    AgentStateRecord base = base_session_record();
    base.payload_inline = "dependency-base";
    AgentStateAppendRequest base_req;
    base_req.record = base;
    AgentStateAppendResult base_res = svc.append(base_req);
    REQUIRE(base_res.ok);

    AgentStateRecord rec = base_session_record();
    rec.parent_state_id = base_res.state_id;
    rec.snapshot_base_state_id = base_res.state_id;
    rec.payload_ref = hash_to_hex(payload_hash);

    AgentStateAppendRequest req;
    req.record = rec;
    req.dependency_hashes = {
        explicit_dep, payload_hash, explicit_dep, payload_hash};
    req.sync = true;
    AgentStateAppendResult res = svc.append(req);
    REQUIRE(res.ok);

    AgentStateDescribeResult described = svc.describe(res.state_id);
    REQUIRE(described.ok);
    REQUIRE(described.record.dependency_hashes.size() == 2);
    REQUIRE(described.record.dependency_hashes[0] == explicit_dep);
    REQUIRE(described.record.dependency_hashes[1] == payload_hash);

    // Both effective dependencies were in the durable publish set. The
    // logical-only base is the sole pending object left behind.
    REQUIRE(store.pending_count() == 1);
    remove_dir_recursive(root);
}

static void test_logical_append_persists_derived_payload_dependency() {
    std::string root = make_tmp_dir();
    ObjectStore store(root);
    REQUIRE(store.init_layout());
    AgentStateService svc(store);

    Hash explicit_dep = store.write_blob(
        std::vector<uint8_t>{'e','x','p'});
    Hash payload_hash = store.write_blob(
        std::vector<uint8_t>{'r','e','f'});
    REQUIRE(explicit_dep != ZERO_HASH);
    REQUIRE(payload_hash != ZERO_HASH);

    AgentStateRecord rec = base_session_record();
    rec.payload_ref = hash_to_hex(payload_hash);
    AgentStateAppendRequest req;
    req.record = rec;
    req.dependency_hashes = {explicit_dep, explicit_dep};
    req.sync = false;
    AgentStateAppendResult res = svc.append(req);
    REQUIRE(res.ok);

    AgentStateDescribeResult described = svc.describe(res.state_id);
    REQUIRE(described.ok);
    REQUIRE(described.record.dependency_hashes.size() == 2);
    REQUIRE(described.record.dependency_hashes[0] == explicit_dep);
    REQUIRE(described.record.dependency_hashes[1] == payload_hash);
    remove_dir_recursive(root);
}

// ── logical-only appends validate hash-shaped fields before writing ──
static void test_sync_false_rejects_malformed_hash_fields_before_write() {
    std::string root = make_tmp_dir();
    ObjectStore store(root);
    REQUIRE(store.init_layout());
    AgentStateService svc(store);

    auto expect_rejected = [&](AgentStateRecord rec) {
        size_t pending_before = store.pending_count();
        AgentStateAppendRequest req;
        req.record = rec;
        req.sync = false;
        AgentStateAppendResult res = svc.append(req);
        REQUIRE(!res.ok);
        REQUIRE(!res.error.empty());
        REQUIRE(res.state_id.empty());
        REQUIRE(store.pending_count() == pending_before);
    };

    {
        AgentStateRecord rec = base_session_record();
        rec.parent_state_id = "not-a-parent-hash";
        expect_rejected(rec);
    }
    {
        AgentStateRecord rec = base_session_record();
        rec.snapshot_base_state_id = std::string(65, 'a');
        expect_rejected(rec);
    }
    {
        AgentStateRecord rec = base_session_record();
        rec.payload_ref = std::string(63, 'a') + "fZ";
        expect_rejected(rec);
    }
    {
        AgentStateRecord rec = base_session_record();
        rec.union_state_id = "not-a-union-hash";
        expect_rejected(rec);
    }

    remove_dir_recursive(root);
}

// ── path-unsafe agent_id such as ../codex is rejected ──
static void test_path_unsafe_agent_id_rejected() {
    std::string root = make_tmp_dir();
    ObjectStore store(root);
    REQUIRE(store.init_layout());
    AgentStateService svc(store);

    AgentStateRecord rec = base_session_record();
    rec.agent_id = "../codex";  // path traversal attempt
    AgentStateAppendRequest req;
    req.record = rec;
    req.sync = false;
    AgentStateAppendResult res = svc.append(req);
    REQUIRE(!res.ok);
    REQUIRE(!res.error.empty());

    // No state/latest directory may have been created for the unsafe id.
    std::string latest_root = root + "/state/latest";
    REQUIRE(!file_exists(latest_root + "/../codex/dev"));
    REQUIRE(!file_exists(latest_root + "/codex/dev"));

    // Other path-unsafe / malformed ids are rejected too. Held in a stable
    // std::vector<std::string> so the loop never reads a dangling pointer (a
    // prior version stored c_str() of a temporary std::string, UB on access).
    const std::vector<std::string> bad_ids = {
        "", "..", "a/b", "cod ex", ".codex", std::string(65, 'a')};
    for (const std::string& bad : bad_ids) {
        AgentStateRecord r = base_session_record();
        r.agent_id = bad;
        AgentStateAppendRequest q;
        q.record = r;
        q.sync = false;
        AgentStateAppendResult s = svc.append(q);
        REQUIRE(!s.ok);
    }

    // A well-formed id still succeeds.
    AgentStateRecord ok_rec = base_session_record();
    ok_rec.agent_id = "codex_42";
    AgentStateAppendRequest ok_req;
    ok_req.record = ok_rec;
    ok_req.sync = false;
    AgentStateAppendResult ok_res = svc.append(ok_req);
    REQUIRE(ok_res.ok);

    remove_dir_recursive(root);
}

// ── missing parent_state_id and snapshot_base_state_id rejected for sync=true ──
static void test_missing_anchors_rejected_for_sync() {
    std::string root = make_tmp_dir();
    ObjectStore store(root);
    REQUIRE(store.init_layout());
    AgentStateService svc(store);

    // Both empty.
    {
        AgentStateRecord rec = base_session_record();
        rec.parent_state_id = "";
        rec.snapshot_base_state_id = "";
        AgentStateAppendRequest req;
        req.record = rec;
        req.sync = true;
        AgentStateAppendResult res = svc.append(req);
        REQUIRE(!res.ok);
        REQUIRE(!res.error.empty());
    }
    // snapshot_base present, parent empty.
    {
        AgentStateRecord rec = base_session_record();
        rec.parent_state_id = "";
        rec.snapshot_base_state_id = std::string(64, 'a');
        AgentStateAppendRequest req;
        req.record = rec;
        req.sync = true;
        AgentStateAppendResult res = svc.append(req);
        REQUIRE(!res.ok);
    }
    // parent present, snapshot_base empty.
    {
        AgentStateRecord rec = base_session_record();
        rec.parent_state_id = std::string(64, 'a');
        rec.snapshot_base_state_id = "";
        AgentStateAppendRequest req;
        req.record = rec;
        req.sync = true;
        AgentStateAppendResult res = svc.append(req);
        REQUIRE(!res.ok);
    }
    // Both present but neither references a real object.
    {
        AgentStateRecord rec = base_session_record();
        rec.parent_state_id = std::string(64, 'a');
        rec.snapshot_base_state_id = std::string(64, 'b');
        AgentStateAppendRequest req;
        req.record = rec;
        req.sync = true;
        AgentStateAppendResult res = svc.append(req);
        REQUIRE(!res.ok);
    }

    // sync=false with the same missing anchors succeeds: logical-only states do
    // not need a durable anchor (e.g. the very first snapshot base).
    {
        AgentStateRecord rec = base_session_record();
        rec.parent_state_id = "";
        rec.snapshot_base_state_id = "";
        AgentStateAppendRequest req;
        req.record = rec;
        req.sync = false;
        AgentStateAppendResult res = svc.append(req);
        REQUIRE(res.ok);
    }

    remove_dir_recursive(root);
}

// ── sync=false writes a describable state but does not publish latest ──
static void test_sync_false_describable_but_no_latest() {
    std::string root = make_tmp_dir();
    ObjectStore store(root);
    REQUIRE(store.init_layout());
    AgentStateService svc(store);

    AgentStateRecord rec = base_session_record();
    rec.payload_inline = "ephemeral";
    AgentStateAppendRequest req;
    req.record = rec;
    req.sync = false;
    AgentStateAppendResult res = svc.append(req);
    REQUIRE(res.ok);
    REQUIRE(res.durability == "logical_only");
    REQUIRE(!res.state_id.empty());

    // Describable by id.
    AgentStateDescribeResult d = svc.describe(res.state_id);
    REQUIRE(d.ok);
    REQUIRE(d.record.payload_inline == "ephemeral");

    // No latest ref published.
    std::string ref_path =
        root + "/state/latest/" + rec.agent_id + "/" + rec.branch;
    REQUIRE(!file_exists(ref_path));
    AgentStateDescribeResult l = svc.latest(rec.agent_id, rec.branch);
    REQUIRE(!l.ok);

    remove_dir_recursive(root);
}

// ── session restore walks a bounded parent chain from a snapshot base ──
static void test_restore_session_walks_bounded_chain() {
    std::string root = make_tmp_dir();
    ObjectStore store(root);
    REQUIRE(store.init_layout());
    AgentStateService svc(store);

    // S = snapshot base (logical-only; it has no parent of its own).
    AgentStateRecord S = base_session_record();
    S.agent_id = "agent-rs";
    S.kind = AgentStateKind::RuntimeSnapshot;
    S.parent_state_id = "";
    S.snapshot_base_state_id = "";
    S.payload_inline = "snapshot-base-S";
    AgentStateAppendRequest sr;
    sr.record = S;
    sr.sync = false;
    auto sr_res = svc.append(sr);
    REQUIRE(sr_res.ok);
    std::string S_id = sr_res.state_id;

    // A -> S, B -> A, C -> B; all durable session states anchored at S.
    auto session_step = [&](const std::string& parent, const std::string& base,
                            const std::string& payload) {
        AgentStateRecord r = base_session_record();
        r.agent_id = "agent-rs";
        r.kind = AgentStateKind::Session;
        r.parent_state_id = parent;
        r.snapshot_base_state_id = base;
        r.payload_inline = payload;
        AgentStateAppendRequest q;
        q.record = r;
        q.sync = true;
        AgentStateAppendResult out = svc.append(q);
        REQUIRE(out.ok);
        return out.state_id;
    };
    std::string A_id = session_step(S_id, S_id, "A");
    std::string B_id = session_step(A_id, S_id, "B");
    std::string C_id = session_step(B_id, S_id, "C");

    // Full chain: target C, walks back to the snapshot base S.
    AgentStateRestoreResult res = svc.restore_session(C_id, 10);
    REQUIRE(res.ok);
    REQUIRE(res.chain.size() == 4);
    REQUIRE(res.chain[0].state_id == C_id);
    REQUIRE(res.chain[1].state_id == B_id);
    REQUIRE(res.chain[2].state_id == A_id);
    REQUIRE(res.chain[3].state_id == S_id);
    REQUIRE(res.chain[3].payload_inline == "snapshot-base-S");

    // max_depth exactly equal to the chain length still succeeds.
    AgentStateRestoreResult res_exact = svc.restore_session(C_id, 4);
    REQUIRE(res_exact.ok);
    REQUIRE(res_exact.chain.size() == 4);

    // max_depth one short fails clearly.
    AgentStateRestoreResult res_short = svc.restore_session(C_id, 3);
    REQUIRE(!res_short.ok);
    REQUIRE(!res_short.error.empty());

    // Restoring the snapshot base itself yields a single-element chain (empty
    // parent termination, no anchor to chase).
    AgentStateRestoreResult res_base = svc.restore_session(S_id, 10);
    REQUIRE(res_base.ok);
    REQUIRE(res_base.chain.size() == 1);
    REQUIRE(res_base.chain[0].state_id == S_id);

    // Restoring from the middle of the chain walks back to S.
    AgentStateRestoreResult res_mid = svc.restore_session(A_id, 10);
    REQUIRE(res_mid.ok);
    REQUIRE(res_mid.chain.size() == 2);
    REQUIRE(res_mid.chain[0].state_id == A_id);
    REQUIRE(res_mid.chain[1].state_id == S_id);

    // Unknown state_id fails clearly.
    AgentStateRestoreResult res_missing =
        svc.restore_session(std::string(64, '0'), 10);
    REQUIRE(!res_missing.ok);
    REQUIRE(!res_missing.error.empty());

    remove_dir_recursive(root);
}

int main() {
    std::printf("test_agent_state_service:\n");
    test_sync_append_publishes_latest_ref();
    test_durable_append_requires_fsync_of_preexisting_state_parent();
    test_main_branch_append_accepted();
    test_describe_loads_state_by_id();
    test_latest_returns_last_durable();
    test_large_payload_ref_round_trips();
    test_durable_append_fsyncs_payload_and_state();
    test_append_persists_effective_dependency_set();
    test_logical_append_persists_derived_payload_dependency();
    test_sync_false_rejects_malformed_hash_fields_before_write();
    test_path_unsafe_agent_id_rejected();
    test_missing_anchors_rejected_for_sync();
    test_sync_false_describable_but_no_latest();
    test_restore_session_walks_bounded_chain();
    std::printf("PASS cas_test_agent_state_service\n");
    return 0;
}
