#include "runtime_supervisor.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace cas {

// ---------------------------------------------------------------------------
RuntimeSupervisor::RuntimeSupervisor(RuntimeProcessController* process_controller)
    : process_controller_(process_controller) {}

bool RuntimeSupervisor::supported(std::string& error) const {
    return process_controller_->supported(error);
}

bool RuntimeSupervisor::register_runtime(const RuntimeCreateRequest& request,
                                         std::string& error) {
    if (!supported(error)) return false;
    std::lock_guard<std::mutex> lk(mu_);
    std::string id = request.runtime_id;
    if (id.empty()) id = make_runtime_id_locked();
    if (runtimes_.count(id) != 0) {
        error = "runtime already registered";
        return false;
    }
    Runtime r;
    r.runtime_id = id;
    r.branch = request.branch;
    r.root_pid = request.root_pid;
    r.active_process_group_id = request.process_group_id;
    r.generation = 1;
    r.command_ref = request.command_ref;
    r.cwd = request.cwd;
    r.cooperative = request.cooperative;
    r.control_token = request.control_token;
    runtimes_[id] = std::move(r);
    error.clear();
    return true;
}

void RuntimeSupervisor::build_status_locked(const Runtime& r,
                                            RuntimeStatus& out) const {
    out = RuntimeStatus{};
    out.runtime_id = r.runtime_id;
    out.branch = r.branch;
    out.root_pid = r.root_pid;
    out.active_process_group_id = r.active_process_group_id;
    out.generation = r.generation;
    out.command_ref = r.command_ref;
    out.cooperative = r.cooperative;

    bool any_template = false;
    bool any_alive = false;
    for (const std::string& tid : r.template_ids) {
        auto it = templates_.find(tid);
        if (it == templates_.end()) continue;
        const Template& t = it->second;
        bool alive = t.ready && t.template_pid > 0 &&
                     process_controller_->process_alive(t.template_pid);
        any_template = true;
        if (alive) any_alive = true;
        TemplateStatus ts;
        ts.template_id = t.template_id;
        ts.runtime_id = t.runtime_id;
        ts.template_pid = t.template_pid;
        ts.template_process_group_id = t.template_process_group_id;
        ts.generation = t.generation;
        ts.alive = alive;
        ts.union_state_id = t.union_state_id;
        ts.fs_commit = t.fs_commit;
        ts.restore_eligibility = alive ? RestoreEligibility::LiveRuntimeRestorable
                                       : RestoreEligibility::FsOnly;
        out.templates.push_back(std::move(ts));
    }
    if (!any_template) {
        out.restore_eligibility = RestoreEligibility::MetadataOnly;
    } else if (any_alive) {
        out.restore_eligibility = RestoreEligibility::LiveRuntimeRestorable;
    } else {
        out.restore_eligibility = RestoreEligibility::FsOnly;
    }
}

bool RuntimeSupervisor::status(const std::string& runtime_id,
                               RuntimeStatus& out,
                               std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = runtimes_.find(runtime_id);
    if (it == runtimes_.end()) {
        error = "unknown runtime";
        return false;
    }
    build_status_locked(it->second, out);
    error.clear();
    return true;
}

std::vector<RuntimeStatus> RuntimeSupervisor::list() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<RuntimeStatus> result;
    result.reserve(runtimes_.size());
    for (auto& kv : runtimes_) {
        RuntimeStatus s;
        build_status_locked(kv.second, s);
        result.push_back(std::move(s));
    }
    return result;
}

