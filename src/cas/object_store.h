#pragma once
#include "hash.h"
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace cas {

class ObjectStore {
public:
    explicit ObjectStore(const std::string& store_root);

    bool init_layout();
    void cleanup_tmp();

    Hash write_blob(const uint8_t* data, size_t len);
    Hash write_blob(const std::vector<uint8_t>& data);
    bool read_blob(const Hash& hash, std::vector<uint8_t>& out);

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

    // Tracks objects that have been written-and-renamed but whose containing
    // shard dir has not yet been fsync'd. Drained at checkpoint time so each
    // object is made durable exactly once. On fsync failure the caller
    // re-inserts the drained set via restore_pending so the next checkpoint
    // retries.
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
};

} // namespace cas
