#include "workspace_cli.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace cas {
namespace workspace {

namespace {

// AF_UNIX sun_path is 108 bytes on Linux. Leave room for the trailing NUL.
constexpr size_t kMaxSunPath = 107;

}  // namespace

bool socket_path_fits(const std::string& socket_path) {
    return socket_path.size() <= kMaxSunPath;
}

bool is_valid_workspace_name(const std::string& name) {
    if (name.empty() || name == "." || name == ".." || name.size() > 80) return false;
    for (unsigned char ch : name) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.') continue;
        return false;
    }
    return true;
}

std::string default_workspace_root() {
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    if (xdg && *xdg) return std::string(xdg) + "/agentvfs";
    return "/tmp/agentvfs-" + std::to_string((long long)getuid());
}

WorkspacePaths make_workspace_paths(const std::string& workspace_root,
                                    const std::string& name) {
    WorkspacePaths paths;
    paths.name = name;
    paths.root = workspace_root + "/" + name;
    paths.source = paths.root + "/source";
    paths.mount = paths.root + "/mount";
    paths.store = paths.root + "/store";
    paths.socket = paths.root + "/control.sock";
    paths.log = paths.root + "/daemon.log";
    paths.session_json = paths.root + "/session.json";
    return paths;
}

std::map<std::string, std::string> parse_key_value_lines(const std::string& text) {
    std::map<std::string, std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        out[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return out;
}

static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

static bool json_find_key(const std::string& json,
                          const std::string& key,
                          size_t& after_colon) {
    std::string needle = "\"" + key + "\"";
    size_t search_from = 0;
    while (search_from <= json.size()) {
        size_t p = json.find(needle, search_from);
        if (p == std::string::npos) return false;

        // Verify the match is at a key position: preceded by '{' or ',' (with
        // optional whitespace), or at byte 0.
        bool valid_prefix = false;
        if (p == 0) {
            valid_prefix = true;
        } else {
            size_t q = p;
            while (q > 0) {
                char c = json[q - 1];
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                    q--;
                    continue;
                }
                if (c == '{' || c == ',') valid_prefix = true;
                break;
            }
        }

        if (valid_prefix) {
            // After the closing quote of the key, allow optional whitespace then ':'.
            size_t after = p + needle.size();
            while (after < json.size() &&
                   (json[after] == ' ' || json[after] == '\t' ||
                    json[after] == '\n' || json[after] == '\r')) {
                after++;
            }
            if (after < json.size() && json[after] == ':') {
                after_colon = after + 1;
                return true;
            }
        }

        search_from = p + 1;
    }
    return false;
}

static bool json_get_string(const std::string& json,
                            const std::string& key,
                            std::string& value) {
    size_t p = 0;
    if (!json_find_key(json, key, p)) return false;
    p = json.find('"', p);
    if (p == std::string::npos) return false;
    p++;

    value.clear();
    while (p < json.size()) {
        char c = json[p++];
        if (c == '"') return true;
        if (c == '\\') {
            if (p >= json.size()) return false;
            char esc = json[p++];
            if (esc == '"' || esc == '\\') value.push_back(esc);
            else if (esc == 'n') value.push_back('\n');
            else if (esc == 'r') value.push_back('\r');
            else if (esc == 't') value.push_back('\t');
            else return false;
        } else {
            value.push_back(c);
        }
    }
    return false;
}

static bool json_get_long(const std::string& json,
                          const std::string& key,
                          long& value) {
    size_t p = 0;
    if (!json_find_key(json, key, p)) return false;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) p++;
    char* end = nullptr;
    errno = 0;
    long parsed = std::strtol(json.c_str() + p, &end, 10);
    if (errno != 0 || end == json.c_str() + p) return false;
    value = parsed;
    return true;
}

static bool json_get_bool(const std::string& json,
                          const std::string& key,
                          bool& value) {
    size_t p = 0;
    if (!json_find_key(json, key, p)) return false;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) p++;
    std::string rest = json.substr(p);
    if (rest.substr(0, 4) == "true") {
        value = true;
        return true;
    }
    if (rest.substr(0, 5) == "false") {
        value = false;
        return true;
    }
    return false;
}

std::string session_to_json(const SessionState& state) {
    std::ostringstream out;
    out << "{\n"
        << "  \"name\":\"" << json_escape(state.name) << "\",\n"
        << "  \"pid\":" << state.pid << ",\n"
        << "  \"root\":\"" << json_escape(state.root) << "\",\n"
        << "  \"source\":\"" << json_escape(state.source) << "\",\n"
        << "  \"mount\":\"" << json_escape(state.mount) << "\",\n"
        << "  \"store\":\"" << json_escape(state.store) << "\",\n"
        << "  \"socket\":\"" << json_escape(state.socket) << "\",\n"
        << "  \"log\":\"" << json_escape(state.log) << "\",\n"
        << "  \"telemetry\":\"" << json_escape(state.telemetry) << "\",\n"
        << "  \"status\":\"" << json_escape(state.status) << "\",\n"
        << "  \"created_at\":\"" << json_escape(state.created_at) << "\"\n"
        << "}\n";
    return out.str();
}

