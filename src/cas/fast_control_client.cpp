#include "fast_control_client.h"
#include "fast_control.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef __linux__
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace {

void copy_error(char* dst, size_t dst_len, const char* msg) {
    if (!dst || dst_len == 0) return;
    std::snprintf(dst, dst_len, "%s", msg ? msg : "");
}

bool valid_label(const char* label) {
    if (!label) return false;
    return strnlen(label, AGENTVFS_FAST_CONTROL_LABEL_MAX) <
           AGENTVFS_FAST_CONTROL_LABEL_MAX;
}

} // namespace

int agentvfs_fast_checkpoint(const char* mountpoint,
                             const char* label,
                             char* commit_hex,
                             size_t commit_hex_len,
                             char* error,
                             size_t error_len) {
    if (!mountpoint || mountpoint[0] == '\0') {
        copy_error(error, error_len, "missing mountpoint");
        return EINVAL;
    }
    if (!valid_label(label)) {
        copy_error(error, error_len, "invalid checkpoint label");
        return EINVAL;
    }
    if (!commit_hex || commit_hex_len < AGENTVFS_FAST_CONTROL_COMMIT_HEX_LEN + 1) {
        copy_error(error, error_len, "commit buffer too small");
        return EINVAL;
    }

#ifndef __linux__
    copy_error(error, error_len, "fast control is unsupported on this platform");
    return ENOTSUP;
#else
    std::string path = std::string(mountpoint) + AGENTVFS_FAST_CONTROL_PATH;
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        int saved = errno;
        copy_error(error, error_len, std::strerror(saved));
        return saved;
    }

    agentvfs_checkpoint_ioc req{};
    req.version = AGENTVFS_FAST_CONTROL_VERSION;
    std::snprintf(req.label, sizeof(req.label), "%s", label);

    int rc = ioctl(fd, AGENTVFS_IOC_CHECKPOINT, &req);
    int saved = (rc == 0) ? req.result_errno : errno;
    close(fd);

    if (rc != 0) {
        copy_error(error, error_len, req.error[0] ? req.error : std::strerror(saved));
        return saved ? saved : EIO;
    }
    if (req.result_errno != 0) {
        copy_error(error, error_len, req.error);
        return req.result_errno;
    }

    std::snprintf(commit_hex, commit_hex_len, "%s", req.commit_hex);
    copy_error(error, error_len, "");
    return 0;
#endif
}
