#pragma once

#include "runtime_supervisor.h"

namespace cas {

// Used on platforms where cooperative runtime snapshots have no process-group
// implementation. It keeps the daemon buildable while rejecting runtime
// control instead of pretending that signals or liveness checks succeeded.
class UnsupportedRuntimeProcessController final : public RuntimeProcessController {
public:
    bool supported(std::string& error) const override;
    bool process_alive(int64_t pid) override;
    bool freeze_process_group(int64_t pgid, std::string& error) override;
    bool resume_process_group(int64_t pgid, std::string& error) override;
    bool terminate_process_group(int64_t pgid, std::string& error) override;
};

} // namespace cas
