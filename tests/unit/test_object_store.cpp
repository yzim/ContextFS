#include "hash.h"
#include "object_store.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace cas;

#define REQUIRE(expr) do { if (!(expr)) { \
    std::fprintf(stderr, "REQUIRE failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
    std::abort(); } } while (0)

static std::string make_tmp_dir(const char* suffix) {
    std::string templ = std::string("/tmp/agentvfs-object-store-") + suffix + "-XXXXXX";
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

static Hash tagged_hash(uint8_t tag) {
    Hash h{};
    h[0] = tag;
    return h;
}

static void test_fsync_objects_reports_fdatasync_failure() {
    std::string root = make_tmp_dir("fdatasync");
    ObjectStore store(root);
    REQUIRE(store.init_layout());

    Hash h = tagged_hash(0xA7);
    std::string shard = root + "/objects/" + hash_to_hex(h).substr(0, 2);
    REQUIRE(mkdir(shard.c_str(), 0755) == 0);
    REQUIRE(symlink("/dev/null", store.object_path(h).c_str()) == 0);

    REQUIRE(!store.fsync_objects({h}));

    remove_dir_recursive(root);
    std::printf("  PASS test_fsync_objects_reports_fdatasync_failure\n");
}

static void test_fsync_shard_dirs_reports_objects_dir_failure() {
    std::string root = make_tmp_dir("objects-dir-fsync");
    ObjectStore store(root);
    REQUIRE(store.init_layout());

    Hash h = tagged_hash(0xB8);
    std::string shard = root + "/objects/" + hash_to_hex(h).substr(0, 2);
    REQUIRE(mkdir(shard.c_str(), 0755) == 0);
    REQUIRE(chmod((root + "/objects").c_str(), 0111) == 0);

    REQUIRE(!store.fsync_shard_dirs({h}));

    REQUIRE(chmod((root + "/objects").c_str(), 0755) == 0);
    remove_dir_recursive(root);
    std::printf("  PASS test_fsync_shard_dirs_reports_objects_dir_failure\n");
}

static void test_pending_tracks_new_writes_and_skips_dedup_hits() {
    std::string root = make_tmp_dir("pending-tracks");
    ObjectStore store(root);
    REQUIRE(store.init_layout());

    REQUIRE(store.pending_count() == 0);

    uint8_t data1[] = {1, 2, 3, 4};
    Hash h1 = store.write_blob(data1, sizeof(data1));
    REQUIRE(h1 != ZERO_HASH);
    REQUIRE(store.pending_count() == 1);

    uint8_t data2[] = {5, 6, 7, 8, 9};
    Hash h2 = store.write_blob(data2, sizeof(data2));
    REQUIRE(h2 != ZERO_HASH);
    REQUIRE(store.pending_count() == 2);

    // Same content -> dedup hit, must NOT re-add to pending.
    Hash h1_again = store.write_blob(data1, sizeof(data1));
    REQUIRE(h1_again == h1);
    REQUIRE(store.pending_count() == 2);

    auto drained = store.drain_pending();
    REQUIRE(drained.size() == 2);
    REQUIRE(store.pending_count() == 0);

    // After drain, the same dedup hit STILL must not re-add: the object is
    // already on disk and (presumably) fsync'd by the caller of drain.
    Hash h1_post = store.write_blob(data1, sizeof(data1));
    REQUIRE(h1_post == h1);
    REQUIRE(store.pending_count() == 0);

    remove_dir_recursive(root);
    std::printf("  PASS test_pending_tracks_new_writes_and_skips_dedup_hits\n");
}

static void test_pending_restore_round_trips() {
    std::string root = make_tmp_dir("pending-restore");
    ObjectStore store(root);
    REQUIRE(store.init_layout());

    uint8_t data[] = {10, 11, 12};
    Hash h = store.write_blob(data, sizeof(data));
    REQUIRE(h != ZERO_HASH);

    auto drained = store.drain_pending();
    REQUIRE(drained.size() == 1);
    REQUIRE(store.pending_count() == 0);

    // Simulate fsync failure: caller puts hashes back so the next checkpoint
    // retries them.
    store.restore_pending(drained);
    REQUIRE(store.pending_count() == 1);

    auto drained2 = store.drain_pending();
    REQUIRE(drained2.size() == 1);
    REQUIRE(drained2[0] == h);

    remove_dir_recursive(root);
    std::printf("  PASS test_pending_restore_round_trips\n");
}

static void test_fsync_pending_erases_only_requested_hashes() {
    std::string root = make_tmp_dir("pending-subset");
    ObjectStore store(root);
    REQUIRE(store.init_layout());

    uint8_t data1[] = {21, 22, 23};
    uint8_t data2[] = {31, 32, 33};
    Hash h1 = store.write_blob(data1, sizeof(data1));
    Hash h2 = store.write_blob(data2, sizeof(data2));
    REQUIRE(h1 != ZERO_HASH);
    REQUIRE(h2 != ZERO_HASH);
    REQUIRE(store.pending_count() == 2);

    std::string error;
    REQUIRE(store.fsync_pending({h1}, error));
    REQUIRE(error.empty());
    REQUIRE(store.pending_count() == 1);

    auto drained = store.drain_pending();
    REQUIRE(drained.size() == 1);
    REQUIRE(drained[0] == h2);

    remove_dir_recursive(root);
    std::printf("  PASS test_fsync_pending_erases_only_requested_hashes\n");
}

static void test_init_layout_rejects_unwritable_object_shard() {
    std::string root = make_tmp_dir("unwritable-shard");
    ObjectStore store(root);
    REQUIRE(store.init_layout());

    std::string shard = root + "/objects/aa";
    REQUIRE(mkdir(shard.c_str(), 0755) == 0);
    REQUIRE(chmod(shard.c_str(), 0555) == 0);

    ObjectStore reopened(root);
    REQUIRE(!reopened.init_layout());

    REQUIRE(chmod(shard.c_str(), 0755) == 0);
    remove_dir_recursive(root);
    std::printf("  PASS test_init_layout_rejects_unwritable_object_shard\n");
}

int main() {
    std::printf("test_object_store:\n");
    test_fsync_objects_reports_fdatasync_failure();
    test_fsync_shard_dirs_reports_objects_dir_failure();
    test_pending_tracks_new_writes_and_skips_dedup_hits();
    test_pending_restore_round_trips();
    test_fsync_pending_erases_only_requested_hashes();
    test_init_layout_rejects_unwritable_object_shard();
    std::printf("PASS test_object_store\n");
    return 0;
}
