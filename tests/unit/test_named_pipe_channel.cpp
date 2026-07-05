#include "platform/windows/named_pipe_channel.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

int main() {
    cas::NamedPipeControlChannel server;
    std::atomic<int> calls{0};
    std::string pipe = "\\\\.\\pipe\\agentvfs-test-" +
                       std::to_string(GetCurrentProcessId());
    assert(server.start(pipe, [&](std::string_view req,
                                  const cas::PeerCredentials&) {
        calls++;
        return "{\"ok\":true,\"echo\":\"" + std::string(req) + "\"}";
    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::wstring wpipe(pipe.begin(), pipe.end());
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 20; ++i) {
        h = CreateFileW(wpipe.c_str(), GENERIC_READ | GENERIC_WRITE,
                        0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) break;
        WaitNamedPipeW(wpipe.c_str(), 200);
    }
    assert(h != INVALID_HANDLE_VALUE);

    const char* req = "status\n";
    DWORD w = 0;
    WriteFile(h, req, (DWORD)std::strlen(req), &w, nullptr);
    char buf[256]; DWORD got = 0;
    ReadFile(h, buf, sizeof(buf)-1, &got, nullptr);
    buf[got] = '\0';
    assert(std::string(buf).find("\"echo\":\"status\"") != std::string::npos);
    CloseHandle(h);
    server.stop();
    assert(calls.load() == 1);
    std::printf("named_pipe_channel: OK\n");
}