bool RuntimeSupervisor::begin_snapshot(const RuntimeSnapshotRequest& request,
                                       std::string& operation_id,
                                       std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = runtimes_.find(request.runtime_id);
    if (it == runtimes_.end()) {
        error = "unknown runtime";
        return false;
    }
    Runtime& r = it->second;
    if (r.has_pending_snapshot) {
        error = "snapshot already in progress";
        return false;
    }
    r.has_pending_snapshot = true;
    r.pending_op_id = make_operation_id_locked();
    r.pending_boundary_kind = request.boundary_kind;
    r.pending_agent_state_id = request.agent_state_id;
    r.pending_timeout_ms = request.timeout_ms;
    r.pending_boundary_id.clear();
    r.boundary_observed = false;
    operation_id = r.pending_op_id;
    error.clear();
    return true;
}

bool RuntimeSupervisor::cancel_snapshot(const std::string& runtime_id,
                                        std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = runtimes_.find(runtime_id);
    if (it == runtimes_.end()) {
        error = "unknown runtime";
        return false;
    }
    Runtime& r = it->second;
    // If a boundary was already observed for the pending snapshot, release
    // that parked boundary handler with an error so it does not block inside
    // wait_boundary_action until its timeout fires.
    if (r.boundary_observed && !r.pending_boundary_id.empty()) {
        auto bit = boundaries_.find(r.pending_boundary_id);
        if (bit != boundaries_.end()) {
            Boundary& b = bit->second;
            if (!b.action_set) {
                b.action.action = "error";
                b.action.operation_id = b.operation_id;
                b.action.template_id.clear();
                b.action.error = "snapshot cancelled";
                b.action_set = true;
            }
        }
    }
    r.has_pending_snapshot = false;
    r.pending_op_id.clear();
    r.pending_boundary_kind.clear();
    r.pending_agent_state_id.clear();
    r.pending_timeout_ms = 0;
    r.pending_boundary_id.clear();
    r.boundary_observed = false;
    cv_.notify_all();
    error.clear();
    return true;
}

bool RuntimeSupervisor::observe_boundary(const std::string& runtime_id,
                                         const std::string& control_token,
                                         int64_t pid,
                                         uint64_t generation,
                                         const std::string& boundary_kind,
                                         std::string& boundary_id,
                                         std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = runtimes_.find(runtime_id);
    if (it == runtimes_.end()) {
        error = "unknown runtime";
        return false;
    }
    Runtime& r = it->second;
    if (r.control_token.empty() || control_token != r.control_token) {
        error = "invalid control token";
        return false;
    }
    if (!r.has_pending_snapshot || r.boundary_observed) {
        // Either no snapshot is pending or the pending one was already
        // observed. The control-protocol handler maps this to
        // action:"continue".
        error = "no pending snapshot";
        return false;
    }
    std::string id = make_boundary_id_locked();
    Boundary b;
    b.boundary_id = id;
    b.runtime_id = runtime_id;
    b.operation_id = r.pending_op_id;
    b.pid = pid;
    b.generation = generation;
    b.boundary_kind = boundary_kind;
    boundaries_[id] = std::move(b);

    r.pending_boundary_id = id;
    r.boundary_observed = true;
    boundary_id = id;
    cv_.notify_all();
    error.clear();
    return true;
}

bool RuntimeSupervisor::release_boundary_for_snapshot(
    const std::string& boundary_id,
    RuntimeBoundaryAction& out,
    std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    auto bit = boundaries_.find(boundary_id);
    if (bit == boundaries_.end()) {
        error = "unknown boundary";
        return false;
    }
    Boundary& b = bit->second;
    if (b.action_set) {
        error = "boundary already released";
        return false;
    }
    std::string template_id = make_template_id_locked();
    Template t;
    t.template_id = template_id;
    t.runtime_id = b.runtime_id;
    t.generation = b.generation;
    templates_[template_id] = std::move(t);

    auto rit = runtimes_.find(b.runtime_id);
    if (rit != runtimes_.end()) {
        Runtime& r = rit->second;
        r.template_ids.push_back(template_id);
        r.pending_template_id = template_id;
        // NOTE: has_pending_snapshot / boundary_observed / pending_op_id /
        // pending_boundary_id are intentionally NOT cleared here. Keeping
        // has_pending_snapshot true until the union state is durable (end of
        // Daemon::snapshot_runtime) serializes snapshots per-runtime: a second
        // begin_snapshot is refused while the first is still in
        // wait_for_template_ready / union-write, so the first's
        // pending_template_id cannot be overwritten by a second
        // release_boundary_for_snapshot. observe_boundary also keeps returning
        // "no pending snapshot" -> "continue" (boundary_observed stays true),
        // so a cooperative runtime calling runtime.boundary in a loop during
        // the snapshot does not park on a second boundary.
    }

    b.template_id = template_id;
    b.action.action = "snapshot";
    b.action.operation_id = b.operation_id;
    b.action.template_id = template_id;
    b.action.error.clear();
    b.action_set = true;
    out = b.action;
    cv_.notify_all();
    error.clear();
    return true;
}

