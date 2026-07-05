#include "runtime_process_posix.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <string>
#include <unistd.h>

namespace cas {

bool PosixRuntimeProcessController::process_alive(int64_t pid) {
    if (pid <= 0) return false;
    return kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
}

bool PosixRuntimeProcessController::freeze_process_group(int64_t pgid,
                                                        std::string& error) {
    if (pgid <= 0) {
        error = "invalid process group";
        return false;
    }
    if (kill(-static_cast<pid_t>(pgid), SIGSTOP) != 0 && errno != ESRCH) {
        error = std::strerror(errno);
        return false;
    }
    error.clear();
    return true;
}

bool PosixRuntimeProcessController::resume_process_group(int64_t pgid,
                                                        std::string& error) {
    if (pgid <= 0) {
        error = "invalid process group";
        return false;
    }
    if (kill(-static_cast<pid_t>(pgid), SIGCONT) != 0 && errno != ESRCH) {
        error = std::strerror(errno);
        return false;
    }
    error.clear();
    return true;
}

bool PosixRuntimeProcessController::terminate_process_group(int64_t pgid,
                                                           std::string& error) {
    if (pgid <= 0) {
        error = "invalid process group";
        return false;
    }
    pid_t target = -static_cast<pid_t>(pgid);
    if (kill(target, SIGKILL) != 0 && errno != ESRCH) {
        error = std::strerror(errno);
        return false;
    }
    error.clear();
    return true;
}

} // namespace cas
