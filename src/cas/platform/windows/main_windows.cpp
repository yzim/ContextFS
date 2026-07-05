#include "bootstrap.h"
#include "control_protocol.h"
#include "daemon.h"
#include "platform.h"
#include "platform/windows/named_pipe_channel.h"
#include "platform/windows/winfsp_preflight.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace {
struct Args {
    std::string source, mountpoint, store, pipe_name;
};

void usage() {
    std::fprintf(stderr,
        "Usage: agentvfs.exe --source <dir> --mountpoint <Z:|path> "
        "[--store <dir>] [--pipe <name>]\n"
        "Prerequisite: install WinFsp 2.0+ from https://winfsp.dev\n");
}

bool parse(int argc, char** argv, Args& out) {
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto need = [&]{ if (i+1 >= argc) { usage(); return false; } return true; };
        if (a == "--source" && need())            out.source = argv[++i];
        else if (a == "--mountpoint" && need())   out.mountpoint = argv[++i];
        else if (a == "--store" && need())        out.store = argv[++i];
        else if (a == "--pipe" && need())         out.pipe_name = argv[++i];
        else { usage(); return false; }
    }
    if (out.source.empty() || out.mountpoint.empty()) { usage(); return false; }
    if (out.store.empty()) out.store = out.source + "/.agentvfs-store";
    if (out.pipe_name.empty()) {
        std::string abs;
        try { abs = std::filesystem::absolute(out.store).string(); }
        catch (...) { abs = out.store; }
        uint32_t h = 2166136261u;
        for (unsigned char c : abs) { h ^= c; h *= 16777619u; }
        char buf[64];
        std::snprintf(buf, sizeof(buf), "\\\\.\\pipe\\agentvfs-%08x", h);
        out.pipe_name = buf;
    }
    return true;
}
} // namespace

int main(int argc, char** argv) {
    // MSVC's stderr can be line- or fully-buffered when redirected to a
    // file (as happens under Start-Process -RedirectStandardError in CI).
    // Force unbuffered so diagnostic messages reach the log promptly
    // even if the daemon hangs or aborts before a clean exit.
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    Args ca;
    if (!parse(argc, argv, ca)) return 1;

    if (!std::filesystem::exists(ca.source)) {
        std::fprintf(stderr, "agentvfs: source '%s' missing\n", ca.source.c_str());
        return 4;
    }
    std::string err;
    if (cas::win::preflight(ca.mountpoint, err) != cas::win::PreflightResult::Ok) {
        std::fprintf(stderr, "agentvfs: %s\n", err.c_str()); return 2;
    }

    cas::Daemon daemon(ca.source, ca.mountpoint, ca.store);
    if (!daemon.initialize()) {
        std::fprintf(stderr, "agentvfs: init store %s failed\n", ca.store.c_str());
        return 5;
    }
    auto bs = std::make_unique<cas::Bootstrap>(
        ca.source, daemon.store(), daemon.working_tree(), daemon.inode_map());
    bs->ensure_path("/");
    bs->start_background();
    daemon.set_bootstrap(std::move(bs));

    cas::NamedPipeControlChannel chan;
    if (!chan.start(ca.pipe_name, [&](std::string_view line,
                                      const cas::PeerCredentials& peer) {
            return cas::control_protocol::dispatch(daemon, line, peer); })) {
        std::fprintf(stderr, "agentvfs: pipe %s failed\n", ca.pipe_name.c_str());
        return 3;
    }
    std::fprintf(stderr, "agentvfs: control channel %s\n", ca.pipe_name.c_str());

    cas::MountOptions opts;
    opts.mountpoint = ca.mountpoint;
    int rc = cas::run_filesystem(daemon, opts);
    chan.stop();
    return rc;
}