bool RuntimeSupervisor::release_boundary_with_error(
    const std::string& boundary_id,
    const std::string& message,
    RuntimeBoundaryAction& out,
    std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    auto bit = boundaries_.find(boundary_id);
    if (bit == boundaries_.end()) {
        error = "unknown boundary";
        return false;
    }
    Boundary& b = bit->second;
    if (b.action_set) {
        error = "boundary already released";
        return false;
    }
    auto rit = runtimes_.find(b.runtime_id);
    if (rit != runtimes_.end()) {
        Runtime& r = rit->second;
        r.has_pending_snapshot = false;
        r.boundary_observed = false;
        r.pending_op_id.clear();
        r.pending_boundary_id.clear();
    }
    b.action.action = "error";
    b.action.operation_id = b.operation_id;
    b.action.template_id.clear();
    b.action.error = message;
    b.action_set = true;
    out = b.action;
    cv_.notify_all();
    error.clear();
    return true;
}

bool RuntimeSupervisor::template_ready(const std::string& runtime_id,
                                       const std::string& control_token,
                                       const std::string& template_id,
                                       int64_t template_pid,
                                       int64_t template_process_group_id,
                                       uint64_t generation,
                                       std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    auto tit = templates_.find(template_id);
    if (tit == templates_.end()) {
        error = "unknown template";
        return false;
    }
    Template& t = tit->second;
    if (!runtime_id.empty() && t.runtime_id != runtime_id) {
        error = "template runtime mismatch";
        return false;
    }
    auto rit = runtimes_.find(t.runtime_id);
    if (rit == runtimes_.end()) {
        error = "unknown runtime";
        return false;
    }
    if (rit->second.control_token.empty() ||
        control_token != rit->second.control_token) {
        error = "invalid control token";
        return false;
    }
    t.template_pid = template_pid;
    t.template_process_group_id = template_process_group_id;
    t.generation = generation;
    t.ready = true;
    cv_.notify_all();
    error.clear();
    return true;
}

bool RuntimeSupervisor::template_status(const std::string& template_id,
                                        TemplateStatus& out,
                                        std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = templates_.find(template_id);
    if (it == templates_.end()) {
        error = "unknown template";
        return false;
    }
    const Template& t = it->second;
    bool alive = t.ready && t.template_pid > 0 &&
                 process_controller_->process_alive(t.template_pid);
    out = TemplateStatus{};
    out.template_id = t.template_id;
    out.runtime_id = t.runtime_id;
    out.template_pid = t.template_pid;
    out.template_process_group_id = t.template_process_group_id;
    out.generation = t.generation;
    out.alive = alive;
    out.union_state_id = t.union_state_id;
    out.fs_commit = t.fs_commit;
    out.restore_eligibility = alive ? RestoreEligibility::LiveRuntimeRestorable
                                    : RestoreEligibility::FsOnly;
    error.clear();
    return true;
}

