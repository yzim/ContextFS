#include "fuse_read_buffer.h"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace cas {

int make_fd_read_buf(const BlobView& view, size_t requested, off_t offset,
                     struct fuse_bufvec** out) {
    if (!out) return -EINVAL;
    *out = nullptr;
    if (!view) return -EIO;
    if (offset < 0) return -EINVAL;

    uint64_t logical = static_cast<uint64_t>(offset);
    uint64_t bounded = std::min<uint64_t>(logical, view.payload_size());
    uint64_t remaining = logical < view.payload_size()
        ? view.payload_size() - logical : 0;
    size_t count = static_cast<size_t>(
        std::min<uint64_t>(remaining, requested));

    auto* result = static_cast<struct fuse_bufvec*>(
        std::malloc(sizeof(struct fuse_bufvec)));
    if (!result) return -ENOMEM;
    std::memset(result, 0, sizeof(*result));
    result->count = 1;
    result->buf[0].size = count;
    result->buf[0].flags = static_cast<enum fuse_buf_flags>(
        FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK);
    result->buf[0].fd = view.fd();
    result->buf[0].pos = static_cast<off_t>(
        BlobView::kPayloadOffset + bounded);
    *out = result;
    return 0;
}

} // namespace cas
