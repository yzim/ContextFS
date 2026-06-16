#include "platform/macos/fuse_t_preflight.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>  // mkdtemp: declared in <unistd.h> on macOS (BSD).
#include <vector>

static std::string make_tmpdir() {
    std::string base = "/tmp/agentvfs-preflight-XXXXXX";
    std::vector<char> buf(base.begin(), base.end());
    buf.push_back('\0');
    char* p = mkdtemp(buf.data());
    assert(p && "mkdtemp failed");
    return std::string(p);
}

int main() {
    // 1. Bundle present -> Ok regardless of pkg-config.
    {
        auto dir = make_tmpdir();
        std::string err;
        assert(cas::macos::preflight_check(dir, false, err)
               == cas::macos::PreflightResult::Ok);
        assert(err.empty());
        std::filesystem::remove_all(dir);
    }
    // 2. Bundle missing but pkg-config finds fuse-t -> Ok.
    {
        std::string err;
        assert(cas::macos::preflight_check("/no/such/path", true, err)
               == cas::macos::PreflightResult::Ok);
        assert(err.empty());
    }
    // 3. Neither -> NotInstalled with actionable message.
    {
        std::string err;
        assert(cas::macos::preflight_check("/no/such/path", false, err)
               == cas::macos::PreflightResult::NotInstalled);
        assert(err.find("fuse-t") != std::string::npos);
        assert(err.find("fuse-t.org") != std::string::npos ||
               err.find("brew") != std::string::npos);
    }

    std::printf("PASS test_fuse_t_preflight\n");
    return 0;
}
