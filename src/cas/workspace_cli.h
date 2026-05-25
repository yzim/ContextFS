#pragma once

#include <map>
#include <string>

namespace cas {
namespace workspace {

struct WorkspacePaths {
    std::string name;
    std::string root;
    std::string source;
    std::string mount;
    std::string store;
    std::string socket;
    std::string log;
    std::string session_json;
};

struct SessionState {
    std::string name;
    long pid = -1;
    std::string root;
    std::string source;
    std::string mount;
    std::string store;
    std::string socket;
    std::string log;
    std::string telemetry;
    std::string status;
    std::string created_at;
};

// Per-workspace persistent config (separate from per-run SessionState).
// Lives at <root>/<name>/workspace.json. Optional file: missing means defaults.
struct WorkspaceConfig {
    std::string mount_override;
    bool allow_root = false;
};

bool is_valid_workspace_name(const std::string& name);
std::string default_workspace_root();
WorkspacePaths make_workspace_paths(const std::string& workspace_root,
                                    const std::string& name);
std::map<std::string, std::string> parse_key_value_lines(const std::string& text);

std::string session_to_json(const SessionState& state);
bool parse_session_json(const std::string& json,
                        SessionState& state,
                        std::string& error);
bool read_session_file(const std::string& path,
                       SessionState& state,
                       std::string& error);
bool write_session_file(const std::string& path,
                        const SessionState& state,
                        std::string& error);

std::string workspace_config_to_json(const WorkspaceConfig& config);
bool parse_workspace_config_json(const std::string& json,
                                 WorkspaceConfig& config,
                                 std::string& error);
// Reads <root>/<name>/workspace.json. Missing file -> empty config + true.
// Returns false only on real I/O or parse errors.
bool read_workspace_config_file(const std::string& path,
                                WorkspaceConfig& config,
                                std::string& error);
bool write_workspace_config_file(const std::string& path,
                                 const WorkspaceConfig& config,
                                 std::string& error);

// Picks the mount path for `start`: CLI override beats persisted config beats
// the default. Returns whichever is the first non-empty input.
std::string resolve_mount_path(const std::string& cli_override,
                               const std::string& config_override,
                               const std::string& default_path);

// Collapse "/./", "//", and "/foo/.." segments in an absolute path. Pure
// textual: does not touch the filesystem. Returns input unchanged if it is
// not absolute.
std::string lexical_normalize_absolute(const std::string& path);
bool socket_responds(const std::string& socket_path);
std::string send_control_line(const std::string& socket_path,
                              const std::string& line,
                              std::string& error);

// True if the AF_UNIX socket path computed for this workspace fits in sun_path.
bool socket_path_fits(const std::string& socket_path);

struct TelemetryAvailability {
    bool ebpf_compiled = false;
    bool ebpf_runtime_usable = false;
    bool fanotify_compiled = false;
    bool fanotify_runtime_usable = false;
};

TelemetryAvailability detect_telemetry_availability();
std::string select_auto_telemetry(const TelemetryAvailability& availability);

int run_workspace_cli(int argc, char** argv, const std::string& self_path);

} // namespace workspace
} // namespace cas
