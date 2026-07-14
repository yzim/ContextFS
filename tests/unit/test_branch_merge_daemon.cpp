#include "control_protocol.h"
#include "control_socket.h"
#include "daemon.h"
#include "commit.h"
#include "hash.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

using namespace cas;

#define REQUIRE(expr) do { if (!(expr)) { \
    std::fprintf(stderr, "REQUIRE failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
    std::abort(); } } while (0)

static std::string make_tmp_dir(const char* suffix) {
    std::string templ = std::string("/tmp/agentvfs-branch-merge-daemon-") + suffix + "-XXXXXX";
    std::vector<char> buf(templ.begin(), templ.end());
    buf.push_back('\0');
    char* path = mkdtemp(buf.data());
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
        if (lstat(child.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) remove_dir_recursive(child);
        else std::remove(child.c_str());
    }
    closedir(dir);
    rmdir(path.c_str());
}

static void write_all(int fd, const std::string& body) {
    size_t off = 0;
    while (off < body.size()) {
        ssize_t n = write(fd, body.data() + off, body.size() - off);
        REQUIRE(n > 0);
        off += (size_t)n;
    }
}

static void write_file(const std::string& path, const std::string& body) {
    FILE* f = std::fopen(path.c_str(), "wb");
    REQUIRE(f != nullptr);
    REQUIRE(std::fwrite(body.data(), 1, body.size(), f) == body.size());
    REQUIRE(std::fclose(f) == 0);
}

static void wait_for_bootstrap(Bootstrap& bs) {
    for (int i = 0; i < 100 && bs.pending(); i++) usleep(10000);
    REQUIRE(!bs.pending());
}

static std::string control_request(const std::string& socket_path,
                                   const std::string& line) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    REQUIRE(socket_path.size() < sizeof(addr.sun_path));
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
    REQUIRE(connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0);

    write_all(fd, line + "\n");

    std::string response;
    char ch;
    while (read(fd, &ch, 1) == 1) {
        if (ch == '\n') break;
        response.push_back(ch);
    }
    close(fd);
    return response;
}

