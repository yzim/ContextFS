#include "refs.h"
#include "hash.h"
#include "daemon.h"
#include "commit.h"
#include "tree_serialize.h"
#include "branch_name.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <algorithm>
#include <fcntl.h>
#include <dirent.h>

using namespace cas;

// assert() is compiled out under -DNDEBUG; use REQUIRE for calls with side effects.
#define REQUIRE(expr) do { if (!(expr)) { \
    std::fprintf(stderr, "REQUIRE failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
    std::abort(); } } while (0)

static std::string make_tmp_dir(const char* suffix) {
    std::string templ = std::string("/tmp/agentvfs-branch-persistence-") + suffix + "-XXXXXX";
    std::vector<char> buf(templ.begin(), templ.end());
    buf.push_back('\0');
    char* path = mkdtemp(buf.data());
    REQUIRE(path != nullptr);
    return std::string(path);
}

static void write_file(const std::string& path, const std::string& body) {
    FILE* f = std::fopen(path.c_str(), "wb");
    REQUIRE(f != nullptr);
    REQUIRE(std::fwrite(body.data(), 1, body.size(), f) == body.size());
    REQUIRE(std::fclose(f) == 0);
}

static Hash tagged_hash(uint8_t tag) {
    Hash h{};
    h[0] = tag;
    return h;
}

static void remove_dir_recursive(const std::string& path);

static Hash empty_tree_hash_for_test() {
    std::string root = make_tmp_dir("empty-tree-hash");
    ObjectStore store(root);
    REQUIRE(store.init_layout());
    WorkingTree wt;
    std::vector<Hash> written;
    Hash h = serialize_working_tree(wt, store, written);
    REQUIRE(h != ZERO_HASH);
    remove_dir_recursive(root);
    return h;
}

static bool contains(const std::vector<std::string>& values, const std::string& needle) {
    return std::find(values.begin(), values.end(), needle) != values.end();
}

static void remove_dir_recursive(const std::string& path) {
    DIR* dir = opendir(path.c_str());
    if (!dir) return;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.' && (ent->d_name[1] == '\0'
            || (ent->d_name[1] == '.' && ent->d_name[2] == '\0'))) continue;
        std::string child = path + "/" + ent->d_name;
        struct stat st;
        if (lstat(child.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            remove_dir_recursive(child);
        } else {
            std::remove(child.c_str());
        }
    }
    closedir(dir);
    rmdir(path.c_str());
}

static Hash write_commit_for_tree(ObjectStore& store, WorkingTree& wt, const std::string& label) {
    std::vector<Hash> written;
    Hash tree = serialize_working_tree(wt, store, written);
    assert(tree != ZERO_HASH);

    CommitData cd;
    cd.tree_hash = tree;
    cd.session_id = 123;    // arbitrary — not meaningful for these tests
    cd.timestamp_ns = 456;  // arbitrary — not meaningful for these tests
    cd.label = label;
    cd.policy_version = 1;

    Hash commit = store.write_commit(serialize_commit(cd));
    assert(commit != ZERO_HASH);
    written.push_back(commit);
    store.fsync_objects(written);
    store.fsync_shard_dirs(written);
    return commit;
}

static void test_daemon_restores_checkpointed_branch() {
    std::string root = make_tmp_dir("restore");
    std::string source = root + "/src";
    std::string mount = root + "/mnt";
    std::string store_root = root + "/store";
    REQUIRE(mkdir(source.c_str(), 0755) == 0);
    REQUIRE(mkdir(mount.c_str(), 0755) == 0);

    ObjectStore store(store_root);
    REQUIRE(store.init_layout());
    Refs refs(store_root);

    WorkingTree main_wt;
    Hash main_commit = write_commit_for_tree(store, main_wt, "main");
    REQUIRE(refs.write_ref("main", main_commit, store.tmp_dir()));

    WorkingTree branch_wt;
    Hash blob = store.write_blob(reinterpret_cast<const uint8_t*>("persisted"), 9);
    assert(blob != ZERO_HASH);
    branch_wt.insert("/a.txt", {EntryKind::Blob, blob, 0100644});
    Hash branch_commit = write_commit_for_tree(store, branch_wt, "branch");
    REQUIRE(refs.write_ref("subagent-1", branch_commit, store.tmp_dir()));

    Daemon daemon(source, mount, store_root);
    REQUIRE(daemon.initialize());

    auto branch = daemon.branch_by_name("subagent-1");
    assert(branch != nullptr);
    assert(branch->branch_id == 1);
    auto entry = branch->wt.lookup("/a.txt");
    assert(entry.has_value());
    assert(entry->kind == EntryKind::Blob);
    assert(entry->hash == blob);

    remove_dir_recursive(root);

    std::printf("  PASS test_daemon_restores_checkpointed_branch\n");
}

