#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int agentvfs_runtime_boundary(const char* boundary_kind,
                              char* error,
                              size_t error_len);
uint64_t agentvfs_runtime_current_generation(void);

#ifdef __cplusplus
}
#endif
