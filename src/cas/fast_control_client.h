#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int agentvfs_fast_checkpoint(const char* mountpoint,
                             const char* label,
                             char* commit_hex,
                             size_t commit_hex_len,
                             char* error,
                             size_t error_len);

#ifdef __cplusplus
}
#endif