bool RuntimeSupervisor::restore_template_status(const std::string& runtime_id,
                                                TemplateStatus& out,
                                                std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    auto rit = runtimes_.find(runtime_id);
    if (rit == runtimes_.end()) {
        error = "unknown runtime";
        return false;
    }
    const Runtime& r = rit->second;
    if (!r.has_pending_restore || r.restore_template_id.empty()) {
        error = "no pending restore";
        return false;
    }
    auto tit = templates_.find(r.restore_template_id);
    if (tit == templates_.end()) {
        error = "unknown template";
        return false;
    }
    const Template& t = tit->second;
    bool alive = t.ready && t.template_pid > 0 &&
                 process_controller_->process_alive(t.template_pid);
    out = TemplateStatus{};
    out.template_id = t.template_id;
    out.runtime_id = t.runtime_id;
    out.template_pid = t.template_pid;
    out.template_process_group_id = t.template_process_group_id;
    out.generation = t.generation;
    out.alive = alive;
    out.union_state_id = t.union_state_id;
    out.fs_commit = t.fs_commit;
    out.restore_eligibility = alive ? RestoreEligibility::LiveRuntimeRestorable
                                    : RestoreEligibility::FsOnly;
    error.clear();
    return true;
}

bool RuntimeSupervisor::attach_union_state(const std::string& template_id,
                                           const std::string& union_state_id,
                                           const Hash& fs_commit,
                                           std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = templates_.find(template_id);
    if (it == templates_.end()) {
        error = "unknown template";
        return false;
    }
    Template& t = it->second;
    t.union_state_id = union_state_id;
    t.fs_commit = fs_commit;
    t.published = true;
    t.publish_error.clear();
    cv_.notify_all();
    error.clear();
    return true;
}

bool RuntimeSupervisor::fail_template_publish(const std::string& template_id,
                                              const std::string& message,
                                              std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = templates_.find(template_id);
    if (it == templates_.end()) {
        error = "unknown template";
        return false;
    }
    Template& t = it->second;
    t.publish_error = message;
    // Do NOT erase the template here: a blocked wait_template_published waiter
    // must be able to re-look it up after the cv fires to read publish_error.
    cv_.notify_all();
    error.clear();
    return true;
}

bool RuntimeSupervisor::begin_restore(const RuntimeRestoreIntent& intent,
                                      std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    auto rit = runtimes_.find(intent.runtime_id);
    if (rit == runtimes_.end()) {
        error = "unknown runtime";
        return false;
    }
    Runtime& r = rit->second;
    auto tit = templates_.find(intent.template_id);
    if (tit == templates_.end()) {
        error = "unknown template";
        return false;
    }
    Template& t = tit->second;
    // Guard the template-ownership invariant: the template must belong to the
    // runtime being restored. template_ready() enforces the same check; without
    // it here a caller could restore runtime A against runtime B's template.
    if (t.runtime_id != intent.runtime_id) {
        error = "unknown template";
        return false;
    }
    if (r.active_process_group_id > 0) {
        std::string freeze_error;
        if (!process_controller_->freeze_process_group(
                r.active_process_group_id, freeze_error)) {
            error = "freeze failed: " + freeze_error;
            return false;
        }
    }
    r.has_pending_restore = true;
    r.restore_template_id = intent.template_id;
    r.restore_target_generation = intent.target_generation;
    r.generation_ready_flag = false;
    r.retire_pgid = 0;  // generation_ready will stash the prior pgid here
    t.restore_pending = true;
    t.restore_released = false;
    t.restore_in_flight = false;  // fresh hand-off for this intent
    t.target_generation = intent.target_generation;
    error.clear();
    return true;
}

bool RuntimeSupervisor::publish_restore(const std::string& runtime_id,
                                        std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    auto rit = runtimes_.find(runtime_id);
    if (rit == runtimes_.end()) {
        error = "unknown runtime";
        return false;
    }
    Runtime& r = rit->second;
    if (!r.has_pending_restore || r.restore_template_id.empty()) {
        error = "no pending restore";
        return false;
    }
    auto tit = templates_.find(r.restore_template_id);
    if (tit == templates_.end()) {
        error = "unknown template";
        return false;
    }
    Template& t = tit->second;
    if (!t.restore_pending || t.runtime_id != runtime_id) {
        error = "no pending restore";
        return false;
    }
    t.restore_released = true;
    cv_.notify_all();
    error.clear();
    return true;
}

