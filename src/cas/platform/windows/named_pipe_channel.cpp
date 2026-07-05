#include "named_pipe_channel.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdio>

namespace cas {

namespace {
constexpr size_t MAX_REQUEST_BYTES = 64 * 1024;
constexpr DWORD  PIPE_BUFFER_BYTES = 64 * 1024;
}

NamedPipeControlChannel::NamedPipeControlChannel() = default;
NamedPipeControlChannel::~NamedPipeControlChannel() { stop(); }

bool NamedPipeControlChannel::start(const std::string& endpoint,
                                    RequestHandler handler) {
    handler_ = std::move(handler);
    int n = MultiByteToWideChar(CP_UTF8, 0, endpoint.data(),
                                (int)endpoint.size(), nullptr, 0);
    pipe_name_w_.assign((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, endpoint.data(), (int)endpoint.size(),
                        pipe_name_w_.data(), n);
    stop_ = false;
    accept_thread_ = std::thread([this] { accept_loop(); });
    return true;
}

void NamedPipeControlChannel::accept_loop() {
    while (!stop_.load()) {
        HANDLE h = CreateNamedPipeW(
            pipe_name_w_.c_str(),
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            PIPE_BUFFER_BYTES, PIPE_BUFFER_BYTES, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            if (!stop_.load())
                std::fprintf(stderr, "cas: CreateNamedPipeW failed %lu\n",
                             GetLastError());
            break;
        }
        BOOL ok = ConnectNamedPipe(h, nullptr)
            ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (stop_.load()) { CloseHandle(h); break; }
        if (!ok) { CloseHandle(h); continue; }

        std::lock_guard<std::mutex> lk(workers_mu_);
        workers_.emplace_back([this, h] { serve_one_client(h); });
    }
}

void NamedPipeControlChannel::serve_one_client(void* raw) {
    HANDLE h = static_cast<HANDLE>(raw);
    std::string buf;
    while (!stop_.load()) {
        char ch; DWORD got = 0;
        if (!ReadFile(h, &ch, 1, &got, nullptr) || got != 1) break;
        if (ch == '\n') {
            PeerCredentials peer;
            std::string resp = handler_(buf, peer) + "\n";
            DWORD written = 0;
            WriteFile(h, resp.data(), (DWORD)resp.size(), &written, nullptr);
            buf.clear();
            continue;
        }
        if (buf.size() >= MAX_REQUEST_BYTES) { buf.clear(); break; }
        buf.push_back(ch);
    }
    DisconnectNamedPipe(h);
    CloseHandle(h);
}

void NamedPipeControlChannel::stop() {
    if (!stop_.exchange(true)) {
        HANDLE wake = CreateFileW(pipe_name_w_.c_str(),
            GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, 0, nullptr);
        if (wake != INVALID_HANDLE_VALUE) CloseHandle(wake);
    }
    if (accept_thread_.joinable()) accept_thread_.join();
    std::vector<std::thread> to_join;
    { std::lock_guard<std::mutex> lk(workers_mu_); to_join.swap(workers_); }
    for (auto& t : to_join) if (t.joinable()) t.join();
}

}
