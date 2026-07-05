#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace cas {

using Hash = std::array<uint8_t, 32>;

static const Hash ZERO_HASH = {};

inline std::string hash_to_hex(const Hash& h) {
    char buf[65];
    for (int i = 0; i < 32; i++)
        std::snprintf(buf + i * 2, 3, "%02x", h[i]);
    buf[64] = '\0';
    return std::string(buf, 64);
}

inline bool hex_to_hash(const char* hex, Hash& out) {
    if (std::strlen(hex) < 64) return false;
    for (int i = 0; i < 32; i++) {
        unsigned val;
        if (std::sscanf(hex + i * 2, "%2x", &val) != 1) return false;
        out[i] = static_cast<uint8_t>(val);
    }
    return true;
}

inline bool is_hex_hash(const std::string& hex) {
    if (hex.size() != 64) return false;
    for (char c : hex) {
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

inline bool hex_to_hash_strict(const std::string& hex, Hash& out) {
    if (!is_hex_hash(hex)) return false;
    return hex_to_hash(hex.c_str(), out);
}

inline std::string shard_path(const Hash& h) {
    char buf[4];
    std::snprintf(buf, sizeof(buf), "%02x/", h[0]);
    return std::string(buf) + hash_to_hex(h);
}

} // namespace cas
