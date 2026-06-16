#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

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

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s <mountpoint> <sock>\n", argv[0]); return 1; }
    const char* mnt  = argv[1];
    const char* sock = argv[2];
    std::string target = std::string(mnt) + "/lifecycle.txt";
    char resp[1024];

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
