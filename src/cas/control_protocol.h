#pragma once
#include "control_channel.h"
#include <string>
#include <string_view>

namespace cas {
class Daemon;

namespace control_protocol {
std::string dispatch(Daemon& daemon, std::string_view line);
std::string dispatch(Daemon& daemon,
                     std::string_view line,
                     const PeerCredentials& peer);
} // namespace control_protocol
} // namespace cas
