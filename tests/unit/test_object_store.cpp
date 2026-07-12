#include "hash.h"
#include "object_store.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
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

// ---------- BlobView tests ----------

static void test_open_blob_exposes_payload_without_copy() {
    std::string root = make_tmp_dir("blob-view");
    ObjectStore store(root);
    REQUIRE(store.init_layout());
    std::vector<uint8_t> payload = {1, 2, 3, 4, 5};
    Hash hash = store.write_blob(payload);
    BlobView view;
    REQUIRE(store.open_blob(hash, view) == 0);
    REQUIRE(view);
    REQUIRE(view.payload_size() == payload.size());
    REQUIRE(BlobView::kPayloadOffset == 12);
    std::vector<uint8_t> got(payload.size());
    REQUIRE(pread(view.fd(), got.data(), got.size(), BlobView::kPayloadOffset) ==
            static_cast<ssize_t>(got.size()));
    REQUIRE(got == payload);
    remove_dir_recursive(root);
    std::printf("  PASS test_open_blob_exposes_payload_without_copy\n");
}

static void test_open_blob_move_owns_fd_once() {
    std::string root = make_tmp_dir("blob-view-move");
    ObjectStore store(root);
    REQUIRE(store.init_layout());
    Hash hash = store.write_blob(nullptr, 0);
    BlobView first;
    REQUIRE(store.open_blob(hash, first) == 0);
    int fd = first.fd();
    BlobView second = std::move(first);
    REQUIRE(!first);
    REQUIRE(second.fd() == fd);
    second = BlobView{};
    errno = 0;
    REQUIRE(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
    remove_dir_recursive(root);
    std::printf("  PASS test_open_blob_move_owns_fd_once\n");
}

static void test_open_blob_rejects_corruption() {
    std::string root = make_tmp_dir("blob-view-corrupt");
    ObjectStore store(root);
    REQUIRE(store.init_layout());
    Hash hash = store.write_blob(reinterpret_cast<const uint8_t*>("abc"), 3);
    std::string path = store.object_path(hash);
    REQUIRE(chmod(path.c_str(), 0644) == 0);
    int fd = open(path.c_str(), O_WRONLY | O_TRUNC);
    REQUIRE(fd >= 0);
    REQUIRE(write(fd, "blob", 4) == 4);
    REQUIRE(close(fd) == 0);
    BlobView view;
    REQUIRE(store.open_blob(hash, view) == EIO);
    REQUIRE(!view);
    remove_dir_recursive(root);
    std::printf("  PASS test_open_blob_rejects_corruption\n");
}

static void test_open_blob_missing_is_eio() {
    std::string root = make_tmp_dir("blob-view-missing");
    ObjectStore store(root);
    REQUIRE(store.init_layout());
    BlobView view;
    REQUIRE(store.open_blob(tagged_hash(0xCC), view) == EIO);
    remove_dir_recursive(root);
    std::printf("  PASS test_open_blob_missing_is_eio\n");
}

static void test_open_blob_rejects_tree_tag_swap() {
    std::string root = make_tmp_dir("blob-view-tree-swap");
    ObjectStore store(root);
    REQUIRE(store.init_layout());
    std::vector<uint8_t> payload = {7, 8, 9};
    Hash hash = store.write_blob(payload);
    std::string path = store.object_path(hash);
    // Rewrite only the first 4 bytes from "blob" to "tree", preserving total
    // file size (12-byte header + payload untouched).
    REQUIRE(chmod(path.c_str(), 0644) == 0);
    int fd = open(path.c_str(), O_WRONLY);
    REQUIRE(fd >= 0);
    REQUIRE(write(fd, "tree", 4) == 4);
    REQUIRE(close(fd) == 0);
    BlobView view;
    REQUIRE(store.open_blob(hash, view) == EIO);
    REQUIRE(!view);
    remove_dir_recursive(root);
    std::printf("  PASS test_open_blob_rejects_tree_tag_swap\n");
}

static void test_open_blob_rejects_length_one_too_big() {
    std::string root = make_tmp_dir("blob-view-len-plus1");
    ObjectStore store(root);
    REQUIRE(store.init_layout());
    std::vector<uint8_t> payload = {10, 20, 30, 40};
    Hash hash = store.write_blob(payload);
    std::string path = store.object_path(hash);
    // Rewrite the 8-byte little-endian size field (offset 4) to
    // payload_size + 1, keeping the rest of the file intact.
    uint64_t encoded = payload.size() + 1;
    REQUIRE(chmod(path.c_str(), 0644) == 0);
    int fd = open(path.c_str(), O_WRONLY);
    REQUIRE(fd >= 0);
    REQUIRE(pwrite(fd, &encoded, sizeof(encoded), 4) ==
            static_cast<ssize_t>(sizeof(encoded)));
    REQUIRE(close(fd) == 0);
    BlobView view;
    REQUIRE(store.open_blob(hash, view) == EIO);
    REQUIRE(!view);
    remove_dir_recursive(root);
    std::printf("  PASS test_open_blob_rejects_length_one_too_big\n");
}

static void test_open_blob_overflow_returns_eoverflow() {
    std::string root = make_tmp_dir("blob-view-overflow");
    ObjectStore store(root);
    REQUIRE(store.init_layout());
    std::vector<uint8_t> payload = {11, 22};
    Hash hash = store.write_blob(payload);
    std::string path = store.object_path(hash);
    // Rewrite the 8-byte size field to UINT64_MAX so the overflow check
    // fires before the physical-size comparison.
    uint64_t encoded = std::numeric_limits<uint64_t>::max();
    REQUIRE(chmod(path.c_str(), 0644) == 0);
    int fd = open(path.c_str(), O_WRONLY);
    REQUIRE(fd >= 0);
    REQUIRE(pwrite(fd, &encoded, sizeof(encoded), 4) ==
            static_cast<ssize_t>(sizeof(encoded)));
    REQUIRE(close(fd) == 0);
    BlobView view;
    REQUIRE(store.open_blob(hash, view) == EOVERFLOW);
    REQUIRE(!view);
    remove_dir_recursive(root);
    std::printf("  PASS test_open_blob_overflow_returns_eoverflow\n");
}

static void test_open_blob_zero_length_payload() {
    std::string root = make_tmp_dir("blob-view-zero");
    ObjectStore store(root);
    REQUIRE(store.init_layout());
    Hash hash = store.write_blob(nullptr, 0);
    BlobView view;
    REQUIRE(store.open_blob(hash, view) == 0);
    REQUIRE(view);
    REQUIRE(view.payload_size() == 0);
    remove_dir_recursive(root);
    std::printf("  PASS test_open_blob_zero_length_payload\n");
}

static void test_blob_payload_size_caches_immutable_sizes() {
    std::string root = make_tmp_dir("size-cache");
    ObjectStore store(root);
    REQUIRE(store.init_layout());
    std::vector<uint8_t> payload = {9, 8, 7};
    Hash hash = store.write_blob(payload);

    uint64_t size = 0;
    REQUIRE(store.blob_payload_size(hash, size) == 0);
    REQUIRE(size == payload.size());

    // Blobs are content-addressed and immutable, and published objects are
    // never deleted, so the size is served from cache: removing the object
    // file must not affect a second query (proves no per-call disk access).
    REQUIRE(std::remove(store.object_path(hash).c_str()) == 0);
    size = 0;
    REQUIRE(store.blob_payload_size(hash, size) == 0);
    REQUIRE(size == payload.size());

    remove_dir_recursive(root);
    std::printf("  PASS test_blob_payload_size_caches_immutable_sizes\n");
}

static void test_blob_payload_size_does_not_cache_errors() {
    std::string root = make_tmp_dir("size-cache-errors");
    ObjectStore store(root);
    REQUIRE(store.init_layout());

    // Missing object: error surfaces, and a later write must succeed —
    // the miss result was not cached.
    std::vector<uint8_t> payload = {1, 2, 3, 4};
    Hash hash;
    {
        ObjectStore scratch(make_tmp_dir("size-cache-scratch"));
        REQUIRE(scratch.init_layout());
        hash = scratch.write_blob(payload);
    }
    uint64_t size = 0;
    REQUIRE(store.blob_payload_size(hash, size) == EIO);
    REQUIRE(store.write_blob(payload) == hash);
    REQUIRE(store.blob_payload_size(hash, size) == 0);
    REQUIRE(size == payload.size());

    // Corrupt object: EIO on every query while corrupt (never cached),
    // correct size after the object is restored.
    std::vector<uint8_t> other = {5, 6, 7, 8, 9};
    Hash corrupt_hash = store.write_blob(other);
    std::string path = store.object_path(corrupt_hash);
    REQUIRE(chmod(path.c_str(), 0644) == 0);
    REQUIRE(truncate(path.c_str(), 4) == 0);
    REQUIRE(store.blob_payload_size(corrupt_hash, size) == EIO);
    REQUIRE(store.blob_payload_size(corrupt_hash, size) == EIO);
    REQUIRE(std::remove(path.c_str()) == 0);
    REQUIRE(store.write_blob(other) == corrupt_hash);
    REQUIRE(store.blob_payload_size(corrupt_hash, size) == 0);
    REQUIRE(size == other.size());

    remove_dir_recursive(root);
    std::printf("  PASS test_blob_payload_size_does_not_cache_errors\n");
}

int main() {
    std::printf("test_object_store:\n");
    test_fsync_objects_reports_fdatasync_failure();
    test_fsync_shard_dirs_reports_objects_dir_failure();
    test_pending_tracks_new_writes_and_skips_dedup_hits();
    test_pending_restore_round_trips();
    test_fsync_pending_erases_only_requested_hashes();
    test_init_layout_rejects_unwritable_object_shard();
    test_open_blob_exposes_payload_without_copy();
    test_open_blob_move_owns_fd_once();
    test_open_blob_rejects_corruption();
    test_open_blob_missing_is_eio();
    test_open_blob_rejects_tree_tag_swap();
    test_open_blob_rejects_length_one_too_big();
    test_open_blob_overflow_returns_eoverflow();
    test_open_blob_zero_length_payload();
    test_blob_payload_size_caches_immutable_sizes();
    test_blob_payload_size_does_not_cache_errors();
    std::printf("PASS test_object_store\n");
    return 0;
}