static void test_daemon_skips_corrupt_non_main_ref() {
    std::string root = make_tmp_dir("corrupt");
    std::string source = root + "/src";
    std::string mount = root + "/mnt";
    std::string store_root = root + "/store";
    REQUIRE(mkdir(source.c_str(), 0755) == 0);
    REQUIRE(mkdir(mount.c_str(), 0755) == 0);

    ObjectStore store(store_root);
    REQUIRE(store.init_layout());
    Refs refs(store_root);

    WorkingTree main_wt;
    Hash main_commit = write_commit_for_tree(store, main_wt, "main");
    REQUIRE(refs.write_ref("main", main_commit, store.tmp_dir()));
    write_file(store_root + "/refs/bad", "not-a-hash\n");

    Daemon daemon(source, mount, store_root);
    REQUIRE(daemon.initialize());
    assert(daemon.branch_by_name("bad") == nullptr);
    assert(daemon.branch_by_name("main") != nullptr);

    remove_dir_recursive(root);

    std::printf("  PASS test_daemon_skips_corrupt_non_main_ref\n");
}

static void test_daemon_init_does_not_write_main_ref_when_empty_tree_write_fails() {
    std::string root = make_tmp_dir("init-tree-fail");
    std::string source = root + "/src";
    std::string mount = root + "/mnt";
    std::string store_root = root + "/store";
    REQUIRE(mkdir(source.c_str(), 0755) == 0);
    REQUIRE(mkdir(mount.c_str(), 0755) == 0);

    Hash empty_tree = empty_tree_hash_for_test();

    ObjectStore store(store_root);
    REQUIRE(store.init_layout());
    std::string blocked_shard = store_root + "/objects/" + hash_to_hex(empty_tree).substr(0, 2);
    write_file(blocked_shard, "not a directory");

    Daemon daemon(source, mount, store_root);
    REQUIRE(!daemon.initialize());

    Refs refs(store_root);
    Hash main_ref = ZERO_HASH;
    REQUIRE(!refs.read_ref("main", main_ref));

    remove_dir_recursive(root);

    std::printf("  PASS test_daemon_init_does_not_write_main_ref_when_empty_tree_write_fails\n");
}

static void test_write_ref_reports_refs_dir_fsync_failure() {
    std::string root = make_tmp_dir("refs-dir-fsync");
    ObjectStore store(root);
    REQUIRE(store.init_layout());
    Refs refs(root);

    REQUIRE(chmod((root + "/refs").c_str(), 0333) == 0);

    REQUIRE(!refs.write_ref("main", tagged_hash(0xC9), store.tmp_dir()));

    REQUIRE(chmod((root + "/refs").c_str(), 0755) == 0);
    Hash ref = ZERO_HASH;
    REQUIRE(!refs.read_ref("main", ref));
    remove_dir_recursive(root);

    std::printf("  PASS test_write_ref_reports_refs_dir_fsync_failure\n");
}

static void test_list_refs_regular_files_only() {
    std::string root = make_tmp_dir("refs");
    REQUIRE(mkdir((root + "/refs").c_str(), 0755) == 0);
    REQUIRE(mkdir((root + "/refs/not-a-ref-dir").c_str(), 0755) == 0);
    write_file(root + "/refs/main", std::string(64, '0') + "\n");
    write_file(root + "/refs/subagent-1", std::string(64, '1') + "\n");

    Refs refs(root);
    std::vector<std::string> names = refs.list_refs();

    assert(contains(names, "main"));
    assert(contains(names, "subagent-1"));
    assert(!contains(names, "."));
    assert(!contains(names, ".."));
    assert(!contains(names, "not-a-ref-dir"));

    remove_dir_recursive(root);

    std::printf("  PASS test_list_refs_regular_files_only\n");
}

static void test_branch_name_validation() {
    assert(!is_valid_branch_name(""));
    assert(!is_valid_branch_name(std::string(65, 'a')));
    assert(is_valid_branch_name(std::string(64, 'a')));
    assert(is_valid_branch_name("a"));

    assert(is_valid_branch_name("subagent-1"));
    assert(is_valid_branch_name("my_branch"));
    assert(is_valid_branch_name("ABC123"));

    assert(!is_valid_branch_name("has space"));
    assert(!is_valid_branch_name("has/slash"));
    assert(!is_valid_branch_name("has.dot"));
    assert(!is_valid_branch_name("has@at"));

    assert(!is_valid_branch_name("main"));
    assert(!is_valid_branch_name("HEAD"));
    assert(!is_valid_branch_name("FETCH_HEAD"));
    assert(!is_valid_branch_name("ORIG_HEAD"));
    assert(!is_valid_branch_name("MERGE_HEAD"));
    assert(!is_valid_branch_name("refs"));
    assert(!is_valid_branch_name("objects"));
    assert(!is_valid_branch_name("tmp"));
    assert(!is_valid_branch_name("telemetry"));

    assert(is_valid_branch_name("main2"));
    assert(is_valid_branch_name("my-refs"));

    std::printf("  PASS test_branch_name_validation\n");
}

int main() {
    std::printf("test_branch_persistence:\n");
    test_branch_name_validation();
    test_list_refs_regular_files_only();
    test_daemon_restores_checkpointed_branch();
    test_daemon_skips_corrupt_non_main_ref();
    test_daemon_init_does_not_write_main_ref_when_empty_tree_write_fails();
    test_write_ref_reports_refs_dir_fsync_failure();
    std::printf("All branch persistence tests passed.\n");
    return 0;
}