static bool is_hex_char(char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static void require_hex_commit_field(const std::string& response) {
    std::string prefix = "\"commit\":\"";
    size_t pos = response.find(prefix);
    REQUIRE(pos != std::string::npos);
    pos += prefix.size();
    REQUIRE(pos + 64 < response.size());
    for (size_t i = 0; i < 64; i++) {
        REQUIRE(is_hex_char(response[pos + i]));
    }
    REQUIRE(response[pos + 64] == '"');
}

struct TestDaemon {
    std::string root;
    std::string source;
    std::string mount;
    std::string store_root;
    Daemon daemon;

    explicit TestDaemon(const char* suffix)
        : root(make_tmp_dir(suffix))
        , source(root + "/src")
        , mount(root + "/mnt")
        , store_root(root + "/store")
        , daemon(source, mount, store_root) {
        REQUIRE(mkdir(source.c_str(), 0755) == 0);
        REQUIRE(mkdir(mount.c_str(), 0755) == 0);
        REQUIRE(daemon.initialize());
    }

    ~TestDaemon() {
        remove_dir_recursive(root);
    }
};

static Hash put_blob(Daemon& d,
                     WorkingTree& wt,
                     const std::string& path,
                     const std::string& body) {
    Hash blob = d.store().write_blob(reinterpret_cast<const uint8_t*>(body.data()), body.size());
    REQUIRE(blob != ZERO_HASH);
    wt.insert(path, {EntryKind::Blob, blob, 0100644});
    return blob;
}

static Hash tagged_hash(uint8_t tag) {
    Hash h{};
    h[0] = tag;
    return h;
}

static std::string blob_text(Daemon& d, const WorkingTree& wt, const std::string& path) {
    auto entry = wt.lookup(path);
    REQUIRE(entry.has_value());
    REQUIRE(entry->kind == EntryKind::Blob);
    std::vector<uint8_t> body;
    REQUIRE(d.store().read_blob(entry->hash, body));
    return std::string(body.begin(), body.end());
}

static CommitData read_commit(Daemon& d, const Hash& h) {
    std::vector<uint8_t> body;
    REQUIRE(d.store().read_commit(h, body));
    CommitData cd;
    REQUIRE(deserialize_commit(body, cd));
    return cd;
}

static CheckpointResult checkpoint_branch(
    Daemon& d,
    const std::shared_ptr<BranchContext>& br,
    const std::string& label) {
    uint32_t bid = br->branch_id;
    return d.checkpoint_mgr().checkpoint(
        label, d.session_id(), d.policy_version(),
        br->checkpoint_mu, br->wt,
        [&d, bid, br] { return d.flush_fhs_for_branch(bid, br->wt); },
        br->name);
}

static std::shared_ptr<BranchContext> create_feature_from_main(Daemon& d) {
    uint32_t id = d.create_branch("feature", "main");
    REQUIRE(id != UINT32_MAX);
    auto feature = d.branch(id);
    REQUIRE(feature != nullptr);
    REQUIRE(feature->name == "feature");
    return feature;
}

static uint64_t allocate_dirty_fh(Daemon& d,
                                  const std::shared_ptr<BranchContext>& br,
                                  const std::string& path,
                                  const std::string& body,
                                  bool stale = false) {
    auto state = std::make_unique<FhState>();
    state->path = path;
    state->branch_id = br->branch_id;
    state->base_blob = ZERO_HASH;
    state->base_size = 0;
    state->base_cache_loaded = true;
    state->write_buf = std::make_unique<WriteBuffer>(ZERO_HASH, 0);
    state->write_buf->write(0, reinterpret_cast<const uint8_t*>(body.data()), body.size());
    state->stale = stale;
    return d.allocate_fh(std::move(state));
}

static void test_clean_merge_advances_target_only_and_writes_two_parent_commit() {
    TestDaemon env("clean");
    Daemon& d = env.daemon;
    auto main = d.main_branch();

    put_blob(d, main->wt, "/base.txt", "base");
    auto base_cp = checkpoint_branch(d, main, "base");
    REQUIRE(base_cp.ok);

    auto feature = create_feature_from_main(d);
    put_blob(d, main->wt, "/target.txt", "target");
    put_blob(d, feature->wt, "/source.txt", "source");

    BranchMergeResult result = d.merge_branch("feature", "main", "ship feature");

    REQUIRE(result.ok);
    REQUIRE(result.error.empty());
    REQUIRE(result.conflicts.empty());
    REQUIRE(result.commit_hash != ZERO_HASH);

    Hash main_ref = ZERO_HASH;
    Hash feature_ref = ZERO_HASH;
    REQUIRE(d.refs().read_ref("main", main_ref));
    REQUIRE(d.refs().read_ref("feature", feature_ref));
    REQUIRE(main_ref == result.commit_hash);
    REQUIRE(feature_ref != result.commit_hash);

    CommitData merge = read_commit(d, result.commit_hash);
    REQUIRE(merge.parents.size() == 2);
    REQUIRE(merge.parents[1] == feature_ref);
    REQUIRE(merge.label == "ship feature");
    CommitData target_premerge = read_commit(d, merge.parents[0]);
    CommitData source_premerge = read_commit(d, merge.parents[1]);
    REQUIRE(target_premerge.label == "pre-merge feature into main target");
    REQUIRE(source_premerge.label == "pre-merge feature into main source");

    REQUIRE(blob_text(d, main->wt, "/base.txt") == "base");
    REQUIRE(blob_text(d, main->wt, "/target.txt") == "target");
    REQUIRE(blob_text(d, main->wt, "/source.txt") == "source");
    REQUIRE(blob_text(d, feature->wt, "/base.txt") == "base");
    REQUIRE(blob_text(d, feature->wt, "/source.txt") == "source");
    REQUIRE(!feature->wt.lookup("/target.txt").has_value());

    std::printf("  PASS test_clean_merge_advances_target_only_and_writes_two_parent_commit\n");
}

static void test_merge_deleted_source_root_file_is_not_reingested_by_lazy_bootstrap() {
    TestDaemon env("delete-bootstrap-live");
    write_file(env.source + "/victim.txt", "source-root");

    Daemon& d = env.daemon;
    auto main = d.main_branch();
    Bootstrap bs(env.source, d.store(), main->wt, d.inode_map(),
                 main->checkpoint_mu);
    REQUIRE(bs.ensure_path("/victim.txt"));

    auto base_cp = checkpoint_branch(d, main, "base");
    REQUIRE(base_cp.ok);
    auto feature = create_feature_from_main(d);
    feature->wt.remove("/victim.txt");

    BranchMergeResult result = d.merge_branch("feature", "main", "delete victim");
    REQUIRE(result.ok);
    REQUIRE(!main->wt.lookup("/victim.txt").has_value());

    bs.ensure_path("/victim.txt");
    REQUIRE(!main->wt.lookup("/victim.txt").has_value());

    std::printf("  PASS test_merge_deleted_source_root_file_is_not_reingested_by_lazy_bootstrap\n");
}

static void test_merge_deleted_source_root_file_survives_restart_and_background_bootstrap() {
    std::string root = make_tmp_dir("delete-bootstrap-restart");
    std::string source = root + "/src";
    std::string mount = root + "/mnt";
    std::string store_root = root + "/store";
    REQUIRE(mkdir(source.c_str(), 0755) == 0);
    REQUIRE(mkdir(mount.c_str(), 0755) == 0);
    write_file(source + "/victim.txt", "source-root");

    {
        Daemon d(source, mount, store_root);
        REQUIRE(d.initialize());
        auto main = d.main_branch();
        Bootstrap bs(source, d.store(), main->wt, d.inode_map(),
                     main->checkpoint_mu);
        REQUIRE(bs.ensure_path("/victim.txt"));

        auto base_cp = checkpoint_branch(d, main, "base");
        REQUIRE(base_cp.ok);
        auto feature = create_feature_from_main(d);
        feature->wt.remove("/victim.txt");

        BranchMergeResult result = d.merge_branch("feature", "main", "delete victim");
        REQUIRE(result.ok);
        REQUIRE(!main->wt.lookup("/victim.txt").has_value());
    }

    {
        Daemon reloaded(source, mount, store_root);
        REQUIRE(reloaded.initialize());
        auto main = reloaded.main_branch();
        REQUIRE(!main->wt.lookup("/victim.txt").has_value());

        Bootstrap bs(source, reloaded.store(), main->wt, reloaded.inode_map(),
                     main->checkpoint_mu);
        bs.start_background();
        wait_for_bootstrap(bs);
        bs.stop_background();
        REQUIRE(!main->wt.lookup("/victim.txt").has_value());

        bs.ensure_path("/victim.txt");
        REQUIRE(!main->wt.lookup("/victim.txt").has_value());
    }

    remove_dir_recursive(root);

    std::printf("  PASS test_merge_deleted_source_root_file_survives_restart_and_background_bootstrap\n");
}

static void test_conflict_returns_paths_and_leaves_target_tree_unmerged() {
    TestDaemon env("conflict");
    Daemon& d = env.daemon;
    auto main = d.main_branch();

    put_blob(d, main->wt, "/same.txt", "base");
    auto base_cp = checkpoint_branch(d, main, "base");
    REQUIRE(base_cp.ok);

    auto feature = create_feature_from_main(d);
    put_blob(d, main->wt, "/same.txt", "target");
    put_blob(d, feature->wt, "/same.txt", "source");

    BranchMergeResult result = d.merge_branch("feature", "main", "");

    REQUIRE(!result.ok);
    REQUIRE(result.commit_hash == ZERO_HASH);
    REQUIRE(result.error == "merge conflicts");
    REQUIRE(result.conflicts.size() == 1);
    REQUIRE(result.conflicts[0] == "/same.txt");
    REQUIRE(blob_text(d, main->wt, "/same.txt") == "target");

    std::printf("  PASS test_conflict_returns_paths_and_leaves_target_tree_unmerged\n");
}

static void test_successful_merge_invalidates_target_file_handles() {
    TestDaemon env("fh");
    Daemon& d = env.daemon;
    auto main = d.main_branch();

    auto base_cp = checkpoint_branch(d, main, "base");
    REQUIRE(base_cp.ok);
    auto feature = create_feature_from_main(d);
    put_blob(d, feature->wt, "/source.txt", "source");

    auto fh_state = std::make_unique<FhState>();
    fh_state->path = "/open.txt";
    fh_state->branch_id = main->branch_id;
    uint64_t fh = d.allocate_fh(std::move(fh_state));
    REQUIRE(d.get_fh(fh) != nullptr);
    REQUIRE(!d.get_fh(fh)->stale);

    BranchMergeResult result = d.merge_branch("feature", "main", "merge feature");

    REQUIRE(result.ok);
    REQUIRE(d.get_fh(fh) != nullptr);
    REQUIRE(d.get_fh(fh)->stale);

    std::printf("  PASS test_successful_merge_invalidates_target_file_handles\n");
}

static void test_merge_auto_checkpoints_dirty_source_and_target_file_handles() {
    TestDaemon env("dirty-fh");
    Daemon& d = env.daemon;
    auto main = d.main_branch();

    auto base_cp = checkpoint_branch(d, main, "base");
    REQUIRE(base_cp.ok);
    auto feature = create_feature_from_main(d);

    uint64_t target_fh = allocate_dirty_fh(d, main, "/target-dirty.txt", "target dirty");
    uint64_t source_fh = allocate_dirty_fh(d, feature, "/source-dirty.txt", "source dirty");

    BranchMergeResult result = d.merge_branch("feature", "main", "merge dirty handles");

    REQUIRE(result.ok);
    REQUIRE(blob_text(d, main->wt, "/target-dirty.txt") == "target dirty");
    REQUIRE(blob_text(d, main->wt, "/source-dirty.txt") == "source dirty");
    REQUIRE(blob_text(d, feature->wt, "/source-dirty.txt") == "source dirty");
    REQUIRE(!feature->wt.lookup("/target-dirty.txt").has_value());
    REQUIRE(d.get_fh(target_fh) != nullptr);
    REQUIRE(d.get_fh(target_fh)->stale);
    REQUIRE(d.get_fh(source_fh) != nullptr);
    REQUIRE(!d.get_fh(source_fh)->stale);

    std::printf("  PASS test_merge_auto_checkpoints_dirty_source_and_target_file_handles\n");
}

static void test_flush_skips_stale_dirty_file_handles() {
    TestDaemon env("stale-skip");
    Daemon& d = env.daemon;
    auto main = d.main_branch();

    put_blob(d, main->wt, "/kept.txt", "kept");
    uint64_t fh = allocate_dirty_fh(d, main, "/stale-dirty.txt", "must not flush", true);

    REQUIRE(d.flush_fhs_for_branch(main->branch_id, main->wt));

    REQUIRE(d.get_fh(fh) != nullptr);
    REQUIRE(d.get_fh(fh)->stale);
    REQUIRE(!main->wt.lookup("/stale-dirty.txt").has_value());
    REQUIRE(blob_text(d, main->wt, "/kept.txt") == "kept");

    std::printf("  PASS test_flush_skips_stale_dirty_file_handles\n");
}

static void test_file_handle_state_outlives_release_when_held() {
    TestDaemon env("fh-hold");
    Daemon& d = env.daemon;
    auto main = d.main_branch();

    auto state = std::make_unique<FhState>();
    state->path = "/held.txt";
    state->branch_id = main->branch_id;
    state->base_size = 123;

    uint64_t fh = d.allocate_fh(std::move(state));
    std::shared_ptr<FhState> held = d.get_fh(fh);
    REQUIRE(held != nullptr);

    d.release_fh(fh);

    REQUIRE(held != nullptr);
    REQUIRE(held->path == "/held.txt");
    REQUIRE(held->branch_id == main->branch_id);
    REQUIRE(held->base_size == 123);
    REQUIRE(d.get_fh(fh) == nullptr);

    std::printf("  PASS test_file_handle_state_outlives_release_when_held\n");
}

static void test_create_branch_rejects_source_branch_without_ref() {
    TestDaemon env("create-source-no-ref");
    Daemon& d = env.daemon;

    auto feature = create_feature_from_main(d);
    REQUIRE(feature != nullptr);
    REQUIRE(d.refs().remove_ref("feature"));

    uint32_t child_id = d.create_branch("child", "feature");

    REQUIRE(child_id == UINT32_MAX);
    REQUIRE(d.branch_by_name("child") == nullptr);
    Hash child_ref = ZERO_HASH;
    REQUIRE(!d.refs().read_ref("child", child_ref));

    std::printf("  PASS test_create_branch_rejects_source_branch_without_ref\n");
}

static void test_checkpoint_rejects_missing_referenced_blob() {
    // Note: as of the pending-set fsync optimization, the checkpoint path no
    // longer scans every referenced leaf. A WT entry pointing to a hash that
    // was never written is a synthetic state that production code cannot
    // produce (write_blob only returns a hash once the object is on disk),
    // so the checkpoint succeeds. Corruption surfaces on the rollback /
    // restart path when the missing blob is actually read.
    TestDaemon env("checkpoint-missing-blob");
    Daemon& d = env.daemon;
    auto main = d.main_branch();

    Hash missing = tagged_hash(0xD4);
    main->wt.insert("/missing.txt", {EntryKind::Blob, missing, 0100644});

    auto result = checkpoint_branch(d, main, "missing blob");
    REQUIRE(result.ok);

    // But a fresh daemon trying to rebuild from this commit must fail,
    // because rebuild_working_tree reads tree objects and any code that
    // reads the missing blob hits read_object failure.
    Hash recorded = ZERO_HASH;
    REQUIRE(d.refs().read_ref("main", recorded));
    std::vector<uint8_t> commit_body;
    REQUIRE(d.store().read_commit(recorded, commit_body));

    std::printf("  PASS test_checkpoint_rejects_missing_referenced_blob\n");
}

static void test_checkpoint_skips_zero_hash_blob() {
    TestDaemon env("checkpoint-zero-blob");
    Daemon& d = env.daemon;
    auto main = d.main_branch();

    main->wt.insert("/zero.txt", {EntryKind::Blob, ZERO_HASH, 0100644});

    Hash before = ZERO_HASH;
    REQUIRE(d.refs().read_ref("main", before));

    auto result = checkpoint_branch(d, main, "zero blob");

    REQUIRE(result.ok);
    Hash after = ZERO_HASH;
    REQUIRE(d.refs().read_ref("main", after));
    REQUIRE(after != before);

    std::printf("  PASS test_checkpoint_skips_zero_hash_blob\n");
}

static void test_unknown_branch_and_same_branch_errors() {
    TestDaemon env("errors");
    Daemon& d = env.daemon;

    BranchMergeResult missing_source = d.merge_branch("missing", "main", "");
    REQUIRE(!missing_source.ok);
    REQUIRE(missing_source.error == "unknown branch");

    BranchMergeResult missing_target = d.merge_branch("main", "missing", "");
    REQUIRE(!missing_target.ok);
    REQUIRE(missing_target.error == "unknown branch");

    BranchMergeResult same = d.merge_branch("main", "main", "");
    REQUIRE(!same.ok);
    REQUIRE(same.error == "cannot merge branch into itself");

    std::printf("  PASS test_unknown_branch_and_same_branch_errors\n");
}

static void test_default_merge_label_is_used_when_label_is_empty() {
    TestDaemon env("label");
    Daemon& d = env.daemon;
    auto main = d.main_branch();

    put_blob(d, main->wt, "/base.txt", "base");
    auto base_cp = checkpoint_branch(d, main, "base");
    REQUIRE(base_cp.ok);
    auto feature = create_feature_from_main(d);
    put_blob(d, feature->wt, "/source.txt", "source");

    auto r = d.merge_branch("feature", "main", "");
    REQUIRE(r.ok);
    CommitData merge = read_commit(d, r.commit_hash);
    REQUIRE(merge.label == "merge feature into main");

    std::printf("  PASS test_default_merge_label_is_used_when_label_is_empty\n");
}

static void test_control_socket_branch_merge_omits_label_and_merges() {
    TestDaemon env("control-merge");
    Daemon& d = env.daemon;
    auto main = d.main_branch();

    put_blob(d, main->wt, "/base.txt", "base");
    auto base_cp = checkpoint_branch(d, main, "base");
    REQUIRE(base_cp.ok);
    auto feature = create_feature_from_main(d);
    put_blob(d, feature->wt, "/source.txt", "source");

    std::string socket_path = env.root + "/control.sock";
    ControlSocket control(d);
    auto handler = [&](std::string_view line, const PeerCredentials&) {
        return control_protocol::dispatch(d, line);
    };
    REQUIRE(control.start(socket_path, handler));

    std::string response = control_request(
        socket_path, "branch.merge {\"source\":\"feature\",\"target\":\"main\"}");
    REQUIRE(response.find("\"ok\":true") != std::string::npos);
    require_hex_commit_field(response);
    REQUIRE(response.find("\"target\":\"main\"") != std::string::npos);
    REQUIRE(response.find("\"source\":\"feature\"") != std::string::npos);

    REQUIRE(blob_text(d, main->wt, "/source.txt") == "source");
    Hash main_ref = ZERO_HASH;
    REQUIRE(d.refs().read_ref("main", main_ref));
    CommitData merge = read_commit(d, main_ref);
    REQUIRE(merge.label == "merge feature into main");

    std::printf("  PASS test_control_socket_branch_merge_omits_label_and_merges\n");
}

static void test_control_socket_branch_merge_requires_actual_source_key() {
    TestDaemon env("control-missing-source");
    Daemon& d = env.daemon;
    auto main = d.main_branch();

    put_blob(d, main->wt, "/base.txt", "base");
    auto base_cp = checkpoint_branch(d, main, "base");
    REQUIRE(base_cp.ok);
    Hash main_ref_before = ZERO_HASH;
    REQUIRE(d.refs().read_ref("main", main_ref_before));
    auto feature = create_feature_from_main(d);
    put_blob(d, feature->wt, "/source.txt", "source");

    std::string socket_path = env.root + "/control.sock";
    ControlSocket control(d);
    auto handler = [&](std::string_view line, const PeerCredentials&) {
        return control_protocol::dispatch(d, line);
    };
    REQUIRE(control.start(socket_path, handler));

    std::string response = control_request(
        socket_path,
        "branch.merge {\"note\":\"source\",\"alias\":\"feature\",\"target\":\"main\"}");
    REQUIRE(response == "{\"ok\":false,\"error\":\"missing source\"}");
    REQUIRE(!main->wt.lookup("/source.txt").has_value());

    Hash main_ref_after = ZERO_HASH;
    REQUIRE(d.refs().read_ref("main", main_ref_after));
    REQUIRE(main_ref_after == main_ref_before);

    std::printf("  PASS test_control_socket_branch_merge_requires_actual_source_key\n");
}

static void test_control_socket_branch_merge_preserves_escaped_label() {
    TestDaemon env("control-label");
    Daemon& d = env.daemon;
    auto main = d.main_branch();

    put_blob(d, main->wt, "/base.txt", "base");
    auto base_cp = checkpoint_branch(d, main, "base");
    REQUIRE(base_cp.ok);
    auto feature = create_feature_from_main(d);
    put_blob(d, feature->wt, "/source.txt", "source");

    std::string socket_path = env.root + "/control.sock";
    ControlSocket control(d);
    auto handler = [&](std::string_view line, const PeerCredentials&) {
        return control_protocol::dispatch(d, line);
    };
    REQUIRE(control.start(socket_path, handler));

    std::string response = control_request(
        socket_path,
        "branch.merge {\"source\":\"feature\",\"target\":\"main\","
        "\"label\":\"release \\\"quoted\\\"\"}");
    REQUIRE(response.find("\"ok\":true") != std::string::npos);
    require_hex_commit_field(response);

    Hash main_ref = ZERO_HASH;
    REQUIRE(d.refs().read_ref("main", main_ref));
    CommitData merge = read_commit(d, main_ref);
    REQUIRE(merge.label == "release \"quoted\"");

    std::printf("  PASS test_control_socket_branch_merge_preserves_escaped_label\n");
}

static void test_control_socket_branch_merge_rejects_malformed_request() {
    TestDaemon env("control-malformed");
    Daemon& d = env.daemon;
    auto main = d.main_branch();

    put_blob(d, main->wt, "/base.txt", "base");
    auto base_cp = checkpoint_branch(d, main, "base");
    REQUIRE(base_cp.ok);
    Hash main_ref_before = ZERO_HASH;
    REQUIRE(d.refs().read_ref("main", main_ref_before));
    auto feature = create_feature_from_main(d);
    put_blob(d, feature->wt, "/source.txt", "source");

    std::string socket_path = env.root + "/control.sock";
    ControlSocket control(d);
    auto handler = [&](std::string_view line, const PeerCredentials&) {
        return control_protocol::dispatch(d, line);
    };
    REQUIRE(control.start(socket_path, handler));

    std::string response = control_request(
        socket_path,
        "branch.merge {\"source\":\"feature\",\"target\":\"main\",");
    REQUIRE(response == "{\"ok\":false,\"error\":\"malformed request\"}");
    REQUIRE(!main->wt.lookup("/source.txt").has_value());

    response = control_request(
        socket_path,
        "branch.merge {\"source\":\"feature\",\"target\":\"main\",\"bad\":not-json}");
    REQUIRE(response == "{\"ok\":false,\"error\":\"malformed request\"}");
    REQUIRE(!main->wt.lookup("/source.txt").has_value());

    Hash main_ref_after = ZERO_HASH;
    REQUIRE(d.refs().read_ref("main", main_ref_after));
    REQUIRE(main_ref_after == main_ref_before);

    std::printf("  PASS test_control_socket_branch_merge_rejects_malformed_request\n");
}

static void test_control_socket_branch_merge_returns_conflict_array() {
    TestDaemon env("control-conflict");
    Daemon& d = env.daemon;
    auto main = d.main_branch();

    put_blob(d, main->wt, "/same\"quoted.txt", "base");
    auto base_cp = checkpoint_branch(d, main, "base");
    REQUIRE(base_cp.ok);
    auto feature = create_feature_from_main(d);
    put_blob(d, main->wt, "/same\"quoted.txt", "target");
    put_blob(d, feature->wt, "/same\"quoted.txt", "source");

    std::string socket_path = env.root + "/control.sock";
    ControlSocket control(d);
    auto handler = [&](std::string_view line, const PeerCredentials&) {
        return control_protocol::dispatch(d, line);
    };
    REQUIRE(control.start(socket_path, handler));

    std::string response = control_request(
        socket_path, "branch.merge {\"source\":\"feature\",\"target\":\"main\"}");
    REQUIRE(response == "{\"ok\":false,\"error\":\"merge conflicts\","
                        "\"conflicts\":[\"/same\\\"quoted.txt\"]}");

    std::printf("  PASS test_control_socket_branch_merge_returns_conflict_array\n");
}

int main() {
    std::printf("test_branch_merge_daemon:\n");
    test_clean_merge_advances_target_only_and_writes_two_parent_commit();
    test_create_branch_rejects_source_branch_without_ref();
    test_checkpoint_rejects_missing_referenced_blob();
    test_checkpoint_skips_zero_hash_blob();
    test_merge_deleted_source_root_file_is_not_reingested_by_lazy_bootstrap();
    test_merge_deleted_source_root_file_survives_restart_and_background_bootstrap();
    test_conflict_returns_paths_and_leaves_target_tree_unmerged();
    test_successful_merge_invalidates_target_file_handles();
    test_merge_auto_checkpoints_dirty_source_and_target_file_handles();
    test_flush_skips_stale_dirty_file_handles();
    test_file_handle_state_outlives_release_when_held();
    test_unknown_branch_and_same_branch_errors();
    test_default_merge_label_is_used_when_label_is_empty();
    test_control_socket_branch_merge_omits_label_and_merges();
    test_control_socket_branch_merge_requires_actual_source_key();
    test_control_socket_branch_merge_preserves_escaped_label();
    test_control_socket_branch_merge_rejects_malformed_request();
    test_control_socket_branch_merge_returns_conflict_array();
    std::printf("PASS test_branch_merge_daemon\n");
    return 0;
}
