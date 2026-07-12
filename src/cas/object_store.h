#pragma once
#include "hash.h"
#include <cstdint>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace cas {

class BlobView {
public:
    static constexpr uint64_t kPayloadOffset = 12;

    BlobView() = default;
    ~BlobView();
    BlobView(BlobView&& other) noexcept;
    BlobView& operator=(BlobView&& other) noexcept;
    BlobView(const BlobView&) = delete;
    BlobView& operator=(const BlobView&) = delete;

    explicit operator bool() const noexcept { return fd_ >= 0; }
    int fd() const noexcept { return fd_; }
    uint64_t payload_size() const noexcept { return payload_size_; }

private:
    friend class ObjectStore;
    BlobView(int fd, uint64_t payload_size) noexcept
        : fd_(fd), payload_size_(payload_size) {}
    void reset() noexcept;

    int fd_ = -1;
    uint64_t payload_size_ = 0;
};

class ObjectStore {
public:
    explicit ObjectStore(const std::string& store_root);

    bool init_layout();
    void cleanup_tmp();

    Hash write_blob(const uint8_t* data, size_t len);
    Hash write_blob(const std::vector<uint8_t>& data);
    bool read_blob(const Hash& hash, std::vector<uint8_t>& out);
    int open_blob(const Hash& hash, BlobView& out) const;

    // Header-validated payload size for a blob, served from a bounded
    // in-memory cache after the first query. Blobs are content-addressed
    // and immutable, and published objects are never deleted, so cached
    // sizes never invalidate. Errors (missing/corrupt objects) are
    // re-probed on every call, never cached. Returns 0 and fills size_out,
    // or a positive errno exactly like open_blob.
    int blob_payload_size(const Hash& hash, uint64_t& size_out) const;

    Hash write_tree(const std::vector<uint8_t>& serialized);
    bool read_tree(const Hash& hash, std::vector<uint8_t>& out);

    Hash write_commit(const std::vector<uint8_t>& serialized);
    bool read_commit(const Hash& hash, std::vector<uint8_t>& out);

    bool object_exists(const Hash& hash) const;
    std::string object_path(const Hash& hash) const;
    std::string last_error() const;

    const std::string& root() const { return store_root_; }
    const std::string& tmp_dir() const { return tmp_dir_; }

    bool fsync_objects(const std::vector<Hash>& hashes);
    bool fsync_shard_dirs(const std::vector<Hash>& hashes);
    bool fsync_pending(const std::vector<Hash>& hashes, std::string& error);

    // Tracks objects that have been written-and-renamed but whose containing
    // shard dir has not yet been fsync'd. Drained at checkpoint time so each
    // object is made durable exactly once. On fsync failure the caller
    // re-inserts the drained set via restore_pending so the next checkpoint
    // retries.
    //
    // fsync_pending() is the subset-safe counterpart for code paths that need
    // to publish one object before the next full checkpoint. It fsyncs only the
    // requested objects/directories and erases only those hashes from pending_,
    // so concurrent checkpoint/merge publishers never see unrelated objects
    // temporarily hidden from drain_pending().
    std::vector<Hash> drain_pending();
    void restore_pending(const std::vector<Hash>& hashes);
    size_t pending_count() const;

private:
    Hash write_object(const char* type_tag, const uint8_t* body, size_t body_len);
    bool read_object(const Hash& hash, const char* expected_tag, std::vector<uint8_t>& out);
    void set_last_error(std::string error) const;

    std::string store_root_;
    std::string objects_dir_;
    std::string tmp_dir_;

    mutable std::mutex pending_mu_;
    std::set<Hash> pending_;

    mutable std::mutex error_mu_;
    mutable std::string last_error_;

    // hash→payload-size cache for blob_payload_size(). The key is already
    // a uniformly distributed content hash, so its first bytes serve as
    // the map hash directly.
    struct HashKey {
        size_t operator()(const Hash& h) const noexcept {
            size_t v;
            std::memcpy(&v, h.data(), sizeof(v));
            return v;
        }
    };
    static constexpr size_t kSizeCacheCap = 65536;
    mutable std::shared_mutex size_cache_mu_;
    mutable std::unordered_map<Hash, uint64_t, HashKey> size_cache_;
};

} // namespace cas
