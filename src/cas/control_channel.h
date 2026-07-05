#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace cas {

struct PeerCredentials {
    bool trusted = false;    // in-process tests / callers that bypass a socket
    bool available = false;  // populated from the transport when supported
    int64_t pid = -1;
    int64_t uid = -1;
    int64_t gid = -1;
};

class ControlChannel {
public:
    using RequestHandler = std::function<std::string(std::string_view request,
                                                     const PeerCredentials& peer)>;
    virtual ~ControlChannel() = default;
    virtual bool start(const std::string& endpoint, RequestHandler handler) = 0;
    virtual void stop() = 0;
};

} // namespace cas
