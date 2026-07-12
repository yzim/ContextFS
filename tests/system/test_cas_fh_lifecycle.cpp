#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

static int ctl_send(const char* sock, const char* line, char* resp, size_t rlen) {
    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a{}; a.sun_family = AF_UNIX;
    std::strncpy(a.sun_path, sock, sizeof(a.sun_path) - 1);
    if (connect(c, (sockaddr*)&a, sizeof(a)) != 0) { close(c); return -1; }
    std::string l = std::string(line) + "\n";
    if (write(c, l.data(), l.size()) != (ssize_t)l.size()) { close(c); return -1; }
    ssize_t n = read(c, resp, rlen - 1);
    close(c);
    if (n <= 0) return -1;
    resp[n] = '\0';
    return 0;
}

// Build the deterministic 64-KiB pattern: byte i = (char)(i % 256).
static std::vector<uint8_t> make_pattern(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(i % 256);
    return v;
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s <mountpoint> <sock>\n", argv[0]); return 1; }
    const char* mnt  = argv[1];
    const char* sock = argv[2];
    std::string target = std::string(mnt) + "/lifecycle.txt";
    char resp[1024];

    // ── (1) 64-KiB deterministic file: exercise reads at block boundaries,
    //    mid-block, and exactly at EOF across the fd-backed read path.
    constexpr size_t KIB64 = 65536;
    std::string big_path = std::string(mnt) + "/big.bin";
    std::vector<uint8_t> pattern = make_pattern(KIB64);
    {
        int fd = open(big_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { std::perror("open big"); return 100; }
        size_t off = 0;
        while (off < KIB64) {
            ssize_t n = write(fd, pattern.data() + off, KIB64 - off);
            if (n <= 0) { std::perror("write big"); return 101; }
            off += (size_t)n;
        }
        if (close(fd) != 0) { std::perror("close big"); return 102; }
    }

    int rfd = open(big_path.c_str(), O_RDONLY);
    if (rfd < 0) { std::perror("open big ro"); return 103; }
    {
        struct stat st{};
        if (fstat(rfd, &st) != 0 || st.st_size != (off_t)KIB64) {
            std::fprintf(stderr, "FAIL: big size=%lld expected %zu\n",
                         (long long)st.st_size, KIB64);
            return 104;
        }
        struct Off { off_t off; const char* name; } offs[] = {
            {0,     "0"},
            {1,     "1"},
            {4093,  "4093"},
            {32768, "32768"},
        };
        for (const auto& o : offs) {
            uint8_t buf[256];
            size_t want = sizeof(buf);
            size_t avail = KIB64 - (size_t)o.off;
            size_t expect_n = want < avail ? want : avail;
            ssize_t n = pread(rfd, buf, want, o.off);
            if (n != (ssize_t)expect_n) {
                std::fprintf(stderr, "FAIL: big pread off=%s got %zd expected %zu\n",
                             o.name, n, expect_n);
                return 105;
            }
            if (std::memcmp(buf, pattern.data() + o.off, (size_t)n) != 0) {
                std::fprintf(stderr, "FAIL: big content mismatch off=%s\n", o.name);
                return 106;
            }
        }
        // Exactly at EOF: pread returns 0 bytes.
        {
            uint8_t buf[16];
            ssize_t n = pread(rfd, buf, sizeof(buf), (off_t)KIB64);
            if (n != 0) {
                std::fprintf(stderr, "FAIL: big EOF pread got %zd expected 0\n", n);
                return 107;
            }
        }
    }
    close(rfd);

    // ── (2) lifecycle.txt O_RDWR flow: verify initial bytes, then write
    //    WORLD, fsync, and immediately re-read — this catches accidental
    //    reuse of the retained pre-write blob fd after flush (the handle is
    //    now mutated and must serve the overlay, not the immutable blob).
    int fd = open(target.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { std::perror("open"); return 2; }
    if (write(fd, "hello", 5) != 5) { std::perror("write"); return 3; }

    struct stat st{};
    if (fstat(fd, &st) != 0) { std::perror("fstat"); return 4; }
    if (st.st_size != 5) { std::fprintf(stderr, "FAIL: fstat size=%lld expected 5\n", (long long)st.st_size); return 5; }

    char buf[16] = {};
    if (pread(fd, buf, 5, 0) != 5 || std::memcmp(buf, "hello", 5) != 0) {
        std::fprintf(stderr, "FAIL: read-your-writes buf=%s\n", buf); return 6;
    }

    if (ctl_send(sock, "checkpoint v1", resp, sizeof(resp)) != 0 || !std::strstr(resp, "\"ok\":true")) {
        std::fprintf(stderr, "FAIL: checkpoint resp=%s\n", resp); return 7;
    }

    if (pwrite(fd, "WORLD", 5, 0) != 5) { std::perror("pwrite"); return 8; }

    // fsync forces a flush path; the subsequent read must reflect WORLD even
    // though the handle may still hold a stale fd view.
    if (fsync(fd) != 0) { std::perror("fsync"); return 13; }

    std::memset(buf, 0, sizeof(buf));
    if (pread(fd, buf, 5, 0) != 5 || std::memcmp(buf, "WORLD", 5) != 0) {
        std::fprintf(stderr, "FAIL: post-fsync read-your-writes buf=%s\n", buf); return 14;
    }

    // ── (3) Rollback then ESTALE on the same handle.
    if (ctl_send(sock, "rollback v1", resp, sizeof(resp)) != 0 || !std::strstr(resp, "\"ok\":true")) {
        std::fprintf(stderr, "FAIL: rollback resp=%s\n", resp); return 9;
    }

    ssize_t r = pread(fd, buf, 5, 0);
    if (r >= 0 || errno != ESTALE) {
        std::fprintf(stderr, "FAIL: expected ESTALE after rollback, got r=%zd errno=%d\n", r, errno); return 10;
    }
    close(fd);

    fd = open(target.c_str(), O_RDONLY);
    if (fd < 0) { std::perror("reopen"); return 11; }
    if (read(fd, buf, 5) != 5 || std::memcmp(buf, "hello", 5) != 0) {
        std::fprintf(stderr, "FAIL: post-rollback content mismatch\n"); return 12;
    }
    close(fd);

    std::printf("PASS test_cas_fh_lifecycle\n");
    return 0;
}
