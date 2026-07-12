#include "routing_fence.h"
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "routing_fence.skel.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace cas {

RoutingFence::RoutingFence() = default;
RoutingFence::~RoutingFence() { detach(); }

bool RoutingFence::load_and_attach() {
    const int pidns_fd = ::open("/proc/self/ns/pid", O_RDONLY | O_CLOEXEC);
    if (pidns_fd < 0) {
        std::fprintf(stderr,
            "agentvfs: routing fence: open /proc/self/ns/pid failed: %s; "
            "using membership reads\n",
            std::strerror(errno));
        return false;
    }

    struct stat pidns_stat {};
    if (::fstat(pidns_fd, &pidns_stat) != 0) {
        const int saved_errno = errno;
        ::close(pidns_fd);
        std::fprintf(stderr,
            "agentvfs: routing fence: stat /proc/self/ns/pid failed: %s; "
            "using membership reads\n",
            std::strerror(saved_errno));
        return false;
    }
    ::close(pidns_fd);

    routing_fence* skel = routing_fence__open();
    if (!skel) {
        std::fprintf(stderr,
            "agentvfs: routing fence: open failed; using membership reads\n");
        return false;
    }

    skel->rodata->target_pidns_dev = static_cast<__u64>(pidns_stat.st_dev);
    skel->rodata->target_pidns_ino = static_cast<__u64>(pidns_stat.st_ino);

    const int load_error = routing_fence__load(skel);
    if (load_error != 0) {
        std::fprintf(stderr,
            "agentvfs: routing fence: load failed: %s; using membership reads\n",
            std::strerror(load_error < 0 ? -load_error : load_error));
        routing_fence__destroy(skel);
        return false;
    }

    const int attach_error = routing_fence__attach(skel);
    if (attach_error != 0) {
        std::fprintf(stderr,
            "agentvfs: routing fence: attach failed: %s; using membership reads\n",
            std::strerror(attach_error < 0 ? -attach_error : attach_error));
        routing_fence__destroy(skel);
        return false;
    }
    skel_ = skel;
    return true;
}

void RoutingFence::detach() {
    if (!skel_) return;
    routing_fence__destroy(skel_);
    skel_ = nullptr;
}

uint64_t RoutingFence::generation() const {
    // Skeleton .bss is mmap'd into this process by libbpf; the BPF
    // programs update the counter with atomic adds. Relaxed is enough:
    // the kernel-side bump happens-before the migrating write(2) returns,
    // which happens-before the FUSE request performing this load.
    return __atomic_load_n(&skel_->bss->generation, __ATOMIC_RELAXED);
}

bool RoutingFence::track(Pid pid) {
    __u32 key = (__u32)pid;
    __u8 one = 1;
    return bpf_map_update_elem(bpf_map__fd(skel_->maps.tracked_pids),
                               &key, &one, BPF_ANY) == 0;
}

} // namespace cas
