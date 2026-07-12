// agentvfs_fuse_io_bench: deterministic Linux FUSE syscall microbenchmark.
//
// Each invocation runs one case for a fixed number of iterations against a
// fixture rooted at <root> and emits exactly one JSON line on stdout:
//
//   {"case":<name>,"elapsed_ns":<ns>,"operations":<n>,
//    "ops_per_second":<rate>,"checksum":<fnv1a>}
//
// Only read-small, seq-read and random-read update the checksum (FNV-1a 64-bit
// over the bytes they return). create-write-close cleans its files up *after*
// the timer stops; create-unlink unlinks inside the timer.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <map>
#include <functional>
#include <chrono>
#include <filesystem>
#include <algorithm>
#include <stdexcept>

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

namespace {

constexpr uint64_t FNV_OFFSET_BASIS = 14695981039346656037ULL;
constexpr uint64_t FNV_PRIME = 1099511628211ULL;
constexpr size_t CHUNK_4K = 4096;
constexpr size_t CHUNK_1M = 1024 * 1024;
constexpr uint64_t RANDOM_SEED = 88172645463325252ULL;

uint64_t fnv1a_update(uint64_t hash, const unsigned char* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        hash ^= static_cast<uint64_t>(data[i]);
        hash *= FNV_PRIME;
    }
    return hash;
}

// Standard xorshift64 (Marsaglia). Advances state in place and returns the
// post-mix value used to derive the next aligned read offset.
uint64_t xorshift64(uint64_t& state) {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * 0x2545F4914F6CDD1DULL;
}

