#include "bootstrap.h"
#ifdef AGENTVFS_BPFTIME
#include "backends/bpftime_backend.h"
#endif
#ifdef AGENTVFS_EBPF
#include "backends/ebpf_backend.h"
#include "platform/linux/ebpf_policy_installer.h"
#endif
#ifdef AGENTVFS_FANOTIFY
#include "backends/fanotify_backend.h"
#endif
#ifdef AGENTVFS_LDPRELOAD
#include "backends/ldpreload_backend.h"
#endif
#ifdef AGENTVFS_LUA
#include "backends/lua_backend.h"
#endif
#ifdef AGENTVFS_PTRACE
#include "backends/ptrace_backend.h"
#endif
#ifdef AGENTVFS_WASM
#include "backends/wasm_backend.h"
#endif
#include "control_protocol.h"
#include "control_socket.h"
#include "daemon.h"
#include "platform.h"
#include "telemetry_drain.h"
#include "telemetry_registry.h"
#include "workspace_cli.h"
#include <cstdio>
#include <memory>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <unistd.h>
#include <vector>

struct CasArgs {
    std::string source;
    std::string mountpoint;
    std::string store;
    std::string control_sock;
    bool foreground = false;
    bool single_threaded = false;
    bool telemetry_specified = false;
    std::vector<std::string> requested_backends;
    std::string telemetry_lua_script;
    std::string telemetry_wasm_module;
    std::string telemetry_ptrace_pids;
    std::string telemetry_ldpreload_socket;
    std::string telemetry_bpftime_probes;
    std::vector<std::string> fuse_passthrough;
};

static bool starts_with(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

static std::string trim_arg(const std::string& s) {
    const char* ws = " \t\r\n";
    size_t first = s.find_first_not_of(ws);
    if (first == std::string::npos) return {};
    size_t last = s.find_last_not_of(ws);
    return s.substr(first, last - first + 1);
}

static std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t comma = s.find(',', start);
        std::string part = trim_arg(
            s.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        if (!part.empty()) {
            out.push_back(std::move(part));
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

[[maybe_unused]] static bool contains_backend(const std::vector<std::string>& backends,
                                              const std::string& name) {
    for (const auto& backend : backends) {
        if (backend == name) return true;
    }
    return false;
}

#ifndef AGENTVFS_VERSION_STRING
#define AGENTVFS_VERSION_STRING "0.1.2"
#endif

[[maybe_unused]] static void print_backend_name(FILE* stream, const char* name, bool& first) {
    std::fprintf(stream, "%s%s", first ? "" : ", ", name);
    first = false;
}

static void usage(FILE* stream) {
    std::fprintf(stream,
        "agentvfs - checkpointable, branchable FUSE workspace for AI agents.\n"
        "\n"
        "Usage:\n"
        "  agentvfs --source <dir> --mountpoint <dir> [options]\n"
        "  agentvfs workspace <subcommand> [args]   (run without args to list subcommands)\n"
        "\n"
        "Required:\n"
        "  --source <dir>         project directory to mount\n"
        "  --mountpoint <dir>     where to expose the working tree\n"
        "\n"
        "Optional:\n"
        "  --store <dir>          CAS object store (default: <source>/.agentvfs-store)\n"
        "  --control-sock <path>  UNIX socket for agentvfs-ctl (default: /tmp/agentvfs-<pid>.sock)\n"
        "  -f                     run in the foreground (do not daemonize)\n"
        "  -s                     single-threaded FUSE loop\n"
        "  --version              print version and exit\n"
        "  --help, -h             print this help and exit\n"
        "\n"
        "Telemetry:\n"
        "  --telemetry=backend1,backend2,...\n"
        "      Backends: ");
    bool first = true;
#ifdef AGENTVFS_EBPF
    print_backend_name(stream, "ebpf", first);
#endif
#ifdef AGENTVFS_FANOTIFY
    print_backend_name(stream, "fanotify", first);
#endif
#ifdef AGENTVFS_PTRACE
    print_backend_name(stream, "ptrace", first);
#endif
#ifdef AGENTVFS_LDPRELOAD
    print_backend_name(stream, "ldpreload", first);
#endif
#ifdef AGENTVFS_BPFTIME
    print_backend_name(stream, "bpftime", first);
#endif
#ifdef AGENTVFS_WASM
    print_backend_name(stream, "wasm", first);
#endif
#ifdef AGENTVFS_LUA
    print_backend_name(stream, "lua", first);
#endif
    if (first) {
        std::fprintf(stream, "(none — rebuild with -DAGENTVFS_EBPF=ON to add)");
    }
    std::fprintf(stream, "\n");
#ifdef AGENTVFS_LUA
    std::fprintf(stream, "  --telemetry-lua-script=PATH\n");
#endif
#ifdef AGENTVFS_WASM
    std::fprintf(stream, "  --telemetry-wasm-module=PATH\n");
#endif
#ifdef AGENTVFS_PTRACE
    std::fprintf(stream, "  --telemetry-ptrace-pids=PID1,PID2,...\n");
#endif
#ifdef AGENTVFS_LDPRELOAD
    std::fprintf(stream, "  --telemetry-ldpreload-socket=PATH\n");
#endif
#ifdef AGENTVFS_BPFTIME
    std::fprintf(stream, "  --telemetry-bpftime-probes=PATH\n");
#endif
    std::fprintf(stream,
        "\n"
        "Exit codes:\n"
        "  0  success, or --help / --version\n"
        "  1  argument error or missing required flag\n"
        "  2  daemon failed to initialize the store\n"
        "  3  control socket failed to start\n");
}

static bool parse_args(int argc, char** argv, CasArgs& out) {
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto need = [&](int more) -> bool {
            if (i + more >= argc) { usage(stderr); return false; }
            return true;
        };
        if (a == "--version") {
            std::fprintf(stdout, "agentvfs %s\n", AGENTVFS_VERSION_STRING);
            std::exit(0);
        }
        else if (a == "--source" && need(1))       out.source = argv[++i];
        else if (a == "--mountpoint" && need(1))   out.mountpoint = argv[++i];
        else if (a == "--store" && need(1))        out.store = argv[++i];
        else if (a == "--control-sock" && need(1)) out.control_sock = argv[++i];
        else if (starts_with(a, "--telemetry=")) {
            out.telemetry_specified = true;
            std::string spec = trim_arg(a.substr(std::strlen("--telemetry=")));
            if (spec == "none") {
                out.requested_backends.clear();
            } else {
                out.requested_backends = split_csv(spec);
            }
        }
        else if (starts_with(a, "--telemetry-lua-script="))
            out.telemetry_lua_script = a.substr(std::strlen("--telemetry-lua-script="));
        else if (starts_with(a, "--telemetry-wasm-module="))
            out.telemetry_wasm_module = a.substr(std::strlen("--telemetry-wasm-module="));
        else if (starts_with(a, "--telemetry-ptrace-pids="))
            out.telemetry_ptrace_pids = a.substr(std::strlen("--telemetry-ptrace-pids="));
        else if (starts_with(a, "--telemetry-ldpreload-socket="))
            out.telemetry_ldpreload_socket = a.substr(std::strlen("--telemetry-ldpreload-socket="));
        else if (starts_with(a, "--telemetry-bpftime-probes="))
            out.telemetry_bpftime_probes = a.substr(std::strlen("--telemetry-bpftime-probes="));
        else if (a == "-f") out.foreground = true;
        else if (a == "-s") out.single_threaded = true;
        else if (a == "-h" || a == "--help") { usage(stdout); std::exit(0); }
        else out.fuse_passthrough.push_back(a);
    }
    if (out.source.empty() || out.mountpoint.empty()) { usage(stderr); return false; }
    if (out.store.empty()) out.store = out.source + "/.agentvfs-store";
    if (out.control_sock.empty()) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "/tmp/agentvfs-%d.sock", (int)getpid());
        out.control_sock = buf;
    }
    if (!out.telemetry_specified) {
#ifdef AGENTVFS_EBPF
        out.requested_backends.push_back("ebpf");
#endif
    }
    return true;
}

