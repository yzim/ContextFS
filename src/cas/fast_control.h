#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __linux__
#include <sys/ioctl.h>
#endif

#define AGENTVFS_FAST_CONTROL_PATH "/.agentvfs-control"
#define AGENTVFS_FAST_CONTROL_VERSION 1u
#define AGENTVFS_FAST_CONTROL_LABEL_MAX 128u
#define AGENTVFS_FAST_CONTROL_COMMIT_HEX_LEN 64u
#define AGENTVFS_FAST_CONTROL_ERROR_MAX 256u

struct agentvfs_checkpoint_ioc {
    uint32_t version;
    uint32_t flags;
    char label[AGENTVFS_FAST_CONTROL_LABEL_MAX];
    char commit_hex[AGENTVFS_FAST_CONTROL_COMMIT_HEX_LEN + 1];
    uint8_t _pad0[3];
    int32_t result_errno;
    char error[AGENTVFS_FAST_CONTROL_ERROR_MAX];
};

#ifdef __linux__
#define AGENTVFS_IOC_MAGIC 'A'
#define AGENTVFS_IOC_CHECKPOINT \
    _IOWR(AGENTVFS_IOC_MAGIC, 1, struct agentvfs_checkpoint_ioc)
#endif

#define AGENTVFS_CHECKPOINT_IOC_SIZE 464u

#ifdef __cplusplus
static_assert(sizeof(agentvfs_checkpoint_ioc) == AGENTVFS_CHECKPOINT_IOC_SIZE,
              "agentvfs_checkpoint_ioc ABI size changed");
static_assert(offsetof(agentvfs_checkpoint_ioc, version) == 0,
              "agentvfs_checkpoint_ioc.version offset changed");
static_assert(offsetof(agentvfs_checkpoint_ioc, flags) == 4,
              "agentvfs_checkpoint_ioc.flags offset changed");
static_assert(offsetof(agentvfs_checkpoint_ioc, label) == 8,
              "agentvfs_checkpoint_ioc.label offset changed");
static_assert(offsetof(agentvfs_checkpoint_ioc, commit_hex) == 136,
              "agentvfs_checkpoint_ioc.commit_hex offset changed");
static_assert(offsetof(agentvfs_checkpoint_ioc, result_errno) == 204,
              "agentvfs_checkpoint_ioc.result_errno offset changed");
static_assert(offsetof(agentvfs_checkpoint_ioc, error) == 208,
              "agentvfs_checkpoint_ioc.error offset changed");
#ifdef __linux__
static_assert(_IOC_SIZE(AGENTVFS_IOC_CHECKPOINT) == sizeof(agentvfs_checkpoint_ioc),
              "AGENTVFS_IOC_CHECKPOINT size does not match request ABI");
#endif
#else
_Static_assert(sizeof(struct agentvfs_checkpoint_ioc) == AGENTVFS_CHECKPOINT_IOC_SIZE,
               "agentvfs_checkpoint_ioc ABI size changed");
_Static_assert(offsetof(struct agentvfs_checkpoint_ioc, version) == 0,
               "agentvfs_checkpoint_ioc.version offset changed");
_Static_assert(offsetof(struct agentvfs_checkpoint_ioc, flags) == 4,
               "agentvfs_checkpoint_ioc.flags offset changed");
_Static_assert(offsetof(struct agentvfs_checkpoint_ioc, label) == 8,
               "agentvfs_checkpoint_ioc.label offset changed");
_Static_assert(offsetof(struct agentvfs_checkpoint_ioc, commit_hex) == 136,
               "agentvfs_checkpoint_ioc.commit_hex offset changed");
_Static_assert(offsetof(struct agentvfs_checkpoint_ioc, result_errno) == 204,
               "agentvfs_checkpoint_ioc.result_errno offset changed");
_Static_assert(offsetof(struct agentvfs_checkpoint_ioc, error) == 208,
               "agentvfs_checkpoint_ioc.error offset changed");
#ifdef __linux__
_Static_assert(_IOC_SIZE(AGENTVFS_IOC_CHECKPOINT) == sizeof(struct agentvfs_checkpoint_ioc),
               "AGENTVFS_IOC_CHECKPOINT size does not match request ABI");
#endif
#endif
