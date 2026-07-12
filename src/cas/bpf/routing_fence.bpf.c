#include <vmlinux.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

char LICENSE[] SEC("license") = "GPL";

/* Identity of the daemon's PID namespace. The FUSE request context supplies
 * caller TIDs relative to this namespace, so exit invalidation must translate
 * the exiting task into the same namespace before consulting tracked_pids. */
const volatile __u64 target_pidns_dev = 0;
const volatile __u64 target_pidns_ino = 0;

/* Bumped on every cgroup migration and on exits of tracked pids. Lives in
 * .bss so the daemon reads it as plain skeleton-mmap'd memory: the hot
 * path costs one atomic load, no syscall. */
__u64 generation SEC(".bss");

/* Entries decay only on pid exit: a live pid whose daemon-side cache entry
 * was evicted stays here until it exits, occupying a slot. Bounded at 4096;
 * when full, new pids simply stay on per-request membership reads. */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 4096);
    __type(key, __u32);
    __type(value, __u8);
} tracked_pids SEC(".maps");

/* Bump unconditionally on cgroup migration. The daemon tracks exact caller
 * TIDs, while cgroup.procs migration can be reported for the thread-group
 * leader; filtering here could therefore miss a cached worker TID. Migrations
 * are rare, and the unconditional bump costs at most one membership re-read
 * per cached TID. */
SEC("tp_btf/cgroup_attach_task")
int BPF_PROG(on_cgroup_attach, struct cgroup* dst, const char* path,
             struct task_struct* task, bool threadgroup)
{
    __sync_fetch_and_add(&generation, 1);
    return 0;
}

/* Exits are constant system-wide churn: only exits of exact namespace-relative
 * TIDs cached by the daemon may invalidate, or the cache would never stay
 * warm. FUSE supplies the caller TID relative to the daemon's PID namespace;
 * translate the exiting task into that namespace and use only that pid key. */
SEC("tp_btf/sched_process_exit")
int BPF_PROG(on_process_exit, struct task_struct* task)
{
    struct bpf_pidns_info nsdata = {};
    if (bpf_get_ns_current_pid_tgid(target_pidns_dev, target_pidns_ino,
                                    &nsdata, sizeof(nsdata)) != 0)
        return 0;

    __u32 tid = nsdata.pid;
    __u8* hit = bpf_map_lookup_elem(&tracked_pids, &tid);
    if (!hit) return 0;
    __sync_fetch_and_add(&generation, 1);
    bpf_map_delete_elem(&tracked_pids, &tid);
    return 0;
}
