#include "control_socket.h"
#include "daemon.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace cas {

ControlSocket::ControlSocket(Daemon&) {}

ControlSocket::~ControlSocket() { stop(); }

bool ControlSocket::start(const std::string& endpoint, RequestHandler handler) {
    handler_ = std::move(handler);
    socket_path_ = endpoint;
    ::unlink(socket_path_.c_str());
    listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        std::fprintf(stderr, "agentvfs: control socket: socket() failed: %s\n",
                     std::strerror(errno));
        return false;
    }
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);
    if (bind(listen_fd_, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        std::fprintf(stderr, "agentvfs: control socket: bind('%s') failed: %s\n",
                     socket_path_.c_str(), std::strerror(errno));
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    chmod(socket_path_.c_str(), 0600);
    if (listen(listen_fd_, 4) != 0) {
        std::fprintf(stderr, "agentvfs: control socket: listen('%s') failed: %s\n",
                     socket_path_.c_str(), std::strerror(errno));
        close(listen_fd_);
        listen_fd_ = -1;
        ::unlink(socket_path_.c_str());
        return false;
    }
    th_ = std::thread([this] { accept_loop(); });
    return true;
}

void ControlSocket::stop() {
    stop_ = true;
    if (listen_fd_ >= 0) { ::shutdown(listen_fd_, SHUT_RDWR); close(listen_fd_); listen_fd_ = -1; }
    if (th_.joinable()) th_.join();
    std::unique_lock<std::mutex> lk(workers_mu_);
    workers_cv_.wait(lk, [this] { return active_workers_ == 0; });
    ::unlink(socket_path_.c_str());
}

void ControlSocket::accept_loop() {
    while (!stop_.load()) {
        int c = accept(listen_fd_, nullptr, nullptr);
        if (c < 0) { if (stop_.load()) return; continue; }
        {
            std::lock_guard<std::mutex> lk(workers_mu_);
            active_workers_++;
        }
        std::thread([this, c] {
            serve_client(c);
            {
                std::lock_guard<std::mutex> lk(workers_mu_);
                active_workers_--;
            }
            workers_cv_.notify_all();
        }).detach();
    }
}

void ControlSocket::serve_client(int fd) {
    std::string buf;
    char ch;
    while (read(fd, &ch, 1) == 1) {
        if (ch == '\n') {
            std::string resp = handler_(buf) + "\n";
            ssize_t n = write(fd, resp.data(), resp.size());
            (void)n;
            buf.clear();
        } else {
            buf.push_back(ch);
            if (buf.size() > 64 * 1024) break;
        }
    }
    close(fd);
}

} // namespace cas
