// Cooperative runtime client.
//
// This translation unit compiles into the TARGET program (linked via the
// `runtime_client.cpp` source, see tests/fixtures/runtime_counter_fixture.cpp
// and Task 6's system test). It speaks the newline-delimited cooperative
// control protocol defined by cas::control_protocol over the daemon's Unix
// domain control socket (AGENTVFS_SOCK).
//
// agentvfs_runtime_boundary() is the quiescent-boundary hook the target calls
// when it is safe to fork a template. On a "snapshot" verdict it forks the
// active runtime; the forked child parks as a template and later forks
// restored grandchildren. See the fork-loop diagram in the report and the
// task-5 brief for the exact parent/child/grandchild return contract.

#include "runtime_client.h"

#include "runtime_io.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>

namespace {

// In-memory generation. Initialized once from AGENTVFS_RUNTIME_GENERATION at
// process load, bumped in the restored grandchild before it resumes the
// program. A plain namespace-scope variable: fork copies it into children, so
// the restored grandchild can mutate its private copy and the next boundary
// call in that process reports the new generation.
uint64_t load_initial_generation() {
    const char* e = std::getenv("AGENTVFS_RUNTIME_GENERATION");
    if (!e || !*e) return 0ULL;
    char* end = nullptr;
    unsigned long long v = std::strtoull(e, &end, 10);
    if (end == e) return 0ULL;
    return static_cast<uint64_t>(v);
}

uint64_t g_current_generation = load_initial_generation();

std::string extract_str(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto p = json.find(needle);
    if (p == std::string::npos) return {};
    p = json.find(':', p + needle.size());
    if (p == std::string::npos) return {};
    p = json.find('"', p);
    if (p == std::string::npos) return {};
    auto end = json.find('"', p + 1);
    if (end == std::string::npos) return {};
    return json.substr(p + 1, end - p - 1);
}

bool extract_uint64(const std::string& json, const std::string& key,
                    uint64_t& out) {
    std::string needle = "\"" + key + "\"";
    auto p = json.find(needle);
    if (p == std::string::npos) return false;
    p = json.find(':', p + needle.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
    char* end = nullptr;
    unsigned long long v = std::strtoull(json.c_str() + p, &end, 10);
    if (end == json.c_str() + p) return false;
    out = static_cast<uint64_t>(v);
    return true;
}

void set_error(char* error, size_t error_len, const char* msg) {
    if (error && error_len) {
        std::snprintf(error, error_len, "%s", msg);
    }
}

} // namespace

extern "C" uint64_t agentvfs_runtime_current_generation(void) {
    return g_current_generation;
}

extern "C" int agentvfs_runtime_boundary(const char* boundary_kind,
                                         char* error,
                                         size_t error_len) {
    const char* sock = std::getenv("AGENTVFS_SOCK");
    const char* rid = std::getenv("AGENTVFS_RUNTIME_ID");
    const char* token_env = std::getenv("AGENTVFS_RUNTIME_TOKEN");
    if (!sock || !*sock || !rid || !*rid || !token_env || !*token_env) {
        set_error(error, error_len,
                  "AGENTVFS_SOCK/AGENTVFS_RUNTIME_ID/AGENTVFS_RUNTIME_TOKEN not set");
        return -1;
    }

    const std::string kind = boundary_kind ? boundary_kind : "";
    const std::string runtime_id = rid;
    const std::string token = token_env;
    const std::string sock_path = sock;

    // 1. Announce the boundary. The daemon blocks this response server-side
    //    until a matching snapshot releases it (or it decides we continue).
    char pidbuf[32];
    std::snprintf(pidbuf, sizeof(pidbuf), "%ld", static_cast<long>(getpid()));
    char genbuf[32];
    std::snprintf(genbuf, sizeof(genbuf), "%llu",
                  static_cast<unsigned long long>(g_current_generation));

    std::string req = "runtime.boundary {\"runtime_id\":\"" +
                      cas::runtime_io::json_escape(runtime_id) +
                      "\",\"control_token\":\"" + cas::runtime_io::json_escape(token) +
                      "\",\"pid\":" + pidbuf +
                      ",\"generation\":" + genbuf +
                      ",\"boundary_kind\":\"" + cas::runtime_io::json_escape(kind) + "\"}";

    std::string resp;
    std::string err;
    if (!cas::runtime_io::control_request(sock_path, req, resp, err)) {
        set_error(error, error_len,
                  ("runtime.boundary transport: " + err).c_str());
        return -1;
    }

    if (!cas::runtime_io::extract_bool(resp, "ok", false)) {
        std::string e = extract_str(resp, "error");
        if (e.empty()) e = resp;
        set_error(error, error_len, ("runtime.boundary: " + e).c_str());
        return -1;
    }

    std::string action = extract_str(resp, "action");
    if (action == "continue") {
        // No snapshot pending; resume the program normally.
        return 0;
    }
    if (action != "snapshot") {
        set_error(error, error_len,
                  ("runtime.boundary: unknown action '" + action + "'").c_str());
        return -1;
    }

    // 2. action == "snapshot": fork a parked template.
    std::string template_id = extract_str(resp, "template_id");
    if (template_id.empty()) {
        set_error(error, error_len,
                  "runtime.boundary: snapshot response missing template_id");
        return -1;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        set_error(error, error_len,
                  ("pipe: " + std::string(std::strerror(errno))).c_str());
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        set_error(error, error_len,
                  ("fork: " + std::string(std::strerror(errno))).c_str());
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid > 0) {
        // ---- Parent: the ACTIVE runtime. It will be retired on restore.
        // Wait for the parked child to finish its runtime.template.ready
        // round-trip before continuing. The child signals "done" simply by
        // closing its write end, so read() until EOF. The old protocol carried
        // a 1-byte status that was discarded here anyway (the active runtime
        // returns 0 whether or not the child parked successfully), so EOF-only
        // signaling is equivalent and simpler. The synchronization invariant
        // is preserved: EOF cannot arrive until the child has completed its
        // template.ready round-trip (the close happens after it), so the daemon
        // has the template registered before the parent returns.
        close(pipefd[1]);
        while (true) {
            char sink;
            ssize_t n = read(pipefd[0], &sink, 1);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (n == 0) break;  // EOF: child finished parking and closed
        }
        close(pipefd[0]);
        return 0;
    }

    // ---- Child: the PARKED TEMPLATE. MUST NOT return into the program.
    // It parks in the poll loop until drop/exit.
    close(pipefd[0]);

    // New process group so generation_ready's retirement of the old active
    // pgid does not kill this parked template.
    setpgid(0, 0);
    const long my_pid = static_cast<long>(getpid());
    const long my_pgid = static_cast<long>(getpgid(0));

    // 3. Publish this template. Blocks until the daemon publishes the union
    //    state (or fails).
    char tpidbuf[32];
    std::snprintf(tpidbuf, sizeof(tpidbuf), "%ld", my_pid);
    char tpgidbuf[32];
    std::snprintf(tpgidbuf, sizeof(tpgidbuf), "%ld", my_pgid);

    std::string ready_req =
        "runtime.template.ready {\"runtime_id\":\"" +
        cas::runtime_io::json_escape(runtime_id) +
        "\",\"control_token\":\"" + cas::runtime_io::json_escape(token) +
        "\",\"template_id\":\"" + cas::runtime_io::json_escape(template_id) +
        "\",\"template_pid\":" + tpidbuf +
        ",\"template_process_group_id\":" + tpgidbuf +
        ",\"generation\":" + genbuf + "}";

    std::string rresp;
    std::string rerr;
    bool ready_ok = cas::runtime_io::control_request(sock_path, ready_req, rresp, rerr) &&
                    cas::runtime_io::extract_bool(rresp, "ok", false);

    // Signal the parent that the template.ready round-trip is complete by
    // closing the write end (EOF). The parent does not consume a status byte;
    // it only waits for this close so it does not continue before the daemon
    // has the template registered.
    close(pipefd[1]);

    if (!ready_ok) {
        // Could not register the template; nothing useful to do but exit.
        _exit(1);
    }

    // 4. Polling loop. Same template can restore more than once until drop.
    const std::string poll_req =
        "runtime.template.poll {\"template_id\":\"" +
        cas::runtime_io::json_escape(template_id) +
        "\",\"control_token\":\"" + cas::runtime_io::json_escape(token) + "\"}";

    // The parked template outlives the active runtime and is normally retired
    // by an explicit runtime.drop (action:"drop"). If the daemon exits first
    // (crash, or the harness kills it), no drop ever arrives: without a bound
    // we would connect-fail in a tight 5 ms loop forever, burning a core and
    // leaking the process. Give up after a sustained failure window; a live
    // daemon answers in well under one poll interval, so ~1 s of consecutive
    // hard failures (connect refused / socket gone) means it is gone for good.
    constexpr int kPollGiveUpFailures = 200;
    int consecutive_failures = 0;

    while (true) {
        std::string presp;
        std::string perr;
        if (!cas::runtime_io::control_request(sock_path, poll_req, presp, perr)) {
            // Transport failure (daemon gone / connect refused): bound the spin
            // so we do not loop forever waiting for a daemon that will not return.
            if (++consecutive_failures >= kPollGiveUpFailures) {
                _exit(0);
            }
            usleep(5000);
            continue;
        }
        if (!cas::runtime_io::extract_bool(presp, "ok", false)) {
            // ok:false (e.g. "unknown template" if the record was reaped before
            // the drop flag was observed, or another transient server error).
            // The normal drop contract is delivered as action:"drop" with
            // ok:true; this guards rarer ok:false spins by counting them
            // against the same ~1s give-up window as transport failures, so the
            // parked template cannot loop at ~200Hz indefinitely.
            if (++consecutive_failures >= kPollGiveUpFailures) {
                _exit(0);
            }
            usleep(5000);
            continue;
        }
        // A well-formed ok:true poll resets the sustained-failure window.
        consecutive_failures = 0;

        std::string paction = extract_str(presp, "action");
        if (paction == "wait") {
            usleep(5000);
            continue;
        }
        if (paction == "drop") {
            _exit(0);
        }
        if (paction == "restore") {
            uint64_t target = 0;
            if (!extract_uint64(presp, "target_generation", target)) {
                usleep(5000);
                continue;
            }

            pid_t gpid = fork();
            if (gpid < 0) {
                // Fork failure (ENOMEM/EAGAIN): the supervisor already marked
                // this restore intent in-flight, so the next poll returns
                // "wait" and we will not retry the fork here. The daemon's
                // wait_for_generation_ready will time out and abort_restore
                // clears the intent, letting a later restore try again.
                usleep(5000);
                continue;
            }

            if (gpid == 0) {
                // ---- Restored GRANDCHILD: resumes the program with the
                // snapshotted memory (counter restored) and the rolled-back
                // filesystem. It is the ONLY process that returns from
                // agentvfs_runtime_boundary() a second time.
                setpgid(0, 0);  // own pgid; survive old-active retirement
                const long g_pid = static_cast<long>(getpid());
                const long g_pgid = static_cast<long>(getpgid(0));

                g_current_generation = target;

                char genvbuf[32];
                std::snprintf(genvbuf, sizeof(genvbuf), "%llu",
                              static_cast<unsigned long long>(target));
                setenv("AGENTVFS_RUNTIME_GENERATION", genvbuf, 1);

                char gpidbuf2[32];
                std::snprintf(gpidbuf2, sizeof(gpidbuf2), "%ld", g_pid);
                char gpgidbuf2[32];
                std::snprintf(gpgidbuf2, sizeof(gpgidbuf2), "%ld", g_pgid);
                char gtgtbuf[32];
                std::snprintf(gtgtbuf, sizeof(gtgtbuf), "%llu",
                              static_cast<unsigned long long>(target));

                std::string greq =
                    "runtime.generation.ready {\"runtime_id\":\"" +
                    cas::runtime_io::json_escape(runtime_id) +
                    "\",\"control_token\":\"" + cas::runtime_io::json_escape(token) +
                    "\",\"pid\":" + gpidbuf2 +
                    ",\"active_process_group_id\":" + gpgidbuf2 +
                    ",\"generation\":" + gtgtbuf + "}";
                std::string gresp;
                std::string gerr;
                // The daemon's generation.ready unblocks restore_runtime. We
                // MUST surface a non-ok ack: if the daemon rejects it (e.g.
                // "unknown runtime") while we proceed with the bumped
                // generation, the daemon's wait_for_generation_ready would
                // time out and roll back, diverging silently. The grandchild
                // still returns from the boundary (it has restored memory and
                // must continue the program), but the boundary call reports
                // -1 so the failure is observable.
                bool gen_ok = cas::runtime_io::control_request(sock_path, greq, gresp, gerr) &&
                              cas::runtime_io::extract_bool(gresp, "ok", false);
                if (!gen_ok) {
                    // A non-ok ack means the daemon rejected this generation
                    // (e.g. the restore was aborted/timed out and the supervisor
                    // cleared the pending intent, so generation_ready returned
                    // "restore aborted or stale"). We are an orphaned grandchild
                    // with restored memory but NO claim on the runtime: the old
                    // active process was SIGCONT'd by abort_restore and still
                    // owns it. We must DIE here, not resume the program as a
                    // second active process (single-active invariant). Returning
                    // would unwind into the program a second time.
                    std::fprintf(stderr,
                        "agentvfs-runtime: generation.ready rejected for runtime %s generation %llu (restore aborted/stale); exiting grandchild: %s\n",
                        runtime_id.c_str(),
                        static_cast<unsigned long long>(target),
                        gresp.c_str());
                    _exit(1);
                }

                return 0;
            }

            // Parked template child: keep polling so the same template can
            // restore again until the daemon drops it.
            continue;
        }

        // Unknown action: back off and retry.
        usleep(5000);
    }
}
