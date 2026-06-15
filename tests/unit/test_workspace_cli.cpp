#include "workspace_cli.h"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static void test_workspace_name_validation() {
    using cas::workspace::is_valid_workspace_name;

    assert(is_valid_workspace_name("default"));
    assert(is_valid_workspace_name("task-1"));
    assert(is_valid_workspace_name("task_1"));
    assert(is_valid_workspace_name("task.1"));

    assert(!is_valid_workspace_name(""));
    assert(!is_valid_workspace_name("."));
    assert(!is_valid_workspace_name(".."));
    assert(!is_valid_workspace_name("../repo"));
    assert(!is_valid_workspace_name("repo/name"));
    assert(!is_valid_workspace_name("repo name"));
    assert(!is_valid_workspace_name("repo:name"));
}

static void test_default_root_prefers_xdg_runtime_dir() {
    setenv("XDG_RUNTIME_DIR", "/tmp/agentvfs-xdg-test", 1);
    std::string root = cas::workspace::default_workspace_root();
    assert(root == "/tmp/agentvfs-xdg-test/agentvfs");
}

static void test_default_root_falls_back_to_tmp() {
    unsetenv("XDG_RUNTIME_DIR");
    std::string root = cas::workspace::default_workspace_root();
    std::string expected = "/tmp/agentvfs-" + std::to_string((long long)getuid());
    assert(root == expected);
}

static void test_make_workspace_paths() {
    auto paths = cas::workspace::make_workspace_paths("/tmp/agentvfs-root", "demo");
    assert(paths.name == "demo");
    assert(paths.root == "/tmp/agentvfs-root/demo");
    assert(paths.source == "/tmp/agentvfs-root/demo/source");
    assert(paths.mount == "/tmp/agentvfs-root/demo/mount");
    assert(paths.store == "/tmp/agentvfs-root/demo/store");
    assert(paths.socket == "/tmp/agentvfs-root/demo/control.sock");
    assert(paths.log == "/tmp/agentvfs-root/demo/daemon.log");
    assert(paths.session_json == "/tmp/agentvfs-root/demo/session.json");
}

static void test_parse_key_value_output() {
    std::map<std::string, std::string> kv =
        cas::workspace::parse_key_value_lines("name=default\nmount=/tmp/m\nwarning=a=b\n");
    assert(kv["name"] == "default");
    assert(kv["mount"] == "/tmp/m");
    assert(kv["warning"] == "a=b");
}

static void test_session_json_roundtrip() {
    cas::workspace::SessionState state;
    state.name = "demo";
    state.pid = 1234;
    state.root = "/tmp/root/demo";
    state.source = "/tmp/root/demo/source";
    state.mount = "/tmp/root/demo/mount";
    state.store = "/tmp/root/demo/store";
    state.socket = "/tmp/root/demo/control.sock";
    state.log = "/tmp/root/demo/daemon.log";
    state.telemetry = "ebpf";
    state.status = "started";
    state.created_at = "2026-04-29T12:34:56Z";

    std::string json = cas::workspace::session_to_json(state);
    cas::workspace::SessionState parsed;
    std::string error;
    assert(cas::workspace::parse_session_json(json, parsed, error));
    assert(parsed.name == state.name);
    assert(parsed.pid == state.pid);
    assert(parsed.root == state.root);
    assert(parsed.source == state.source);
    assert(parsed.mount == state.mount);
    assert(parsed.store == state.store);
    assert(parsed.socket == state.socket);
    assert(parsed.log == state.log);
    assert(parsed.telemetry == state.telemetry);
    assert(parsed.status == state.status);
    assert(parsed.created_at == state.created_at);
}

static void test_session_json_rejects_missing_name() {
    cas::workspace::SessionState parsed;
    std::string error;
    assert(!cas::workspace::parse_session_json("{\"pid\":123}", parsed, error));
    assert(!error.empty());
}

