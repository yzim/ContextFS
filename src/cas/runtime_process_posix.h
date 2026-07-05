#pragma once

#include "runtime_supervisor.h"

namespace cas {

class PosixRuntimeProcessController final : public RuntimeProcessController {
public:
    bool process_alive(int64_t pid) override;
    bool freeze_process_group(int64_t pgid, std::string& error) override;
    bool resume_process_group(int64_t pgid, std::string& error) override;
    bool terminate_process_group(int64_t pgid, std::string& error) override;
};

} // namespace cas
