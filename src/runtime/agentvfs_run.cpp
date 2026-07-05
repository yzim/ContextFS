// agentvfs-run: Linux launcher for a cooperative runtime.
//
//   agentvfs-run --sock <path> --branch <branch> [--id <runtime-id>] -- <command> [args...]
//
// It forks a fresh process group for the child, exports AGENTVFS_SOCK /
// AGENTVFS_RUNTIME_ID / AGENTVFS_RUNTIME_GENERATION=1 into the child's
// environment, execs the target, then registers the runtime with the daemon
// via runtime.create. The target must link runtime_client.cpp and call
// agentvfs_runtime_boundary() itself to become restorable.

#include "runtime_io.h"

#include <cerrno>
#include <fcntl.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#if defined(__linux__)
#include <sys/random.h>
#endif
#include <time.h>

namespace {

void usage(const char* prog) {
    std::fprintf(stderr,
        "usage: %s --sock <path> --branch <branch> [--id <runtime-id>] -- "
        "<command> [args...]\n",
        prog);
}

std::string hex64(unsigned long long v) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", v);
    return std::string(buf);
}

bool fill_random(void* data, size_t len) {
    unsigned char* p = static_cast<unsigned char*>(data);
    size_t done = 0;
#if defined(__linux__)
    while (done < len) {
        ssize_t n = getrandom(p + done, len - done, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        done += static_cast<size_t>(n);
    }
    if (done == len) return true;
#endif
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    done = 0;
    while (done < len) {
        ssize_t n = read(fd, p + done, len - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return false;
        }
        if (n == 0) {
            close(fd);
            return false;
        }
        done += static_cast<size_t>(n);
    }
    close(fd);
    return true;
}

bool make_token(std::string& token) {
    unsigned long long words[2] = {};
    if (!fill_random(words, sizeof(words))) return false;
    token = hex64(words[0]) + hex64(words[1]);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::string sock;
    std::string branch;
    std::string id;
    bool have_dashdash = false;
    std::vector<std::string> cmd;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (have_dashdash) {
            cmd.push_back(a);
            continue;
        }
        if (a == "--") {
            have_dashdash = true;
            continue;
        }
        if (a == "--sock") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "agentvfs-run: --sock requires a value\n");
                return 2;
            }
            sock = argv[++i];
        } else if (a == "--branch") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "agentvfs-run: --branch requires a value\n");
                return 2;
            }
            branch = argv[++i];
        } else if (a == "--id") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "agentvfs-run: --id requires a value\n");
                return 2;
            }
            id = argv[++i];
        } else {
            std::fprintf(stderr, "agentvfs-run: unexpected argument '%s'\n",
                         a.c_str());
            usage(argv[0]);
            return 2;
        }
    }

    if (sock.empty() || branch.empty() || !have_dashdash || cmd.empty()) {
        usage(argv[0]);
        return 2;
    }

    // Generate a locally-unique ASCII runtime id when one was not supplied.
    if (id.empty()) {
        struct timespec ts;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
            std::fprintf(stderr, "agentvfs-run: clock_gettime: %s\n",
                         std::strerror(errno));
            return 2;
        }
        unsigned long long ns =
            static_cast<unsigned long long>(ts.tv_sec) * 1000000000ULL +
            static_cast<unsigned long long>(ts.tv_nsec);
        char idbuf[64];
        std::snprintf(idbuf, sizeof(idbuf), "rt-%d-%llu",
                      static_cast<int>(getpid()), ns);
        id = idbuf;
    }
    std::string token;
    if (!make_token(token)) {
        std::fprintf(stderr, "agentvfs-run: failed to generate runtime token\n");
        return 1;
    }

    const pid_t child = fork();
    if (child < 0) {
        std::fprintf(stderr, "agentvfs-run: fork: %s\n", std::strerror(errno));
        return 1;
    }

    if (child == 0) {
        // ---- Child: own process group, then exec the target with the
        // cooperative-runtime env vars populated.
        setpgid(0, 0);

        setenv("AGENTVFS_SOCK", sock.c_str(), 1);
        setenv("AGENTVFS_RUNTIME_ID", id.c_str(), 1);
        setenv("AGENTVFS_RUNTIME_TOKEN", token.c_str(), 1);
        setenv("AGENTVFS_RUNTIME_GENERATION", "1", 1);

        // Build a writable argv for execvp.
        std::vector<char*> argv2;
        argv2.reserve(cmd.size() + 1);
        for (auto& c : cmd) argv2.push_back(&c[0]);
        argv2.push_back(nullptr);

        execvp(argv2[0], argv2.data());
        // execvp only returns on failure.
        std::fprintf(stderr, "agentvfs-run: exec %s: %s\n", argv2[0],
                     std::strerror(errno));
        _exit(127);
    }

    // ---- Parent. Ensure the child landed in its own process group before we
    // report its pgid (both parent and child race to setpgid; whichever runs
    // last is a no-op against the same target value). The parent-side
    // setpgid(child, child) can only fail with EINTR/EACCES/ESRCH, all benign
    // here: the child already did setpgid(0,0), so the target group is
    // established regardless of which side wins the race. Retry EINTR; the
    // other races are swallowed (the child exec'd or exited into its group).
    while (setpgid(child, child) != 0 && errno == EINTR) {
    }
    // Resolve the child's process group id, retrying on EINTR. The child did
    // setpgid(0,0) so its pgid equals its pid; on getpgid failure (ESRCH if it
    // already exited, EPERM, ...) fall back to the child pid rather than
    // registering the runtime with process_group_id=-1, which would make a
    // later restore's freeze of the (bogus) pgid fail opaquely.
    pid_t child_pgid_raw = getpgid(child);
    while (child_pgid_raw < 0 && errno == EINTR) {
        child_pgid_raw = getpgid(child);
    }
    if (child_pgid_raw < 0) {
        std::fprintf(stderr,
                     "agentvfs-run: getpgid(%d) failed: %s; falling back to child pid as pgid\n",
                     static_cast<int>(child), std::strerror(errno));
        child_pgid_raw = child;
    }
    const long child_pgid = static_cast<long>(child_pgid_raw);

    char rootpidbuf[32];
    std::snprintf(rootpidbuf, sizeof(rootpidbuf), "%ld",
                  static_cast<long>(child));
    char pgidbuf[32];
    std::snprintf(pgidbuf, sizeof(pgidbuf), "%ld", child_pgid);

    const std::string command_ref = "argv:" + cmd[0];

    std::string create_req =
        "runtime.create {\"runtime_id\":\"" + cas::runtime_io::json_escape(id) +
        "\",\"branch\":\"" + cas::runtime_io::json_escape(branch) +
        "\",\"root_pid\":" + rootpidbuf +
        ",\"process_group_id\":" + pgidbuf +
        ",\"command_ref\":\"" + cas::runtime_io::json_escape(command_ref) +
        "\",\"control_token\":\"" + cas::runtime_io::json_escape(token) +
        "\",\"cooperative\":true}";

    std::string cresp;
    std::string cerrmsg;
    bool create_ok = cas::runtime_io::control_request(sock, create_req, cresp, cerrmsg) &&
                     cas::runtime_io::extract_bool(cresp, "ok");
    std::string create_error_detail;
    if (!create_ok) {
        create_error_detail = cerrmsg.empty() ? cresp : cerrmsg;
    }

    // Print the runtime id so callers (and Task 6's test) can capture it.
    std::printf("%s\n", id.c_str());
    std::fflush(stdout);

    if (!create_ok) {
        std::fprintf(stderr, "agentvfs-run: runtime.create failed: %s\n",
                     create_error_detail.c_str());
    }

    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) continue;
        std::fprintf(stderr, "agentvfs-run: waitpid: %s\n",
                     std::strerror(errno));
        return 1;
    }

    if (!create_ok) return 1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}