static void test_extract_json_string_handles_escapes() {
    cas::workspace::SessionState parsed;
    std::string error;
    std::string json =
        "{\"name\":\"de\\\\mo\",\"pid\":1,\"root\":\"/r\",\"source\":\"/s\","
        "\"mount\":\"/m\",\"store\":\"/st\",\"socket\":\"/sock\","
        "\"log\":\"/log\",\"telemetry\":\"none\",\"status\":\"started\","
        "\"created_at\":\"2026-04-29T12:34:56Z\"}";
    assert(cas::workspace::parse_session_json(json, parsed, error));
    assert(parsed.name == "de\\mo");
}

static void test_session_json_distinguishes_key_from_value_substring() {
    // Value of "log" contains the literal text "socket" — must not collide
    // with the lookup of the "socket" key.
    cas::workspace::SessionState parsed;
    std::string error;
    std::string json =
        "{\"name\":\"demo\",\"pid\":7,"
        "\"root\":\"/r\",\"source\":\"/s\",\"mount\":\"/m\",\"store\":\"/st\","
        "\"socket\":\"/real/sock\","
        "\"log\":\"socket\","
        "\"telemetry\":\"none\",\"status\":\"started\","
        "\"created_at\":\"2026-04-30T00:00:00Z\"}";
    assert(cas::workspace::parse_session_json(json, parsed, error));
    assert(parsed.socket == "/real/sock");
    assert(parsed.log == "socket");
}

static void test_socket_path_fits() {
    // ~50-char path: fits.
    assert(cas::workspace::socket_path_fits(
        "/run/user/1000/agentvfs/default/control.sock"));
    // 107 chars exactly: fits.
    std::string p107(107, 'a');
    assert(cas::workspace::socket_path_fits(p107));
    // 108 chars: too long.
    std::string p108(108, 'a');
    assert(!cas::workspace::socket_path_fits(p108));
}

static void test_workspace_config_json_roundtrip() {
    cas::workspace::WorkspaceConfig config;
    config.mount_override = "/tmp/custom-mount";

    std::string json = cas::workspace::workspace_config_to_json(config);
    cas::workspace::WorkspaceConfig parsed;
    std::string error;
    assert(cas::workspace::parse_workspace_config_json(json, parsed, error));
    assert(parsed.mount_override == "/tmp/custom-mount");
    assert(error.empty());
}

static void test_workspace_config_json_handles_empty_override() {
    cas::workspace::WorkspaceConfig config; // mount_override default-empty

    std::string json = cas::workspace::workspace_config_to_json(config);
    cas::workspace::WorkspaceConfig parsed;
    parsed.mount_override = "leftover";
    std::string error;
    assert(cas::workspace::parse_workspace_config_json(json, parsed, error));
    assert(parsed.mount_override.empty());
}

static void test_workspace_config_file_roundtrip() {
    char tmpdir[] = "/tmp/agentvfs-cfg-XXXXXX";
    assert(mkdtemp(tmpdir) != nullptr);
    std::string path = std::string(tmpdir) + "/workspace.json";

    cas::workspace::WorkspaceConfig out;
    out.mount_override = "/some/abs/path";
    std::string error;
    assert(cas::workspace::write_workspace_config_file(path, out, error));

    cas::workspace::WorkspaceConfig in;
    assert(cas::workspace::read_workspace_config_file(path, in, error));
    assert(in.mount_override == "/some/abs/path");

    unlink(path.c_str());
    rmdir(tmpdir);
}

static void test_workspace_config_missing_file_is_empty() {
    cas::workspace::WorkspaceConfig config;
    config.mount_override = "leftover";
    std::string error = "leftover";
    assert(cas::workspace::read_workspace_config_file(
        "/tmp/agentvfs-no-such-dir-xyz/workspace.json", config, error));
    assert(config.mount_override.empty());
    assert(error.empty());
}