bool parse_session_json(const std::string& json,
                        SessionState& state,
                        std::string& error) {
    SessionState parsed;
    if (!json_get_string(json, "name", parsed.name) ||
        !json_get_long(json, "pid", parsed.pid) ||
        !json_get_string(json, "root", parsed.root) ||
        !json_get_string(json, "source", parsed.source) ||
        !json_get_string(json, "mount", parsed.mount) ||
        !json_get_string(json, "store", parsed.store) ||
        !json_get_string(json, "socket", parsed.socket) ||
        !json_get_string(json, "log", parsed.log) ||
        !json_get_string(json, "telemetry", parsed.telemetry) ||
        !json_get_string(json, "status", parsed.status) ||
        !json_get_string(json, "created_at", parsed.created_at)) {
        error = "session.json is missing a required field";
        return false;
    }
    state = parsed;
    error.clear();
    return true;
}

bool read_session_file(const std::string& path,
                       SessionState& state,
                       std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "cannot open " + path + ": " + std::strerror(errno);
        return false;
    }
    std::ostringstream body;
    body << in.rdbuf();
    return parse_session_json(body.str(), state, error);
}

bool write_session_file(const std::string& path,
                        const SessionState& state,
                        std::string& error) {
    std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            error = "cannot write " + tmp + ": " + std::strerror(errno);
            return false;
        }
        out << session_to_json(state);
        if (!out) {
            error = "failed writing " + tmp;
            return false;
        }
    }
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        error = "rename " + tmp + " to " + path + ": " + std::strerror(errno);
        return false;
    }
    error.clear();
    return true;
}

std::string workspace_config_to_json(const WorkspaceConfig& config) {
    std::ostringstream out;
    out << "{\n"
        << "  \"mount_override\":\"" << json_escape(config.mount_override) << "\",\n"
        << "  \"allow_root\":" << (config.allow_root ? "true" : "false") << "\n"
        << "}\n";
    return out.str();
}

bool parse_workspace_config_json(const std::string& json,
                                 WorkspaceConfig& config,
                                 std::string& error) {
    // Body must be a JSON object. Reject garbage / truncated input.
    size_t i = 0;
    while (i < json.size() &&
           (json[i] == ' ' || json[i] == '\t' ||
            json[i] == '\n' || json[i] == '\r')) i++;
    if (i >= json.size() || json[i] != '{') {
        error = "workspace.json: expected JSON object";
        return false;
    }
    size_t j = json.size();
    while (j > 0 &&
           (json[j-1] == ' ' || json[j-1] == '\t' ||
            json[j-1] == '\n' || json[j-1] == '\r')) j--;
    if (j == 0 || json[j-1] != '}') {
        error = "workspace.json: truncated or unterminated object";
        return false;
    }

    WorkspaceConfig parsed;
    // mount_override and allow_root are optional. Missing keys are valid
    // (forward-compat). Keys present but unparseable is an error.
    size_t after_colon = 0;
    if (json_find_key(json, "mount_override", after_colon)) {
        if (!json_get_string(json, "mount_override", parsed.mount_override)) {
            error = "workspace.json: malformed mount_override value";
            return false;
        }
    }
    if (json_find_key(json, "allow_root", after_colon)) {
        if (!json_get_bool(json, "allow_root", parsed.allow_root)) {
            error = "workspace.json: malformed allow_root value";
            return false;
        }
    }
    config = parsed;
    error.clear();
    return true;
}

bool read_workspace_config_file(const std::string& path,
                                WorkspaceConfig& config,
                                std::string& error) {
    config = WorkspaceConfig{};
    error.clear();
    std::ifstream in(path);
    if (!in) {
        if (errno == ENOENT) return true; // missing file = default config
        error = "cannot open " + path + ": " + std::strerror(errno);
        return false;
    }
    std::ostringstream body;
    body << in.rdbuf();
    return parse_workspace_config_json(body.str(), config, error);
}

bool write_workspace_config_file(const std::string& path,
                                 const WorkspaceConfig& config,
                                 std::string& error) {
    std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            error = "cannot write " + tmp + ": " + std::strerror(errno);
            return false;
        }
        out << workspace_config_to_json(config);
        if (!out) {
            error = "failed writing " + tmp;
            return false;
        }
    }
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        error = "rename " + tmp + " to " + path + ": " + std::strerror(errno);
        return false;
    }
    error.clear();
    return true;
}

std::string resolve_mount_path(const std::string& cli_override,
                               const std::string& config_override,
                               const std::string& default_path) {
    if (!cli_override.empty()) return cli_override;
    if (!config_override.empty()) return config_override;
    return default_path;
}

std::string lexical_normalize_absolute(const std::string& path) {
    if (path.empty() || path[0] != '/') return path;
    std::vector<std::string> parts;
    std::string seg;
    auto flush = [&]() {
        if (seg.empty()) return;
        if (seg == ".") {
            // skip
        } else if (seg == "..") {
            if (!parts.empty()) parts.pop_back();
            // ".." at root: silently drop, matching POSIX.
        } else {
            parts.push_back(seg);
        }
        seg.clear();
    };
    for (size_t i = 1; i < path.size(); i++) {
        if (path[i] == '/') flush();
        else seg.push_back(path[i]);
    }
    flush();
    if (parts.empty()) return "/";
    std::string out;
    for (const std::string& p : parts) {
        out += "/";
        out += p;
    }
    return out;
}