int usage() {
    std::fprintf(stderr,
                 "Usage: agentvfs_fuse_io_bench <case> <root> <iterations>\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        return usage();
    }

    const std::string case_name = argv[1];
    const std::string root = argv[2];
    const std::string iterations_raw = argv[3];

    // Reject non-positive iteration counts (including a leading '-').
    // strtoull silently wraps "-5" to a huge unsigned value, so the sign
    // has to be screened off explicitly before parsing.
    if (iterations_raw.empty() || iterations_raw[0] == '-') {
        return usage();
    }
    char* endp = nullptr;
    errno = 0;
    const unsigned long long iterations_ll =
        std::strtoull(iterations_raw.c_str(), &endp, 10);
    if (errno != 0 || endp == iterations_raw.c_str() || *endp != '\0' ||
        iterations_ll == 0) {
        return usage();
    }
    const uint64_t iterations = static_cast<uint64_t>(iterations_ll);

    const std::filesystem::path root_path(root);
    const std::filesystem::path data_dir = root_path / "data";
    const std::filesystem::path large_file = root_path / "large.bin";
    const std::filesystem::path write_dir = root_path / ".bench-write";

    // Build the sorted data-file list before starting the timer.
    std::vector<std::string> data_files;
    std::error_code dir_ec;
    if (std::filesystem::is_directory(data_dir, dir_ec)) {
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(data_dir)) {
            if (entry.is_regular_file()) {
                data_files.push_back(entry.path().string());
            }
        }
    }
    std::sort(data_files.begin(), data_files.end());

    uint64_t operations = 0;
    std::vector<std::string> cleanup_files;  // create-write-close, post-timer

    std::map<std::string, std::function<uint64_t()>> cases;

    cases["stat-existing"] = [&]() -> uint64_t {
        if (data_files.empty()) {
            throw std::runtime_error("stat-existing: no data files");
        }
        struct stat st;
        for (uint64_t i = 0; i < iterations; ++i) {
            const std::string& path = data_files[i % data_files.size()];
            if (::lstat(path.c_str(), &st) != 0) {
                throw std::runtime_error(
                    std::string("stat-existing: lstat failed: ") +
                    std::strerror(errno));
            }
        }
        operations = iterations;
        return 0;
    };

    cases["lookup-missing"] = [&]() -> uint64_t {
        struct stat st;
        for (uint64_t i = 0; i < iterations; ++i) {
            const std::string path =
                (data_dir / ("missing-" + std::to_string(i) + ".dat"))
                    .string();
            if (::lstat(path.c_str(), &st) == 0) {
                throw std::runtime_error(
                    "lookup-missing: expected ENOENT but path exists: " + path);
            }
            if (errno != ENOENT) {
                throw std::runtime_error(
                    std::string("lookup-missing: unexpected errno: ") +
                    std::strerror(errno));
            }
        }
        operations = iterations;
        return 0;
    };

    cases["open-close"] = [&]() -> uint64_t {
        if (data_files.empty()) {
            throw std::runtime_error("open-close: no data files");
        }
        for (uint64_t i = 0; i < iterations; ++i) {
            const std::string& path = data_files[i % data_files.size()];
            int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) {
                throw std::runtime_error(
                    std::string("open-close: open failed: ") +
                    std::strerror(errno));
            }
            ::close(fd);
        }
        operations = iterations;
        return 0;
    };

    cases["tree-walk"] = [&]() -> uint64_t {
        uint64_t entries = 0;
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(data_dir)) {
            (void)entry;  // counting forces the iterator to be consumed
            ++entries;
        }
        (void)entries;
        operations = 1;
        return 0;
    };

    cases["read-small"] = [&]() -> uint64_t {
        if (data_files.empty()) {
            throw std::runtime_error("read-small: no data files");
        }
        std::vector<unsigned char> buf(CHUNK_4K);
        uint64_t checksum = FNV_OFFSET_BASIS;
        for (uint64_t i = 0; i < iterations; ++i) {
            const std::string& path = data_files[i % data_files.size()];
            int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) {
                throw std::runtime_error(
                    std::string("read-small: open failed: ") +
                    std::strerror(errno));
            }
            ssize_t n = ::pread(fd, buf.data(), buf.size(), 0);
            ::close(fd);
            if (n <= 0) {
                throw std::runtime_error(
                    std::string("read-small: pread failed: ") +
                    std::strerror(errno));
            }
            checksum = fnv1a_update(checksum, buf.data(),
                                    static_cast<size_t>(n));
        }
        operations = iterations;
        return checksum;
    };

    cases["seq-read"] = [&]() -> uint64_t {
        int fd = ::open(large_file.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            throw std::runtime_error(
                std::string("seq-read: open failed: ") + std::strerror(errno));
        }
        std::vector<unsigned char> buf(CHUNK_1M);
        uint64_t checksum = FNV_OFFSET_BASIS;
        for (;;) {
            ssize_t n = ::read(fd, buf.data(), buf.size());
            if (n < 0) {
                ::close(fd);
                throw std::runtime_error(
                    std::string("seq-read: read failed: ") +
                    std::strerror(errno));
            }
            if (n == 0) {
                break;
            }
            checksum = fnv1a_update(checksum, buf.data(),
                                    static_cast<size_t>(n));
        }
        ::close(fd);
        operations = 1;  // the whole sequential read is one operation
        return checksum;
    };

    cases["random-read"] = [&]() -> uint64_t {
        struct stat st;
        if (::stat(large_file.c_str(), &st) != 0) {
            throw std::runtime_error(
                std::string("random-read: stat large.bin failed: ") +
                std::strerror(errno));
        }
        const uint64_t large_size = static_cast<uint64_t>(st.st_size);
        if (large_size < CHUNK_4K) {
            throw std::runtime_error(
                "random-read: large.bin smaller than 4096 bytes");
        }
        int fd = ::open(large_file.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            throw std::runtime_error(
                std::string("random-read: open failed: ") +
                std::strerror(errno));
        }
        std::vector<unsigned char> buf(CHUNK_4K);
        uint64_t checksum = FNV_OFFSET_BASIS;
        uint64_t state = RANDOM_SEED;
        const uint64_t chunks = large_size / CHUNK_4K;
        for (uint64_t i = 0; i < iterations; ++i) {
            const uint64_t r = xorshift64(state);
            const off_t offset =
                static_cast<off_t>((r % chunks) * CHUNK_4K);
            ssize_t n = ::pread(fd, buf.data(), buf.size(), offset);
            if (n <= 0) {
                ::close(fd);
                throw std::runtime_error(
                    std::string("random-read: pread failed: ") +
                    std::strerror(errno));
            }
            checksum = fnv1a_update(checksum, buf.data(),
                                    static_cast<size_t>(n));
        }
        ::close(fd);
        operations = iterations;
        return checksum;
    };

    cases["create-write-close"] = [&]() -> uint64_t {
        std::vector<unsigned char> wbuf(CHUNK_4K, 0);
        for (uint64_t i = 0; i < iterations; ++i) {
            std::string path =
                (write_dir / ("cwc-" + std::to_string(i))).string();
            int fd = ::open(path.c_str(),
                            O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            if (fd < 0) {
                throw std::runtime_error(
                    std::string("create-write-close: open failed: ") +
                    std::strerror(errno));
            }
            ssize_t written = ::write(fd, wbuf.data(), wbuf.size());
            ::close(fd);
            if (written < 0) {
                throw std::runtime_error(
                    std::string("create-write-close: write failed: ") +
                    std::strerror(errno));
            }
            cleanup_files.push_back(std::move(path));
        }
        operations = iterations;
        return 0;
    };

    cases["create-unlink"] = [&]() -> uint64_t {
        std::vector<unsigned char> wbuf(CHUNK_4K, 0);
        for (uint64_t i = 0; i < iterations; ++i) {
            const std::string path =
                (write_dir / ("cu-" + std::to_string(i))).string();
            int fd = ::open(path.c_str(),
                            O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            if (fd < 0) {
                throw std::runtime_error(
                    std::string("create-unlink: open failed: ") +
                    std::strerror(errno));
            }
            ssize_t written = ::write(fd, wbuf.data(), wbuf.size());
            ::close(fd);
            if (written < 0) {
                throw std::runtime_error(
                    std::string("create-unlink: write failed: ") +
                    std::strerror(errno));
            }
            if (::unlink(path.c_str()) != 0) {
                throw std::runtime_error(
                    std::string("create-unlink: unlink failed: ") +
                    std::strerror(errno));
            }
        }
        operations = iterations;
        return 0;
    };

    const auto it = cases.find(case_name);
    if (it == cases.end()) {
        return usage();
    }

    uint64_t checksum = 0;
    const auto t0 = std::chrono::steady_clock::now();
    try {
        checksum = it->second();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
    const auto t1 = std::chrono::steady_clock::now();

    // create-write-close: clean up its files AFTER the timer stops.
    for (const auto& path : cleanup_files) {
        ::unlink(path.c_str());
    }

    const uint64_t elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

    std::printf(
        "{\"case\":\"%s\",\"elapsed_ns\":%llu,\"operations\":%llu,"
        "\"ops_per_second\":%.6f,\"checksum\":%llu}\n",
        case_name.c_str(),
        static_cast<unsigned long long>(elapsed_ns),
        static_cast<unsigned long long>(operations),
        elapsed_ns ? (double)operations * 1e9 / (double)elapsed_ns : 0.0,
        static_cast<unsigned long long>(checksum));

    return 0;
}