static std::string current_exe_path(char** argv) {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0 && (size_t)n < sizeof(buf) - 1) {
        buf[n] = '\0';
        return buf;
    }
    return (argv && argv[0]) ? std::string(argv[0]) : std::string("agentvfs");
}

static int run_daemon_main(int argc, char** argv) {
    CasArgs ca;
    if (!parse_args(argc, argv, ca)) return 1;

    cas::Daemon daemon(ca.source, ca.mountpoint, ca.store);
    if (!daemon.initialize()) {
        std::fprintf(stderr, "agentvfs: failed to initialize store at %s\n", ca.store.c_str());
        return 2;
    }

    auto bs = std::make_unique<cas::Bootstrap>(
        ca.source, daemon.store(), daemon.working_tree(), daemon.inode_map());
    bs->ensure_path("/");
    bs->start_background();
    daemon.set_bootstrap(std::move(bs));

    auto drain = std::make_unique<cas::TelemetryDrain>(
        daemon, ca.store + "/telemetry");
    if (!drain->start()) {
        std::fprintf(stderr, "agentvfs: failed to start telemetry drain\n");
        drain.reset();
    }

    cas::TelemetryRegistry::DrainCallback drain_cb;
    if (drain) {
        drain_cb = [drain_ptr = drain.get()](cas::TelemetryEvent ev) {
            drain_ptr->write_event(ev);
        };
    }
    auto registry = std::make_unique<cas::TelemetryRegistry>(std::move(drain_cb));

#ifdef AGENTVFS_EBPF
    // EbpfPolicyInstaller holds a reference to the EbpfLoader (which lives
    // inside the EbpfBackend owned by the registry).  Heap-allocated so its
    // lifetime spans the rest of run_daemon_main regardless of scope.
    // Created before the backend is moved into the registry.
    std::unique_ptr<cas::EbpfPolicyInstaller> ebpf_pi;
#endif

    cas::BackendConfig bcfg{};
    bcfg.store_path = ca.store;
    bcfg.mount_path = ca.mountpoint;
    for (const auto& name : ca.requested_backends) {
#ifdef AGENTVFS_EBPF
        if (name == "ebpf") {
            auto ebpf_backend = std::make_unique<cas::EbpfBackend>(daemon);
            ebpf_pi = std::make_unique<cas::EbpfPolicyInstaller>(ebpf_backend->loader());
            registry->add(std::move(ebpf_backend));
            continue;
        }
#endif
#ifdef AGENTVFS_FANOTIFY
        if (name == "fanotify") {
            registry->add(std::make_unique<cas::FanotifyBackend>());
            continue;
        }
#endif
#ifdef AGENTVFS_PTRACE
        if (name == "ptrace") {
            bcfg.params["pids"] = ca.telemetry_ptrace_pids;
            registry->add(std::make_unique<cas::PtraceBackend>());
            continue;
        }
#endif
#ifdef AGENTVFS_LDPRELOAD
        if (name == "ldpreload") {
            bcfg.params["socket"] = ca.telemetry_ldpreload_socket;
            registry->add(std::make_unique<cas::LdpreloadBackend>());
            continue;
        }
#endif
#ifdef AGENTVFS_BPFTIME
        if (name == "bpftime") {
            bcfg.params["probes"] = ca.telemetry_bpftime_probes;
            registry->add(std::make_unique<cas::BpftimeBackend>());
            continue;
        }
#endif
#ifdef AGENTVFS_WASM
        if (name == "wasm") {
            bcfg.params["module"] = ca.telemetry_wasm_module;
            registry->add(std::make_unique<cas::WasmBackend>());
            continue;
        }
#endif
#ifdef AGENTVFS_LUA
        if (name == "lua") {
            bcfg.params["script"] = ca.telemetry_lua_script;
            registry->add(std::make_unique<cas::LuaBackend>());
            continue;
        }
#endif
        std::fprintf(stderr, "agentvfs: unknown or disabled backend: %s\n", name.c_str());
    }
    registry->start_all(bcfg);
    // Transfer ownership of the registry into the Daemon. After this point,
    // `registry` is null — all access goes through `daemon.registry()`.
    // Daemon's destructor (or shutdown_telemetry()) will stop_all() before
    // the registry is torn down, which is essential because backends may
    // hold callbacks that capture `daemon`.
    daemon.install_registry(std::move(registry));
#ifdef AGENTVFS_EBPF
    if (contains_backend(ca.requested_backends, "ebpf") && !ebpf_pi) {
        std::fprintf(stderr, "agentvfs: running without eBPF telemetry\n");
    }
    if (ebpf_pi) {
        daemon.set_policy_installer(ebpf_pi.get());
    }
#endif

    cas::ControlSocket csock(daemon);
    auto handler = [&](std::string_view line) {
        return cas::control_protocol::dispatch(daemon, line);
    };
    if (!csock.start(ca.control_sock, handler)) {
        std::fprintf(stderr, "agentvfs: failed to bind control socket %s\n",
                     ca.control_sock.c_str());
        // Order matters: control socket already failed to start, so we just
        // need to tear down telemetry and the drain. shutdown_telemetry()
        // calls stop_all() and releases the unique_ptr.
        daemon.shutdown_telemetry();
        daemon.set_policy_installer(nullptr);
        if (drain) drain->stop();
        return 3;
    }
    std::fprintf(stderr, "agentvfs: control socket at %s\n", ca.control_sock.c_str());

    cas::MountOptions opts;
    opts.mountpoint = ca.mountpoint;
    opts.foreground = ca.foreground;
    opts.single_threaded = ca.single_threaded;
    opts.passthrough_args = std::move(ca.fuse_passthrough);
    int rc = cas::run_filesystem(daemon, opts);

    // Shutdown order (preserved from commit bf4a933):
    //   1. control socket — must stop first so no new requests can call into
    //      the registry while we're tearing it down.
    //   2. telemetry registry — stop_all() quiesces all backend threads
    //      (BPF poll loop, fanotify thread, etc.) before any callbacks
    //      capturing `daemon` get destroyed.
    //   3. drain — last, so any in-flight events from step 2's stop have
    //      already been delivered.
    csock.stop();
    daemon.shutdown_telemetry();
    daemon.set_policy_installer(nullptr);
    if (drain) drain->stop();
    return rc;
}

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "workspace") {
        return cas::workspace::run_workspace_cli(argc - 1, argv + 1, current_exe_path(argv));
    }
    return run_daemon_main(argc, argv);
}
