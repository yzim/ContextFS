// Cooperative runtime counter fixture.
//
// Driven entirely by a FIFO whose path is argv[1]. The counter lives ONLY in
// process memory; the only way it reaches disk is an explicit `write` command.
// That is the integration proof (in Task 6's system test) that a restore
// resumed from process memory rather than from the filesystem.
//
// Commands (one per line):
//   inc              -> counter++
//   write <path>     -> write "counter=<value>\n" to <path>
//   boundary <kind>  -> call agentvfs_runtime_boundary(kind, err, sizeof err)
//   exit             -> return 0

#include "runtime_client.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>

namespace {

// Returns true if the program should keep running (false => exit).
bool process_line(const std::string& line, int& counter) {
    // Trim leading whitespace.
    size_t start = 0;
    while (start < line.size() &&
           (line[start] == ' ' || line[start] == '\t' || line[start] == '\r')) {
        ++start;
    }
    std::string s = line.substr(start);
    if (s.empty()) return true;

    if (s == "exit") return false;

    if (s == "inc") {
        ++counter;
        return true;
    }

    const char* kWrite = "write";
    const size_t kWriteLen = 5;
    if (s.size() >= kWriteLen &&
        s.compare(0, kWriteLen, kWrite) == 0 &&
        (s.size() == kWriteLen || s[kWriteLen] == ' ' || s[kWriteLen] == '\t')) {
        size_t arg_off = kWriteLen;
        while (arg_off < s.size() && (s[arg_off] == ' ' || s[arg_off] == '\t')) {
            ++arg_off;
        }
        std::string path = s.substr(arg_off);
        if (path.empty()) {
            std::fprintf(stderr, "fixture: write requires a path\n");
            return true;
        }
        char numbuf[32];
        std::snprintf(numbuf, sizeof(numbuf), "%d", counter);
        std::string body = "counter=";
        body += numbuf;
        body += "\n";
        int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            std::fprintf(stderr, "fixture: open %s: %s\n", path.c_str(),
                         std::strerror(errno));
            return true;
        }
        size_t off = 0;
        while (off < body.size()) {
            ssize_t n = write(fd, body.data() + off, body.size() - off);
            if (n < 0) {
                if (errno == EINTR) continue;
                std::fprintf(stderr, "fixture: write %s: %s\n", path.c_str(),
                             std::strerror(errno));
                break;
            }
            off += static_cast<size_t>(n);
        }
        close(fd);
        return true;
    }

    const char* kBoundary = "boundary";
    const size_t kBoundaryLen = 8;
    if (s.size() >= kBoundaryLen &&
        s.compare(0, kBoundaryLen, kBoundary) == 0 &&
        (s.size() == kBoundaryLen ||
         s[kBoundaryLen] == ' ' || s[kBoundaryLen] == '\t')) {
        size_t arg_off = kBoundaryLen;
        while (arg_off < s.size() && (s[arg_off] == ' ' || s[arg_off] == '\t')) {
            ++arg_off;
        }
        std::string kind = s.substr(arg_off);
        if (kind.empty()) kind = "manual";
        char err[256] = {};
        if (agentvfs_runtime_boundary(kind.c_str(), err, sizeof(err)) != 0) {
            std::fprintf(stderr, "fixture: boundary failed: %s\n", err);
        }
        return true;
    }

    std::fprintf(stderr, "fixture: unknown command: %s\n", s.c_str());
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <fifo-path>\n", argv[0]);
        return 2;
    }
    const char* fifo = argv[1];

    int counter = 0;
    std::string pending;

    // Open the FIFO blocking; reopen on writer-EOF so the harness may write
    // one line per writer OR batch many lines through one writer.
    while (true) {
        int fd = open(fifo, O_RDONLY);
        if (fd < 0) {
            std::fprintf(stderr, "fixture: open %s: %s\n", fifo,
                         std::strerror(errno));
            return 2;
        }
        char buf[256];
        while (true) {
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n < 0) {
                if (errno == EINTR) continue;
                std::fprintf(stderr, "fixture: read: %s\n",
                             std::strerror(errno));
                close(fd);
                return 2;
            }
            if (n == 0) break;  // writer closed; reopen below
            for (ssize_t i = 0; i < n; ++i) {
                char c = buf[i];
                if (c == '\n') {
                    if (!process_line(pending, counter)) {
                        close(fd);
                        return 0;
                    }
                    pending.clear();
                } else {
                    pending.push_back(c);
                }
            }
        }
        close(fd);
        // Loop back and reopen the FIFO for the next writer. If a partial
        // line was buffered across an EOF (unusual), keep it pending.
    }
}