bool RuntimeSupervisor::abort_restore(const std::string& runtime_id,
                                      std::string& error) {
    int64_t frozen_pgid = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = runtimes_.find(runtime_id);
        if (it == runtimes_.end()) {
            error = "unknown runtime";
            return false;
        }
        Runtime& r = it->second;
        frozen_pgid = r.active_process_group_id;
        r.has_pending_restore = false;
        r.generation_ready_flag = false;
        if (!r.restore_template_id.empty()) {
            auto tit = templates_.find(r.restore_template_id);
            if (tit != templates_.end()) {
                tit->second.restore_pending = false;
                tit->second.restore_released = false;
                tit->second.restore_in_flight = false;
                tit->second.target_generation = 0;
            }
            r.restore_template_id.clear();
        }
        r.restore_target_generation = 0;
        error.clear();
    }
    // Resume the frozen active pgid outside mu_ so the registry stays
    // responsive during the signal round-trip (same discipline as
    // retire_pending_generation's terminate).
    if (frozen_pgid > 0) {
        std::string resume_error;
        if (!process_controller_->resume_process_group(frozen_pgid,
                                                      resume_error)) {
            // Surface the failure: a frozen pgid left SIGSTOP'd with no
            // diagnostic is a silent resource leak (EPERM/ESRCH).
            if (resume_error.empty()) resume_error = "unknown resume error";
            std::fprintf(stderr,
                         "agentvfs: abort_restore resume failed for runtime %s: %s\n",
                         runtime_id.c_str(), resume_error.c_str());
            error = "resume failed: " + resume_error;
        }
    }
    return true;
}

bool RuntimeSupervisor::template_poll(const std::string& template_id,
                                      const std::string& control_token,
                                      RuntimeTemplateAction& out,
                                      std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = templates_.find(template_id);
    if (it == templates_.end()) {
        error = "unknown template";
        return false;
    }
    Template& t = it->second;
    auto rit = runtimes_.find(t.runtime_id);
    if (rit == runtimes_.end()) {
        error = "unknown runtime";
        return false;
    }
    if (rit->second.control_token.empty() ||
        control_token != rit->second.control_token) {
        error = "invalid control token";
        return false;
    }
    if (t.dropped) {
        // The drop action is the terminal hand-off to the parked template: it
        // _exit(0)s on receipt. This poll is therefore the last consumer of
        // the record; erase it now so dropped templates do not accumulate.
        out.action = "drop";
        out.target_generation = 0;
        templates_.erase(it);
        error.clear();
        return true;
    }
    if (t.restore_pending && t.restore_released && !t.restore_in_flight) {
        // First poll for this pending intent: hand the restore off to the
        // template exactly once. Subsequent polls return "wait" until the
        // forked grandchild reaches generation_ready() (which clears the
        // intent) or abort_restore() drops it; otherwise the poll loop would
        // fork multiple grandchildren from one restore.
        out.action = "restore";
        out.target_generation = t.target_generation;
        it->second.restore_in_flight = true;
    } else {
        out.action = "wait";
        out.target_generation = 0;
    }
    error.clear();
    return true;
}

