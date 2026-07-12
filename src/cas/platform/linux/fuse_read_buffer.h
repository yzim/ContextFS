#pragma once
#ifndef FUSE_USE_VERSION
#define FUSE_USE_VERSION 31
#endif
#include "object_store.h"
#include <fuse3/fuse.h>

namespace cas {
int make_fd_read_buf(const BlobView& view, size_t requested, off_t offset,
                     struct fuse_bufvec** out);
}