static void test_workspace_config_rejects_malformed_json() {
    cas::workspace::WorkspaceConfig parsed;
    std::string error;
    // Garbage input must not silently parse as an empty config.
    assert(!cas::workspace::parse_workspace_config_json("not-json", parsed, error));
    assert(!error.empty());

    // Truncated body.
    error.clear();
    assert(!cas::workspace::parse_workspace_config_json("{\"mount_override\":\"/x", parsed, error));
    assert(!error.empty());
}

static void test_workspace_config_ignores_unknown_keys() {
    cas::workspace::WorkspaceConfig parsed;
    std::string error;
    std::string json =
        "{\"mount_override\":\"/x\",\"future_field\":\"y\",\"other\":42}";
    assert(cas::workspace::parse_workspace_config_json(json, parsed, error));
    assert(parsed.mount_override == "/x");
    assert(error.empty());
}

static void test_lexical_normalize_absolute() {
    using cas::workspace::lexical_normalize_absolute;

    // The case from the user's bug report: relative `./X` joined with cwd.
    assert(lexical_normalize_absolute("/home/user/./agentvfs-workspace")
           == "/home/user/agentvfs-workspace");
    // Trailing `/.`
    assert(lexical_normalize_absolute("/a/b/.") == "/a/b");
    assert(lexical_normalize_absolute("/.") == "/");
    // `..` collapses
    assert(lexical_normalize_absolute("/a/b/../c") == "/a/c");
    assert(lexical_normalize_absolute("/a/..") == "/");
    // Repeated slashes
    assert(lexical_normalize_absolute("/a//b") == "/a/b");
    assert(lexical_normalize_absolute("//") == "/");
    // Already-normal paths unchanged
    assert(lexical_normalize_absolute("/foo/bar") == "/foo/bar");
    assert(lexical_normalize_absolute("/") == "/");
    // Non-absolute input passed through (caller's responsibility)
    assert(lexical_normalize_absolute("relative/path") == "relative/path");
    assert(lexical_normalize_absolute("") == "");
}

static void test_resolve_mount_path_precedence() {
    // CLI override beats config and default.
    assert(cas::workspace::resolve_mount_path("/cli", "/cfg", "/def") == "/cli");
    // Config used when no CLI.
    assert(cas::workspace::resolve_mount_path("", "/cfg", "/def") == "/cfg");
    // Default when neither.
    assert(cas::workspace::resolve_mount_path("", "", "/def") == "/def");
    // CLI wins even when config also set.
    assert(cas::workspace::resolve_mount_path("/cli", "", "/def") == "/cli");
}

static void test_select_auto_telemetry() {
    cas::workspace::TelemetryAvailability availability;

    availability.ebpf_compiled = true;
    availability.ebpf_runtime_usable = true;
    availability.fanotify_compiled = true;
    availability.fanotify_runtime_usable = true;
    assert(cas::workspace::select_auto_telemetry(availability) == "ebpf");

    availability.ebpf_runtime_usable = false;
    assert(cas::workspace::select_auto_telemetry(availability) == "fanotify");

    availability.fanotify_runtime_usable = false;
    assert(cas::workspace::select_auto_telemetry(availability) == "none");
}

int main() {
    test_workspace_name_validation();
    test_default_root_prefers_xdg_runtime_dir();
    test_default_root_falls_back_to_tmp();
    test_make_workspace_paths();
    test_parse_key_value_output();
    test_session_json_roundtrip();
    test_session_json_rejects_missing_name();
    test_extract_json_string_handles_escapes();
    test_session_json_distinguishes_key_from_value_substring();
    test_socket_path_fits();
    test_select_auto_telemetry();
    test_workspace_config_json_roundtrip();
    test_workspace_config_json_handles_empty_override();
    test_workspace_config_file_roundtrip();
    test_workspace_config_missing_file_is_empty();
    test_workspace_config_rejects_malformed_json();
    test_workspace_config_ignores_unknown_keys();
    test_lexical_normalize_absolute();
    test_resolve_mount_path_precedence();
    std::cout << "PASS test_workspace_cli\n";
    return 0;
}