bool RuntimeSupervisor::generation_ready(const std::string& runtime_id,
                                         const std::string& control_token,
                                         int64_t pid,
                                         int64_t active_process_group_id,
                                         uint64_t generation,
                                         std::string& error) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = runtimes_.find(runtime_id);
        if (it == runtimes_.end()) {
            error = "unknown runtime";
            return false;
        }
        Runtime& r = it->second;
        if (r.control_token.empty() || control_token != r.control_token) {
            error = "invalid control token";
            return false;
        }
        // Single-active invariant: only honor a generation.ready that matches a
        // pending restore for the SAME target generation. If the daemon already
        // aborted the restore (has_pending_restore cleared by abort_restore) or
        // a newer restore retargeted the generation, this is a LATE/stale
        // grandchild whose ready ack must be rejected, not honored. Accepting
        // it would overwrite root_pid/active_process_group_id/generation and
        // stash retire_pgid that no restore_runtime will ever consume, leaving
        // both the SIGCONT'd prior active AND this grandchild servicing the
        // runtime -> single-active violation.
        if (!r.has_pending_restore ||
            r.restore_target_generation != generation) {
            error = "restore aborted or stale";
            return false;
        }
        // Stash the previously-active process group for the daemon's
        // restore_runtime() to retire. We deliberately do NOT terminate it
        // here: this call is the acknowledgement the restored grandchild is
        // blocked on. Record the new generation, hand the prior pgid to
        // restore_runtime, and return so the new active runtime resumes before
        // old-generation cleanup.
        r.retire_pgid = r.active_process_group_id;
        // Record the new generation BEFORE retiring the prior process group so
        // status() observers see the viable generation during the kill window.
        r.root_pid = pid;
        r.active_process_group_id = active_process_group_id;
        r.generation = generation;
        r.generation_ready_flag = true;
        r.has_pending_restore = false;
        // Clear the restore action on the template that drove this generation.
        if (!r.restore_template_id.empty()) {
            auto tit = templates_.find(r.restore_template_id);
            if (tit != templates_.end()) {
                tit->second.restore_pending = false;
                tit->second.restore_released = false;
                tit->second.restore_in_flight = false;
            }
            r.restore_template_id.clear();
        }
        cv_.notify_all();
        error.clear();
    }
    return true;
}

bool RuntimeSupervisor::retire_pending_generation(const std::string& runtime_id,
                                                   std::string& error) {
    int64_t prior_pgid = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = runtimes_.find(runtime_id);
        if (it == runtimes_.end()) {
            error = "unknown runtime";
            return false;
        }
        prior_pgid = it->second.retire_pgid;
        it->second.retire_pgid = 0;
    }
    // Terminate the retired process group outside mu_ so the registry remains
    // usable (status / list / observe_boundary / begin_snapshot / wait_*) while
    // the controller performs signal delivery. The daemon drives restores
    // serially per runtime, so this cannot race a concurrent restore of the
    // same runtime.
    error.clear();
    if (prior_pgid > 0) {
        std::string term_error;
        if (!process_controller_->terminate_process_group(prior_pgid,
                                                          term_error)) {
            // Surface the failure: a frozen old active left un-killed (EPERM/
            // ESRCH) with no diagnostic is a silent leak. Mirror abort_restore's
            // stderr discipline. We still return true (the runtime is known and
            // the retire record is consumed); the error is propagated via the
            // out-param for observability but does not fail the restore, whose
            // filesystem rollback already succeeded.
            if (term_error.empty()) term_error = "unknown terminate error";
            std::fprintf(stderr,
                         "agentvfs: retire pending generation failed to terminate pgid %lld: %s\n",
                         static_cast<long long>(prior_pgid),
                         term_error.c_str());
            error = "terminate failed: " + term_error;
        }
    }
    return true;
}