std::string send_control_line(const std::string& socket_path,
                              const std::string& line,
                              std::string& error) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        error = "socket: " + std::string(std::strerror(errno));
        return {};
    }

    if (!socket_path_fits(socket_path)) {
        error = "socket path is " + std::to_string(socket_path.size()) +
                " bytes; AF_UNIX maximum is " + std::to_string(kMaxSunPath);
        close(fd);
        return {};
    }

    // Best-effort send/recv timeouts. On Linux, SO_SNDTIMEO does not bound
    // connect() for blocking sockets, but it does bound send() once connected.
    // SO_RCVTIMEO bounds the read loop below. Failure to set is non-fatal.
    struct timeval tv{};
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        error = "connect " + socket_path + ": " + std::strerror(errno);
        close(fd);
        return {};
    }

    std::string msg = line;
    if (msg.empty() || msg.back() != '\n') msg.push_back('\n');
    size_t off = 0;
    while (off < msg.size()) {
        ssize_t n = send(fd, msg.data() + off, msg.size() - off, MSG_NOSIGNAL);
        if (n <= 0) {
            error = "write control socket: " + std::string(std::strerror(errno));
            close(fd);
            return {};
        }
        off += static_cast<size_t>(n);
    }

    std::string resp;
    char ch = 0;
    bool saw_newline = false;
    while (resp.size() < 128 * 1024) {
        ssize_t n = read(fd, &ch, 1);
        if (n <= 0) break;
        if (ch == '\n') {
            saw_newline = true;
            break;
        }
        resp.push_back(ch);
    }
    if (resp.empty() && !saw_newline) {
        error = "control socket closed without response";
        close(fd);
        return {};
    }
    close(fd);
    error.clear();
    return resp;
}

bool socket_responds(const std::string& socket_path) {
    std::string error;
    std::string resp = send_control_line(socket_path, "status", error);
    return resp.find("\"ok\":true") != std::string::npos;
}

static int workspace_usage() {
    std::cerr
        << "Usage: agentvfs workspace start [name] [--root <dir>] [--mount <dir>] [--telemetry auto|none|<csv>] [--allow-root]\n"
        << "       agentvfs workspace init <name> --from <dir> [--root <dir>] [--mount <dir>]\n"
        << "       agentvfs workspace status [name] [--root <dir>]\n"
        << "       agentvfs workspace list [--root <dir>]\n"
        << "       agentvfs workspace checkpoint [name] [label] [--root <dir>]\n"
        << "       agentvfs workspace rollback [name] <label-or-hash> [--root <dir>]\n"
        << "       agentvfs workspace stop [name] [--root <dir>] [--checkpoint|--no-checkpoint]\n";
    return 2;
}

struct ParsedCommon {
    std::string name = "default";
    std::string root = default_workspace_root();
    std::string telemetry = "auto";
    bool checkpoint_on_stop = true;
    std::string label;
    std::string target;
    std::string from_dir;        // for init
    std::string mount_override;  // for init/start
    bool allow_root = false;    // for init/start
};

static bool is_dir(const std::string& path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static bool is_socket_path(const std::string& path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
}

static bool ensure_dir(const std::string& path, std::string& error) {
    if (mkdir(path.c_str(), 0700) == 0 || errno == EEXIST) {
        if (is_dir(path)) return true;
    }
    error = "cannot create directory " + path + ": " + std::strerror(errno);
    return false;
}

static bool ensure_session_dirs(const WorkspacePaths& paths, std::string& error) {
    // mount/ is created after override resolution by the caller, so we don't
    // leave a stray empty <root>/<name>/mount/ when --mount points elsewhere.
    return ensure_dir(paths.root, error) &&
           ensure_dir(paths.source, error) &&
           ensure_dir(paths.store, error);
}

// Resolve `path` to absolute (relative paths are taken against current cwd).
// Output is lexically normalized (no embedded "/./" or "/foo/.."). Returns
// false on getcwd failure so callers never persist a relative path.
static bool make_absolute(const std::string& path,
                          std::string& out,
                          std::string& error) {
    if (path.empty()) {
        out = path;
        return true;
    }
    if (path[0] == '/') {
        out = lexical_normalize_absolute(path);
        return true;
    }
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) {
        error = std::string("getcwd: ") + std::strerror(errno);
        return false;
    }
    out = lexical_normalize_absolute(std::string(cwd) + "/" + path);
    return true;
}

static bool mkdir_p(const std::string& path, std::string& error) {
    if (path.empty()) {
        error = "empty path";
        return false;
    }
    std::string cur;
    size_t i = 0;
    if (path[0] == '/') {
        cur = "/";
        i = 1;
    }
    while (i <= path.size()) {
        size_t j = path.find('/', i);
        size_t end = (j == std::string::npos) ? path.size() : j;
        if (end > i) {
            if (!cur.empty() && cur.back() != '/') cur.push_back('/');
            cur.append(path, i, end - i);
            if (mkdir(cur.c_str(), 0700) != 0 && errno != EEXIST) {
                error = "mkdir " + cur + ": " + std::strerror(errno);
                return false;
            }
            if (!is_dir(cur)) {
                error = cur + " exists and is not a directory";
                return false;
            }
        }
        if (j == std::string::npos) break;
        i = j + 1;
    }
    return true;
}

