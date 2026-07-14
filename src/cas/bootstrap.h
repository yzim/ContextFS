#pragma once
#include "hash.h"
#include "inode_map.h"
#include "object_store.h"
#include "working_tree.h"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace cas {

class Bootstrap {
public:
    Bootstrap(std::string source_root,
              ObjectStore& store,
              WorkingTree& wt,
              InodeMap& inode_map,
              std::mutex& checkpoint_mu);
    ~Bootstrap();

    bool ensure_path(const std::string& vpath);

    void start_background();
    void stop_background();

    bool pending() const { return pending_.load(); }

private:
    enum class IngestResult {
        Inserted,
        Missing,
        Unsupported,
        Failed,
    };

    // ingest_entry derives kind/size/symlink-target itself from
    // std::filesystem::symlink_status rather than taking a stat struct —
    // that way this header doesn't have to drag in <sys/stat.h> (or its
    // MSVC-incompatible cousin) into every translation unit that includes
    // bootstrap.h transitively via daemon.h.
    IngestResult ingest_entry(const std::string& vpath,
                              const std::string& source_abspath,
                              std::string& error);
    void walk_bg();

    std::string source_root_;
    ObjectStore& store_;
    WorkingTree& wt_;
    InodeMap& inode_map_;
    std::mutex& checkpoint_mu_;

    std::thread bg_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> pending_{true};
};

} // namespace cas