bool RuntimeSupervisor::drop_template(const std::string& template_id,
                                      std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = templates_.find(template_id);
    if (it == templates_.end()) {
        error = "unknown template";
        return false;
    }
    Template& t = it->second;
    auto rit = runtimes_.find(t.runtime_id);
    if (rit != runtimes_.end()) {
        Runtime& r = rit->second;
        if (r.has_pending_restore && r.restore_template_id == template_id) {
            error = "template restore in progress";
            return false;
        }
    }
    // Mark the template dropped and wake any waiter, but do NOT erase yet: the
    // parked template process is polling runtime.template.poll and must observe
    // action:"drop" on its next poll to _exit(0) cleanly. Erasing now would
    // make the next poll return {"ok":false,"error":"unknown template"}, which
    // the client treats as backoff -> it would spin at ~200Hz forever (the
    // ok:false path did not previously count against the give-up window).
    // template_poll erases the record once the drop action has been read.
    t.dropped = true;
    // Stop advertising the template from the runtime's status/list so a dropped
    // template is not chosen for a new restore.
    if (rit != runtimes_.end()) {
        Runtime& r = rit->second;
        r.template_ids.erase(
            std::remove(r.template_ids.begin(), r.template_ids.end(),
                        template_id),
            r.template_ids.end());
        if (r.pending_template_id == template_id) {
            r.pending_template_id.clear();
        }
    }
    cv_.notify_all();
    error.clear();
    return true;
}

// ---------------------------------------------------------------------------
// Blocking coordination primitives.
// ---------------------------------------------------------------------------
bool RuntimeSupervisor::wait_for_boundary(const std::string& runtime_id,
                                          uint64_t timeout_ms,
                                          std::string& boundary_id,
                                          std::string& error) {
    std::unique_lock<std::mutex> lk(mu_);
    auto it = runtimes_.find(runtime_id);
    if (it == runtimes_.end()) {
        error = "unknown runtime";
        return false;
    }
    Runtime& r = it->second;
    bool ok = cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                           [&] { return r.boundary_observed; });
    if (!ok) {
        error = "snapshot timeout";
        return false;
    }
    boundary_id = r.pending_boundary_id;
    error.clear();
    return true;
}

bool RuntimeSupervisor::wait_for_template_ready(const std::string& runtime_id,
                                                uint64_t timeout_ms,
                                                std::string& error) {
    std::unique_lock<std::mutex> lk(mu_);
    auto it = runtimes_.find(runtime_id);
    if (it == runtimes_.end()) {
        error = "unknown runtime";
        return false;
    }
    Runtime& r = it->second;
    bool ok = cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] {
        if (r.pending_template_id.empty()) return false;
        auto tit = templates_.find(r.pending_template_id);
        return tit != templates_.end() && tit->second.ready;
    });
    if (!ok) {
        error = "snapshot timeout";
        return false;
    }
    error.clear();
    return true;
}

bool RuntimeSupervisor::wait_for_generation_ready(
    const std::string& runtime_id,
    uint64_t timeout_ms,
    std::string& error) {
    std::unique_lock<std::mutex> lk(mu_);
    auto it = runtimes_.find(runtime_id);
    if (it == runtimes_.end()) {
        error = "unknown runtime";
        return false;
    }
    Runtime& r = it->second;
    bool ok = cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                           [&] { return r.generation_ready_flag; });
    if (!ok) {
        error = "restore timeout";
        return false;
    }
    error.clear();
    return true;
}