static bool dir_is_nonempty(const std::string& path) {
    DIR* dir = opendir(path.c_str());
    if (!dir) return false;
    bool found = false;
    while (dirent* entry = readdir(dir)) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        found = true;
        break;
    }
    closedir(dir);
    return found;
}

static bool prepare_mount_override(const std::string& path, std::string& error) {
    struct stat st {};
    if (lstat(path.c_str(), &st) != 0) {
        if (errno == ENOENT) return mkdir_p(path, error);
        error = "cannot stat " + path + ": " + std::strerror(errno);
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        error = path + " exists and is not a directory";
        return false;
    }
    if (dir_is_nonempty(path)) {
        std::cerr << "agentvfs: warning: --mount target " << path
                  << " is non-empty; existing files will be hidden by the FUSE overlay while mounted\n";
    }
    return true;
}

static bool pid_alive(long pid) {
    return pid > 0 && (kill((pid_t)pid, 0) == 0 || errno == EPERM);
}

static bool mountpoint_active(const std::string& path) {
#ifdef __APPLE__
    std::string cmd = "mount | grep -F -- ";
    cmd += "'";
    for (char c : path) {
        if (c == '\'') cmd += "'\\''";
        else cmd.push_back(c);
    }
    cmd += "'";
    cmd += " >/dev/null";
    int rc = system(cmd.c_str());
    return rc == 0;
#else
    std::string cmd = "mountpoint -q ";
    cmd += "'";
    for (char c : path) {
        if (c == '\'') cmd += "'\\''";
        else cmd.push_back(c);
    }
    cmd += "'";
    int rc = system(cmd.c_str());
    return rc == 0;
#endif
}

static bool path_owned_by_current_user_or_absent(const std::string& path, std::string& error) {
    struct stat st {};
    if (lstat(path.c_str(), &st) != 0) {
        if (errno == ENOENT) return true;
        error = "cannot stat " + path + ": " + std::strerror(errno);
        return false;
    }
    if (st.st_uid != geteuid()) {
        error = path + " is not owned by the current user";
        return false;
    }
    return true;
}

