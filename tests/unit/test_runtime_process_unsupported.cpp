#include "runtime_process_unsupported.h"

#ifdef _WIN32
#include "daemon.h"
#endif

#include <cstdio>
#include <string>

using namespace cas;

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

} // namespace

int main() {
    UnsupportedRuntimeProcessController controller;
    std::string error;

    expect(!controller.supported(error),
           "unsupported controller reports unavailable");
    expect(error == "cooperative runtime control is unsupported on this platform",
           "unsupported controller reports a stable error");
    expect(!controller.process_alive(1),
           "unsupported controller never reports a live process");

    error.clear();
    expect(!controller.freeze_process_group(1, error),
           "unsupported controller rejects freeze");
    expect(error == "cooperative runtime control is unsupported on this platform",
           "freeze reports unsupported");

    error.clear();
    expect(!controller.resume_process_group(1, error),
           "unsupported controller rejects resume");
    expect(error == "cooperative runtime control is unsupported on this platform",
           "resume reports unsupported");

    error.clear();
    expect(!controller.terminate_process_group(1, error),
           "unsupported controller rejects terminate");
    expect(error == "cooperative runtime control is unsupported on this platform",
           "terminate reports unsupported");

    RuntimeSupervisor supervisor(&controller);
    error.clear();
    expect(!supervisor.supported(error),
           "supervisor exposes controller availability");
    expect(error == "cooperative runtime control is unsupported on this platform",
           "supervisor preserves unsupported error");

    RuntimeCreateRequest request;
    request.runtime_id = "unsupported";
    error.clear();
    expect(!supervisor.register_runtime(request, error),
           "supervisor rejects runtime registration");
    expect(error == "cooperative runtime control is unsupported on this platform",
           "runtime registration reports unsupported");

#ifdef _WIN32
    Daemon daemon("unused-source", "unused-mount", "unused-store");

    RuntimeSnapshotRequest snapshot_request;
    snapshot_request.runtime_id = "unsupported";
    RuntimeSnapshotResult snapshot = daemon.snapshot_runtime(snapshot_request);
    expect(!snapshot.ok, "daemon rejects runtime snapshots");
    expect(snapshot.error ==
               "cooperative runtime control is unsupported on this platform",
           "runtime snapshot reports unsupported");

    RuntimeRestoreResult restore = daemon.restore_runtime("invalid");
    expect(!restore.ok, "daemon rejects runtime restores");
    expect(restore.error ==
               "cooperative runtime control is unsupported on this platform",
           "runtime restore reports unsupported before parsing state");
#endif

    if (failures != 0) return 1;
    std::puts("PASS test_runtime_process_unsupported");
    return 0;
}
