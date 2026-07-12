#pragma once
#include "branch_router.h"
#include <cstdint>

// bpftool-generated skeleton (routing_fence.skel.h); forward-declared so
// this header stays libbpf-free. Declared at global scope: an elaborated
// declaration inside namespace cas would introduce cas::routing_fence.
struct routing_fence;

namespace cas {

// Kernel-side invalidation fence for BranchRouter's pid cache. Wraps the
// routing_fence BPF skeleton: a generation counter bumped on every cgroup
// migration and on exits of tracked pids. load_and_attach() fails cleanly
// without root/CAP_BPF/kernel BTF; the router then stays on per-request
// membership reads, which are equally strict, just slower. generation()
// and track() may only be called after a successful load_and_attach().
class RoutingFence {
public:
    RoutingFence();
    ~RoutingFence();
    RoutingFence(const RoutingFence&) = delete;
    RoutingFence& operator=(const RoutingFence&) = delete;

    bool load_and_attach();
    void detach();

    uint64_t generation() const;
    bool track(Pid pid);

private:
    ::routing_fence* skel_ = nullptr;
};

} // namespace cas
