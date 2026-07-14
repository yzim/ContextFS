#include "runtime_process_unsupported.h"

namespace cas {
namespace {

constexpr const char* kUnsupportedError =
    "cooperative runtime control is unsupported on this platform";

bool reject(std::string& error) {
    error = kUnsupportedError;
    return false;
}

} // namespace

bool UnsupportedRuntimeProcessController::supported(std::string& error) const {
    return reject(error);
}

bool UnsupportedRuntimeProcessController::process_alive(int64_t) {
    return false;
}

bool UnsupportedRuntimeProcessController::freeze_process_group(
    int64_t, std::string& error) {
    return reject(error);
}

bool UnsupportedRuntimeProcessController::resume_process_group(
    int64_t, std::string& error) {
    return reject(error);
}

bool UnsupportedRuntimeProcessController::terminate_process_group(
    int64_t, std::string& error) {
    return reject(error);
}

} // namespace cas
