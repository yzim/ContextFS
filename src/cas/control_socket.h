#pragma once
#include "control_channel.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace cas {

class Daemon;

class ControlSocket : public ControlChannel {
public:
    explicit ControlSocket(Daemon& daemon);
    ~ControlSocket() override;

    bool start(const std::string& endpoint, RequestHandler handler) override;
    void stop() override;

private:
    void accept_loop();
    void serve_client(int fd);

    RequestHandler handler_;
    std::string socket_path_;
    int listen_fd_ = -1;
    std::thread th_;
    std::mutex workers_mu_;
    std::condition_variable workers_cv_;
    size_t active_workers_ = 0;
    std::atomic<bool> stop_{false};
};

} // namespace cas
