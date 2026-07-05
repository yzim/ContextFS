// Shared newline-delimited control-protocol I/O helpers used by the
// cooperative-runtime client (runtime_client.cpp, linked into the target
// program) and the launcher (agentvfs_run.cpp). Kept header-only + inline so
// the two translation units (which live in different binaries and do not share
// a library) get identical behavior for the connect/write/read-until-newline
// idiom and the flat-JSON field extraction the protocol relies on.
//
// Behavior is deliberately fixed: a 64 KiB per-line cap, the same JSON string
// escaping, and the same connect+write+read-one-line exchange.

#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

#include <unistd.h>

#include <sys/socket.h>
#include <sys/un.h>

namespace cas {
namespace runtime_io {

// Matches ControlSocket::serve_client's per-line cap.
constexpr size_t kMaxLine = 64 * 1024;

inline std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Extract a boolean field from flat JSON. Returns `dflt` when the key is
// absent or the value is neither true nor false.
inline bool extract_bool(const std::string& json, const std::string& key,
                         bool dflt = false) {
    std::string needle = "\"" + key + "\"";
    auto p = json.find(needle);
    if (p == std::string::npos) return dflt;
    p = json.find(':', p + needle.size());
    if (p == std::string::npos) return dflt;
    ++p;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
    if (json.compare(p, 4, "true") == 0) return true;
    if (json.compare(p, 5, "false") == 0) return false;
    return dflt;
}

inline bool write_all_fd(int fd, const std::string& body) {
    size_t off = 0;
    while (off < body.size()) {
        ssize_t n = write(fd, body.data() + off, body.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

// Read one line (up to '\n', bounded to kMaxLine) from a connected socket.
inline bool read_line_fd(int fd, std::string& out) {
    out.clear();
    char ch;
    while (true) {
        ssize_t n = read(fd, &ch, 1);
        if (n == 1) {
            if (ch == '\n') return true;
            out.push_back(ch);
            if (out.size() > kMaxLine) return false;
        } else if (n == 0) {
            return false;  // EOF before newline
        } else {
            if (errno == EINTR) continue;
            return false;
        }
    }
}

// One full request/response exchange: connect, send "line\n", read one line.
// Mirrors the connect+write+read-until-newline idiom in
// tests/unit/test_branch_merge_daemon.cpp's control_request().
inline bool control_request(const std::string& sock_path,
                            const std::string& line,
                            std::string& response,
                            std::string& error) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        error = std::strerror(errno);
        return false;
    }
    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    if (sock_path.size() >= sizeof(addr.sun_path)) {
        error = "socket path too long";
        close(fd);
        return false;
    }
    std::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
                sizeof(addr)) != 0) {
        error = std::strerror(errno);
        close(fd);
        return false;
    }
    if (!write_all_fd(fd, line + "\n")) {
        error = std::strerror(errno);
        close(fd);
        return false;
    }
    bool ok = read_line_fd(fd, response);
    if (!ok) error = "no response line";
    close(fd);
    return ok;
}

} // namespace runtime_io
} // namespace cas
