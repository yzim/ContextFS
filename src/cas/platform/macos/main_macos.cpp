// macOS daemon entry. Mirrors src/cas/platform/windows/main_windows.cpp:
// parse args → preflight → Daemon → Bootstrap → control channel →
// run_filesystem. macOS uses the existing AF_UNIX ControlSocket from
// cas_core (POSIX). No telemetry on macOS in v1.

#include "bootstrap.h"
#include "control_protocol.h"
#include "control_socket.h"
#include "daemon.h"
#include "platform.h"
#include "platform/macos/fuse_t_preflight.h"
#include "workspace_cli.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

struct Args {
    std::string source, mountpoint, store, control_sock;
    std::string volume_name = "agentvfs";
};

void usage() {
    std::fprintf(stderr,
        "Usage: agentvfs --source <dir> --mountpoint <dir> "
        "[--store <dir>] [--sock <path>] [--volume-name <name>]\n"
        "Prerequisite: install fuse-t from https://www.fuse-t.org\n");
}

bool parse(int argc, char** argv, Args& out) {
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto need = [&] { if (i+1 >= argc) { usage(); return false; } return true; };
        if      (a == "--source"      && need()) out.source = argv[++i];
        else if (a == "--mountpoint"  && need()) out.mountpoint = argv[++i];
        else if (a == "--store"       && need()) out.store = argv[++i];
        else if ((a == "--sock" || a == "--control-sock") && need()) out.control_sock = argv[++i];
        else if (a == "--volume-name" && need()) out.volume_name = argv[++i];
        else if (a == "-o"            && need()) { ++i; }
        else if (a == "-f")           { /* foreground is already the only mode */ }
        else if (a.rfind("--telemetry=", 0) == 0) {
            /* Workspace CLI can pass Linux telemetry flags; macOS v1 ignores them. */
        }
        else if (a == "-h" || a == "--help") { usage(); return false; }
        else { usage(); return false; }
    }
    if (out.source.empty() || out.mountpoint.empty()) { usage(); return false; }
    if (out.store.empty()) out.store = out.source + "/.agentvfs-store";
    if (out.control_sock.empty()) {
        // FNV-1a over the absolute store path so the same source/store
        // resolves to a stable socket path across daemon restarts —
        // matches the Windows main's pipe-name scheme.
        std::string abs;
        try { abs = std::filesystem::absolute(out.store).string(); }
        catch (...) { abs = out.store; }
        uint32_t h = 2166136261u;
        for (unsigned char c : abs) { h ^= c; h *= 16777619u; }
        char buf[64];
        std::snprintf(buf, sizeof(buf), "/tmp/agentvfs-%08x.sock", h);
        out.control_sock = buf;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "workspace") {
        return cas::workspace::run_workspace_cli(argc - 1, argv + 1, argv[0]);
    }

    Args ca;
    if (!parse(argc, argv, ca)) return 1;

    if (!std::filesystem::exists(ca.source)) {
        std::fprintf(stderr, "agentvfs: source '%s' missing\n", ca.source.c_str());
        return 1;
    }

    std::string err;
    if (cas::macos::preflight(err) != cas::macos::PreflightResult::Ok) {
        std::fprintf(stderr, "agentvfs: %s\n", err.c_str());
        return 2;
    }

    cas::Daemon daemon(ca.source, ca.mountpoint, ca.store);
    if (!daemon.initialize()) {
        std::fprintf(stderr, "agentvfs: init store %s failed\n",
                     ca.store.c_str());
        return 1;
    }
    auto bs = std::make_unique<cas::Bootstrap>(
        ca.source, daemon.store(), daemon.working_tree(),
        daemon.inode_map());
    bs->ensure_path("/");
    bs->start_background();
    daemon.set_bootstrap(std::move(bs));

    cas::ControlSocket csock(daemon);
    if (!csock.start(ca.control_sock, [&](std::string_view line,
                                          const cas::PeerCredentials& peer) {
            return cas::control_protocol::dispatch(daemon, line, peer);
        })) {
        std::fprintf(stderr,
                     "agentvfs: failed to bind control socket %s (errno=%d)\n",
                     ca.control_sock.c_str(), errno);
        return (errno == EACCES) ? 4 : 1;
    }
    std::fprintf(stderr, "agentvfs: control socket at %s\n",
                 ca.control_sock.c_str());

    // fuse-t / libfuse 2.9 install their own SIGINT/SIGTERM handlers
    // and unmount cleanly on signal. The is_appledouble() filter inside
    // the adapter is the actual AppleDouble/Spotlight mitigation —
    // noappledouble/noapplexattr are macFUSE-only mount options that
    // fuse-t does not advertise.
    //
    // attr_timeout / entry_timeout / negative_timeout = 0 are critical
    // for rollback correctness. fuse-t bridges through macOS NFS loopback,
    // which caches attributes and dentries independently of libfuse's
    // per-fh direct_io. Without these, a checkpoint -> overwrite ->
    // rollback -> read sequence reads the post-overwrite page from the
    // NFS client's attribute cache when old and new content are the
    // same size, never reaching cas_read.
    std::string opt_string =
        "volname=" + ca.volume_name +
        ",iosize=1048576"
        ",attr_timeout=0,entry_timeout=0,negative_timeout=0";

    cas::MountOptions opts;
    opts.mountpoint = ca.mountpoint;
    opts.foreground = true;
    opts.passthrough_args = {"-o", opt_string};

    int rc = cas::run_filesystem(daemon, opts);
    csock.stop();
    return rc;
}