bool RuntimeSupervisor::wait_boundary_action(const std::string& boundary_id,
                                             uint64_t timeout_ms,
                                             RuntimeBoundaryAction& out,
                                             std::string& error) {
    std::unique_lock<std::mutex> lk(mu_);
    // Initial existence check.
    if (boundaries_.find(boundary_id) == boundaries_.end()) {
        error = "unknown boundary";
        return false;
    }
    // The predicate RE-LOOKS-UP the boundary by id each evaluation: we never
    // hold a Boundary& across cv_.wait_for. Mirrors the hardened
    // wait_template_published pattern -- a concurrent consumer/erase would
    // dangle a captured reference. Treat "gone" as terminal (stop waiting).
    // The wait_for return value is intentionally discarded: we re-check
    // action_set under the lock below, which is authoritative whether we timed
    // out or were notified.
    cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] {
        auto it = boundaries_.find(boundary_id);
        if (it == boundaries_.end()) return true;  // gone -> stop waiting
        return it->second.action_set;
    });
    // Re-find under the lock to inspect the terminal state by value (never a
    // captured reference).
    auto it = boundaries_.find(boundary_id);
    if (it == boundaries_.end()) {
        error = "unknown boundary";
        return false;
    }
    const Boundary& b = it->second;
    if (!b.action_set) {
        // Timed out before an action was set. The waiter created this boundary
        // via observe_boundary and is giving up, so it owns cleanup: reap the
        // record now. Without this erase, wait_boundary_action was the SOLE
        // eraser of boundaries_ and only erased on a successful consume -- a
        // timeout left the record behind, and a later
        // release_boundary_for_snapshot / release_boundary_with_error /
        // cancel_snapshot would set action_set on a boundary with no consumer,
        // leaking one Boundary (and its strings) per such race for the daemon's
        // lifetime. The producers tolerate a missing boundary and return
        // "unknown boundary"; the daemon's failure paths are unaffected (the
        // snapshot already failed with a timeout on this side). Every boundary
        // is now erased either here on timeout or below on consume -- zero leak.
        boundaries_.erase(it);
        error = "snapshot timeout";
        return false;
    }
    out = b.action;
    // Consume: this is the single waiter for the boundary (one
    // runtime.boundary handler per id). Erase now so boundaries_ does not grow
    // unboundedly across repeated snapshots -- previously one Boundary (with its
    // strings) accumulated per snapshot for the daemon's lifetime.
    boundaries_.erase(it);
    error.clear();
    return true;
}

bool RuntimeSupervisor::wait_template_published(const std::string& template_id,
                                                uint64_t timeout_ms,
                                                std::string& error) {
    std::unique_lock<std::mutex> lk(mu_);
    // Initial existence check.
    if (templates_.find(template_id) == templates_.end()) {
        error = "unknown template";
        return false;
    }
    // The predicate must RE-LOOKUP the template each evaluation: a concurrent
    // drop_template could erase the node, and fail_template_publish marks
    // publish_error without erasing. We never capture a Template& across the
    // wait (that would dangle if the node is erased under us). Treat "not
    // found OR published OR publish_error non-empty OR dropped" as ready.
    bool ok = cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] {
        auto it = templates_.find(template_id);
        if (it == templates_.end()) return true;  // gone -> stop waiting
        const Template& t = it->second;
        return t.published || !t.publish_error.empty() || t.dropped;
    });
    // Re-find under the lock to inspect the terminal state by value (never a
    // captured reference).
    auto it = templates_.find(template_id);
    if (it == templates_.end()) {
        error = "unknown template";
        return false;
    }
    const Template& t = it->second;
    if (t.dropped) {
        error = "template dropped";
        return false;
    }
    if (!t.publish_error.empty()) {
        error = t.publish_error;
        return false;
    }
    if (!ok || !t.published) {
        // Timed out before reaching a terminal (published) state.
        error = "snapshot timeout";
        return false;
    }
    error.clear();
    return true;
}

// ---------------------------------------------------------------------------
// ID generation (must be called with mu_ held).
// ---------------------------------------------------------------------------
std::string RuntimeSupervisor::make_runtime_id_locked() {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "rt-%016llx",
                  static_cast<unsigned long long>(++runtime_counter_));
    return buf;
}

std::string RuntimeSupervisor::make_template_id_locked() {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "tmpl-%016llx",
                  static_cast<unsigned long long>(++template_counter_));
    return buf;
}

std::string RuntimeSupervisor::make_boundary_id_locked() {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "bd-%016llx",
                  static_cast<unsigned long long>(++boundary_counter_));
    return buf;
}

std::string RuntimeSupervisor::make_operation_id_locked() {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "op-%016llx",
                  static_cast<unsigned long long>(++operation_counter_));
    return buf;
}

} // namespace cas
