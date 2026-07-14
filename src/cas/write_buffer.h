#pragma once
#include "hash.h"
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace cas {

static constexpr uint64_t WRITE_BUFFER_MAX_BYTES = 64 * 1024 * 1024; // 64MB per-fh cap

class WriteBuffer {
public:
    explicit WriteBuffer(const Hash& base_blob, uint64_t base_size);

    void write(uint64_t offset, const uint8_t* data, size_t len);
    size_t read(uint64_t offset, uint8_t* buf, size_t len,
                const std::vector<uint8_t>& base_data) const;

    void truncate(uint64_t new_size);
    uint64_t effective_size(uint64_t base_size) const;

    bool over_cap() const { return dirty_bytes_ >= WRITE_BUFFER_MAX_BYTES; }
    bool is_dirty() const { return dirty_bytes_ > 0 || size_override_.has_value(); }
    // Bytes of dirty overlay data outstanding (the mem-and-gc stats.memory
    // command sums this across open file handles).
    uint64_t dirty_bytes() const { return dirty_bytes_; }

    std::vector<uint8_t> materialize(const std::vector<uint8_t>& base_data) const;

    void clear();

    const Hash& base_blob() const { return base_blob_; }

private:
    Hash base_blob_;
    uint64_t base_size_;
    std::map<uint64_t, std::vector<uint8_t>> overlay_;
    std::optional<uint64_t> size_override_;
    uint64_t dirty_bytes_ = 0;
};

} // namespace cas
