#pragma once
#include "hash.h"
#include <string>
#include <vector>

namespace cas {

class Refs {
public:
    explicit Refs(const std::string& store_root);

    bool read_ref(const std::string& branch, Hash& out) const;
    bool write_ref(const std::string& branch, const Hash& commit_hash,
                   const std::string& tmp_dir);
    bool remove_ref(const std::string& branch);

    // Error-reporting enumeration used by correctness-sensitive callers such
    // as GC. On failure `names` is cleared and `error` describes the scan
    // failure, so callers cannot mistake a partial result for no roots.
    bool list_refs(std::vector<std::string>& names, std::string& error) const;

    // Compatibility wrapper for callers whose existing contract cannot
    // surface enumeration errors. New root-discovery code should use the
    // error-reporting overload above.
    std::vector<std::string> list_refs() const;

    // Backward-compatible wrappers
    bool read_main(Hash& out) const { return read_ref("main", out); }
    bool write_main(const Hash& h, const std::string& tmp_dir) {
        return write_ref("main", h, tmp_dir);
    }

private:
    std::string refs_dir_;
};

} // namespace cas