static bool wait_ready(const WorkspacePaths& paths, pid_t daemon_pid,
                      std::chrono::milliseconds budget = std::chrono::seconds(10)) {
    auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (daemon_pid > 0) {
            int status = 0;
            pid_t r = waitpid(daemon_pid, &status, WNOHANG);
            if (r == daemon_pid || (r < 0 && errno == ECHILD)) {
                return false;  // Daemon exited before becoming ready.
            }
        }
        if (is_socket_path(paths.socket) &&
            mountpoint_active(paths.mount) &&
            socket_responds(paths.socket)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

static void print_session(const SessionState& state) {
    std::cout << "name=" << state.name << "\n";
    std::cout << "mount=" << state.mount << "\n";
    std::cout << "socket=" << state.socket << "\n";
    std::cout << "store=" << state.store << "\n";
    std::cout << "telemetry=" << state.telemetry << "\n";
    std::cout << "status=" << state.status << "\n";
}

static bool healthy_session(const SessionState& state) {
    return pid_alive(state.pid) &&
           is_socket_path(state.socket) &&
           mountpoint_active(state.mount) &&
           socket_responds(state.socket);
}

static std::string current_timestamp_utc() {
    std::time_t now = std::time(nullptr);
    std::tm tm {};
    gmtime_r(&now, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

static bool parse_common_args(int argc,
                              char** argv,
                              int start,
                              const std::string& cmd,
                              ParsedCommon& out) {
    std::vector<std::string> positional;
    for (int i = start; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--root") {
            if (++i >= argc) {
                std::cerr << "agentvfs: --root requires <dir>\n";
                return false;
            }
            out.root = argv[i];
        } else if (arg == "--telemetry" && cmd == "start") {
            if (++i >= argc) {
                std::cerr << "agentvfs: --telemetry requires auto|none|<csv>\n";
                return false;
            }
            out.telemetry = argv[i];
        } else if (arg == "--checkpoint" && cmd == "stop") {
            out.checkpoint_on_stop = true;
        } else if (arg == "--no-checkpoint" && cmd == "stop") {
            out.checkpoint_on_stop = false;
        } else if (arg == "--from" && cmd == "init") {
            if (++i >= argc) {
                std::cerr << "agentvfs: --from requires <dir>\n";
                return false;
            }
            out.from_dir = argv[i];
        } else if (arg == "--mount" && (cmd == "init" || cmd == "start")) {
            if (++i >= argc) {
                std::cerr << "agentvfs: --mount requires <dir>\n";
                return false;
            }
            out.mount_override = argv[i];
        } else if (arg == "--allow-root" && (cmd == "init" || cmd == "start")) {
            out.allow_root = true;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "agentvfs: unknown option for workspace " << cmd << ": " << arg << "\n";
            return false;
        } else {
            positional.push_back(arg);
        }
    }

    if (cmd == "start" || cmd == "status" || cmd == "stop") {
        if (positional.size() > 1) {
            std::cerr << "agentvfs: workspace " << cmd << " takes at most one name\n";
            return false;
        }
        if (positional.size() == 1) out.name = positional[0];
    } else if (cmd == "checkpoint") {
        if (positional.size() > 2) {
            std::cerr << "agentvfs: workspace checkpoint takes [name] [label]\n";
            return false;
        }
        if (positional.size() >= 1) out.name = positional[0];
        if (positional.size() == 2) out.label = positional[1];
    } else if (cmd == "rollback") {
        if (positional.empty() || positional.size() > 2) {
            std::cerr << "agentvfs: workspace rollback takes [name] <label-or-hash>\n";
            return false;
        }
        if (positional.size() == 1) {
            out.target = positional[0];
        } else {
            out.name = positional[0];
            out.target = positional[1];
        }
    } else if (cmd == "init") {
        if (positional.size() != 1) {
            std::cerr << "agentvfs: workspace init takes exactly one name\n";
            return false;
        }
        out.name = positional[0];
        if (out.from_dir.empty()) {
            std::cerr << "agentvfs: workspace init requires --from <dir>\n";
            return false;
        }
    }

    if (!is_valid_workspace_name(out.name)) {
        std::cerr << "agentvfs: invalid workspace name: " << out.name << "\n";
        return false;
    }
    return true;
}

struct StartLock {
    int fd = -1;
    StartLock() = default;
    StartLock(const StartLock&) = delete;
    StartLock& operator=(const StartLock&) = delete;
    ~StartLock() {
        if (fd >= 0) {
            flock(fd, LOCK_UN);
            close(fd);
        }
    }
};

static bool acquire_start_lock(const WorkspacePaths& paths,
                               StartLock& lock,
                               std::string& error) {
    std::string lock_path = paths.root + "/start.lock";
    int fd = open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) {
        error = "open " + lock_path + ": " + std::strerror(errno);
        return false;
    }
    if (flock(fd, LOCK_EX) != 0) {
        error = "flock " + lock_path + ": " + std::strerror(errno);
        close(fd);
        return false;
    }
    lock.fd = fd;
    return true;
}

static pid_t spawn_daemon(const std::string& self_path,
                          const WorkspacePaths& paths,
                          const std::string& telemetry,
                          bool allow_root,
                          std::string& error) {
    (void)telemetry;
    pid_t pid = fork();
    if (pid < 0) {
        error = "fork: " + std::string(std::strerror(errno));
        return -1;
    }
    if (pid == 0) {
        int log_fd = open(paths.log.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
            close(log_fd);
        }

        std::vector<std::string> args = {
            self_path,
            "--source", paths.source,
            "--mountpoint", paths.mount,
            "--store", paths.store,
            "--control-sock", paths.socket,
            "-f"
        };
#ifndef __APPLE__
        args.push_back("--telemetry=" + telemetry);
#endif
        if (allow_root) {
            args.push_back("-o");
            args.push_back("allow_root");
        }
        std::vector<char*> cargs;
        for (auto& a : args) cargs.push_back(const_cast<char*>(a.c_str()));
        cargs.push_back(nullptr);
        execv(self_path.c_str(), cargs.data());
        _exit(127);
    }
    error.clear();
    return pid;
}

static bool unmount_workspace(const std::string& mount_path) {
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
#ifdef __APPLE__
        execlp("umount", "umount", "-f", mount_path.c_str(), (char*)nullptr);
#else
        execlp("fusermount3", "fusermount3", "-u", mount_path.c_str(), (char*)nullptr);
#endif
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void wait_daemon_exit(pid_t pid) {
    if (pid <= 0) return;
    for (int i = 0; i < 50; i++) {
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid || (r < 0 && errno == ECHILD)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (kill(pid, 0) == 0 || errno == EPERM) {
        kill(pid, SIGKILL);
        for (int i = 0; i < 20; i++) {
            int status = 0;
            pid_t r = waitpid(pid, &status, WNOHANG);
            if (r == pid || (r < 0 && errno == ECHILD)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

static bool response_ok(const std::string& json) {
    size_t after_colon = 0;
    if (!json_find_key(json, "ok", after_colon)) return false;
    while (after_colon < json.size() &&
           (json[after_colon] == ' ' || json[after_colon] == '\t')) after_colon++;
    return after_colon + 4 <= json.size() &&
           json.compare(after_colon, 4, "true") == 0;
}

static std::string response_string_field(const std::string& json,
                                         const std::string& key) {
    std::string value;
    if (json_get_string(json, key, value)) return value;
    return {};
}

#if defined(AGENTVFS_EBPF) || defined(AGENTVFS_FANOTIFY)
static bool has_capability_bit(int bit) {
    std::ifstream in("/proc/self/status");
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("CapEff:", 0) != 0) continue;
        std::string hex = line.substr(7);
        unsigned long long value = std::strtoull(hex.c_str(), nullptr, 16);
        return (value & (1ULL << bit)) != 0;
    }
    return false;
}
#endif

TelemetryAvailability detect_telemetry_availability() {
    TelemetryAvailability availability;
#ifdef AGENTVFS_EBPF
    availability.ebpf_compiled = true;
    availability.ebpf_runtime_usable =
        (geteuid() == 0 || has_capability_bit(39)) &&
        access("/sys/kernel/btf/vmlinux", R_OK) == 0;
#endif
#ifdef AGENTVFS_FANOTIFY
    availability.fanotify_compiled = true;
    availability.fanotify_runtime_usable =
        geteuid() == 0 || has_capability_bit(21);
#endif
    return availability;
}

std::string select_auto_telemetry(const TelemetryAvailability& availability) {
    if (availability.ebpf_compiled && availability.ebpf_runtime_usable) return "ebpf";
    if (availability.fanotify_compiled && availability.fanotify_runtime_usable) return "fanotify";
    return "none";
}

static bool load_healthy_session(const ParsedCommon& opts, SessionState& state) {
    WorkspacePaths paths = make_workspace_paths(opts.root, opts.name);
    std::string error;
    if (!read_session_file(paths.session_json, state, error)) {
        std::cerr << "agentvfs: no session for workspace " << opts.name << "\n";
        return false;
    }
    if (!healthy_session(state)) {
        std::cerr << "agentvfs: workspace " << opts.name << " is not running\n";
        return false;
    }
    return true;
}

static int command_checkpoint(const ParsedCommon& opts) {
    SessionState state;
    if (!load_healthy_session(opts, state)) return 1;
    std::string line = "checkpoint";
    if (!opts.label.empty()) line += " " + opts.label;

    std::string error;
    std::string resp = send_control_line(state.socket, line, error);
    if (resp.empty() || !response_ok(resp)) {
        std::string msg = response_string_field(resp, "error");
        std::cerr << "agentvfs: checkpoint failed: "
                  << (msg.empty() ? error : msg) << "\n";
        return 1;
    }
    std::string commit = response_string_field(resp, "commit");
    std::cout << commit << "\n";
    return commit.empty() ? 1 : 0;
}

static int command_rollback(const ParsedCommon& opts) {
    if (opts.target.empty()) {
        std::cerr << "agentvfs: workspace rollback requires <label-or-hash>\n";
        return 2;
    }
    SessionState state;
    if (!load_healthy_session(opts, state)) return 1;

    std::string error;
    std::string resp = send_control_line(state.socket, "rollback " + opts.target, error);
    if (resp.empty() || !response_ok(resp)) {
        std::string msg = response_string_field(resp, "error");
        std::cerr << "agentvfs: rollback failed: "
                  << (msg.empty() ? error : msg) << "\n";
        return 1;
    }
    std::string commit = response_string_field(resp, "rolled_back_to");
    std::cout << commit << "\n";
    return commit.empty() ? 1 : 0;
}

static int command_stop(const ParsedCommon& opts) {
    WorkspacePaths paths = make_workspace_paths(opts.root, opts.name);
    SessionState state;
    std::string error;
    if (!read_session_file(paths.session_json, state, error)) {
        std::cout << "name=" << opts.name << "\n";
        std::cout << "mount=" << paths.mount << "\n";
        std::cout << "status=stopped\n";
        return 0;
    }

    std::string final_checkpoint;
    bool active_mount = mountpoint_active(state.mount);
    if (active_mount && opts.checkpoint_on_stop) {
        if (!healthy_session(state)) {
            std::cerr << "agentvfs: cannot checkpoint unhealthy workspace "
                      << state.name << "\n";
            std::cerr << "agentvfs: leaving mount running at " << state.mount << "\n";
            return 1;
        }
        std::string resp = send_control_line(state.socket, "checkpoint shutdown", error);
        if (resp.empty() || !response_ok(resp)) {
            std::string msg = response_string_field(resp, "error");
            std::cerr << "agentvfs: stop checkpoint failed: "
                      << (msg.empty() ? error : msg) << "\n";
            std::cerr << "agentvfs: leaving mount running at " << state.mount << "\n";
            return 1;
        }
        final_checkpoint = response_string_field(resp, "commit");
    }

    if (active_mount && !unmount_workspace(state.mount)) {
        std::cerr << "agentvfs: failed to unmount " << state.mount << "\n";
        return 1;
    }
    wait_daemon_exit((pid_t)state.pid);

    state.status = "stopped";
    state.pid = -1;
    if (!write_session_file(paths.session_json, state, error)) {
        std::cerr << "agentvfs: warning: failed to update session.json: " << error << "\n";
    }
    std::cout << "name=" << state.name << "\n";
    std::cout << "mount=" << state.mount << "\n";
    if (!final_checkpoint.empty()) std::cout << "checkpoint=" << final_checkpoint << "\n";
    std::cout << "status=stopped\n";
    return 0;
}

static int command_init(const ParsedCommon& opts) {
    WorkspacePaths paths = make_workspace_paths(opts.root, opts.name);

    struct stat st {};
    if (stat(opts.from_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        std::cerr << "agentvfs: --from " << opts.from_dir
                  << " is not a directory\n";
        return 1;
    }
    if (access(paths.session_json.c_str(), F_OK) == 0) {
        std::cerr << "agentvfs: workspace " << opts.name
                  << " already exists; stop it first or pick another name\n";
        return 1;
    }

    std::string error;
    if (!ensure_dir(opts.root, error) || !ensure_session_dirs(paths, error)) {
        std::cerr << "agentvfs: " << error << "\n";
        return 1;
    }

    auto shell_quote = [](const std::string& s) {
        std::string out = "'";
        for (char c : s) {
            if (c == '\'') out += "'\\''";
            else out.push_back(c);
        }
        out += "'";
        return out;
    };
    std::string cmd;
#ifdef __APPLE__
    cmd = "cp -a ";
#else
    cmd = "cp -a --reflink=auto -- ";
#endif
    cmd += shell_quote(opts.from_dir + "/.");
    cmd += " ";
    cmd += shell_quote(paths.source);
    int rc = system(cmd.c_str());
    if (rc != 0) {
        std::cerr << "agentvfs: copy from " << opts.from_dir
                  << " into " << paths.source
                  << " failed (cp exit " << rc << ")\n";
        return 1;
    }

    {
        WorkspaceConfig config;
        if (!opts.mount_override.empty()) {
            if (!make_absolute(opts.mount_override, config.mount_override, error)) {
                std::cerr << "agentvfs: cannot resolve --mount " << opts.mount_override
                          << ": " << error << "\n";
                return 1;
            }
        }
        config.allow_root = opts.allow_root;
        if (!opts.mount_override.empty() || opts.allow_root) {
            std::string config_path = paths.root + "/workspace.json";
            if (!write_workspace_config_file(config_path, config, error)) {
                std::cerr << "agentvfs: " << error << "\n";
                return 1;
            }
        }
    }

    std::cout << "name=" << opts.name << "\n";
    std::cout << "source=" << paths.source << "\n";
    std::cout << "seeded_from=" << opts.from_dir << "\n";
    std::cout << "status=initialized\n";
    return 0;
}

static int command_status(const ParsedCommon& opts) {
    WorkspacePaths paths = make_workspace_paths(opts.root, opts.name);
    SessionState state;
    std::string error;
    if (!read_session_file(paths.session_json, state, error)) {
        state.name = opts.name;
        state.root = paths.root;
        state.source = paths.source;
        state.mount = paths.mount;
        state.store = paths.store;
        state.socket = paths.socket;
        state.log = paths.log;
        state.telemetry = "unknown";
        state.status = "stopped";
        print_session(state);
        return 1;
    }
    state.status = healthy_session(state) ? "started" : "stale";
    print_session(state);
    return state.status == "started" ? 0 : 1;
}

static int command_list(const ParsedCommon& opts) {
    DIR* dir = opendir(opts.root.c_str());
    if (!dir) {
        if (errno == ENOENT) return 0;
        std::cerr << "agentvfs: cannot open " << opts.root
                  << ": " << std::strerror(errno) << "\n";
        return 1;
    }

    std::vector<std::string> names;
    while (dirent* entry = readdir(dir)) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        std::string session_json = opts.root + "/" + name + "/session.json";
        if (access(session_json.c_str(), F_OK) != 0) continue;
        names.push_back(std::move(name));
    }
    closedir(dir);
    std::sort(names.begin(), names.end());

    for (const std::string& name : names) {
        WorkspacePaths paths = make_workspace_paths(opts.root, name);
        SessionState state;
        std::string error;
        if (!read_session_file(paths.session_json, state, error)) continue;
        std::string status = healthy_session(state) ? "started"
                            : (mountpoint_active(paths.mount) ? "stale" : "stopped");
        std::cout << name << "\t" << status << "\t" << state.mount << "\n";
    }
    return 0;
}

static int command_start(const ParsedCommon& opts, const std::string& self_path) {
    WorkspacePaths paths = make_workspace_paths(opts.root, opts.name);
    std::string error;

    if (!socket_path_fits(paths.socket)) {
        std::cerr << "agentvfs: workspace socket path "
                  << paths.socket << " is " << paths.socket.size()
                  << " bytes; AF_UNIX limit is " << kMaxSunPath
                  << ". Use a shorter workspace name or --root.\n";
        return 1;
    }

    if (!path_owned_by_current_user_or_absent(paths.root, error)) {
        std::cerr << "agentvfs: " << error << "\n";
        return 1;
    }

    auto warn_if_override_ignored = [&opts](const SessionState& existing) {
        if (opts.mount_override.empty()) return;
        std::string abs_override;
        std::string ignored;
        if (!make_absolute(opts.mount_override, abs_override, ignored)) return;
        if (abs_override == existing.mount) return;
        std::cerr << "agentvfs: warning: workspace " << existing.name
                  << " is already running at " << existing.mount
                  << "; --mount " << opts.mount_override
                  << " ignored. Stop the workspace first to change it.\n";
    };

    SessionState existing;
    if (read_session_file(paths.session_json, existing, error) && healthy_session(existing)) {
        warn_if_override_ignored(existing);
        existing.status = "started";
        print_session(existing);
        return 0;
    }

    if (!ensure_dir(opts.root, error) || !ensure_session_dirs(paths, error)) {
        std::cerr << "agentvfs: " << error << "\n";
        return 1;
    }

    StartLock lock;
    if (!acquire_start_lock(paths, lock, error)) {
        std::cerr << "agentvfs: " << error << "\n";
        return 1;
    }

    // Re-check under lock: another start may have just finished.
    SessionState relocked;
    if (read_session_file(paths.session_json, relocked, error) && healthy_session(relocked)) {
        warn_if_override_ignored(relocked);
        relocked.status = "started";
        print_session(relocked);
        return 0;
    }

    // Resolve mount path: CLI override > persisted workspace.json > default.
    std::string default_mount = paths.mount;
    std::string cli_override;
    if (!opts.mount_override.empty()) {
        if (!make_absolute(opts.mount_override, cli_override, error)) {
            std::cerr << "agentvfs: cannot resolve --mount " << opts.mount_override
                      << ": " << error << "\n";
            return 1;
        }
    }
    std::string config_path = paths.root + "/workspace.json";
    WorkspaceConfig config;
    if (!read_workspace_config_file(config_path, config, error)) {
        std::cerr << "agentvfs: " << error << "\n";
        return 1;
    }
    paths.mount = resolve_mount_path(cli_override, config.mount_override, default_mount);
    if (opts.allow_root) config.allow_root = true;

    // Persist CLI overrides so subsequent starts inherit them.
    if (!cli_override.empty() && cli_override != config.mount_override) {
        WorkspaceConfig new_config = config;
        new_config.mount_override = cli_override;
        new_config.allow_root = config.allow_root || opts.allow_root;
        if (!write_workspace_config_file(config_path, new_config, error)) {
            std::cerr << "agentvfs: " << error << "\n";
            return 1;
        }
    } else if (opts.allow_root && !config.allow_root) {
        WorkspaceConfig new_config = config;
        new_config.allow_root = true;
        if (!write_workspace_config_file(config_path, new_config, error)) {
            std::cerr << "agentvfs: " << error << "\n";
            return 1;
        }
    }

    // Materialize the chosen mount dir (default or override).
    if (paths.mount == default_mount) {
        if (!ensure_dir(paths.mount, error)) {
            std::cerr << "agentvfs: " << error << "\n";
            return 1;
        }
    } else {
        if (!prepare_mount_override(paths.mount, error)) {
            std::cerr << "agentvfs: " << error << "\n";
            return 1;
        }
    }

    if (mountpoint_active(paths.mount)) {
        std::cerr << "agentvfs: " << paths.mount
                  << " is already mounted but not owned by a healthy recorded session\n";
        return 1;
    }
    unlink(paths.socket.c_str());
    std::string telemetry = opts.telemetry;
    bool telemetry_warning = false;
    if (telemetry == "auto") {
        telemetry = select_auto_telemetry(detect_telemetry_availability());
        telemetry_warning = telemetry == "none";
    }
    pid_t pid = spawn_daemon(self_path, paths, telemetry, config.allow_root, error);
    if (pid <= 0) {
        std::cerr << "agentvfs: " << error << "\n";
        return 1;
    }

    if (!wait_ready(paths, pid)) {
        if (kill(pid, 0) != 0 && errno == ESRCH) {
            std::cerr << "agentvfs: daemon exited before becoming ready; log=" << paths.log << "\n";
        } else {
            std::cerr << "agentvfs: daemon did not become ready within 10s; log=" << paths.log << "\n";
            kill(pid, SIGTERM);
        }
        wait_daemon_exit(pid);
        return 1;
    }

    SessionState state;
    state.name = opts.name;
    state.pid = pid;
    state.root = paths.root;
    state.source = paths.source;
    state.mount = paths.mount;
    state.store = paths.store;
    state.socket = paths.socket;
    state.log = paths.log;
    state.telemetry = telemetry;
    state.status = "started";
    state.created_at = current_timestamp_utc();
    if (!write_session_file(paths.session_json, state, error)) {
        std::cerr << "agentvfs: " << error << "\n";
        if (mountpoint_active(paths.mount)) unmount_workspace(paths.mount);
        kill(pid, SIGTERM);
        wait_daemon_exit(pid);
        return 1;
    }
    if (telemetry_warning) {
        std::cout << "warning=no usable telemetry backend found; mounted without telemetry\n";
    }
    print_session(state);
    return 0;
}

int run_workspace_cli(int argc, char** argv, const std::string& self_path) {
    if (argc < 2) return workspace_usage();
    std::string cmd = argv[1];

    if (cmd == "list") {
        ParsedCommon opts;
        for (int i = 2; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--root") {
                if (++i >= argc) {
                    std::cerr << "agentvfs: --root requires <dir>\n";
                    return 2;
                }
                opts.root = argv[i];
            } else {
                std::cerr << "agentvfs: unknown option for workspace list: " << arg << "\n";
                return 2;
            }
        }
        return command_list(opts);
    }

    if (!(cmd == "start" || cmd == "init" || cmd == "status" || cmd == "list" ||
          cmd == "checkpoint" || cmd == "rollback" || cmd == "stop")) {
        std::cerr << "agentvfs: workspace requires start|init|status|list|checkpoint|rollback|stop\n";
        return 2;
    }

    ParsedCommon opts;
    if (!parse_common_args(argc, argv, 2, cmd, opts)) return 2;

    if (cmd == "start") return command_start(opts, self_path);
    if (cmd == "init") return command_init(opts);
    if (cmd == "status") return command_status(opts);
    if (cmd == "checkpoint") return command_checkpoint(opts);
    if (cmd == "rollback") return command_rollback(opts);
    if (cmd == "stop") return command_stop(opts);
    return workspace_usage();
}

} // namespace workspace
} // namespace cas
