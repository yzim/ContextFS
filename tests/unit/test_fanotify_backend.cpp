#include "backends/fanotify_backend.h"

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/types.h>

namespace cas {
namespace fanotify_detail {

using WriteFn = ssize_t (*)(int, const void*, size_t);
bool write_full_response(int fd, const void* data, size_t len, WriteFn write_fn);

} // namespace fanotify_detail
} // namespace cas

#define CHECK(expr)                                                           \
    do {                                                                      \
        if (!(expr)) {                                                        \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,    \
                         __LINE__, #expr);                                   \
            std::abort();                                                     \
        }                                                                     \
    } while (false)

static cas::OpMask op_bit(cas::OpType op) {
    return 1u << static_cast<unsigned>(op);
}

static int g_write_calls = 0;

static ssize_t eintr_then_success_write(int, const void*, size_t len) {
    ++g_write_calls;
    if (g_write_calls == 1) {
        errno = EINTR;
        return -1;
    }
    return static_cast<ssize_t>(len);
}

static ssize_t short_write(int, const void*, size_t len) {
    ++g_write_calls;
    return static_cast<ssize_t>(len - 1);
}

static ssize_t eio_write(int, const void*, size_t) {
    ++g_write_calls;
    errno = EIO;
    return -1;
}

static void test_name_and_capabilities() {
    cas::FanotifyBackend backend;
    cas::Capabilities caps = backend.capabilities();

    CHECK(backend.name() == "fanotify");
    CHECK(caps.supported_ops & op_bit(cas::OpType::Read));
    CHECK(caps.supported_ops & op_bit(cas::OpType::Write));
    CHECK(caps.supported_ops & op_bit(cas::OpType::Open));
    CHECK(caps.supported_ops & op_bit(cas::OpType::Close));
    CHECK(caps.supported_ops & op_bit(cas::OpType::Exec));
    CHECK(!(caps.supported_ops & op_bit(cas::OpType::Unlink)));
    CHECK(!(caps.supported_ops & op_bit(cas::OpType::Rename)));
    CHECK(!caps.pre_op_verdicts);
    CHECK(!caps.requires_cgroup);
    CHECK(caps.requires_root);
}

static void test_start_with_nonexistent_mount_fails_gracefully() {
    cas::FanotifyBackend backend;
    cas::BackendConfig cfg{};
    bool callback_called = false;

    cfg.mount_path = "/nonexistent";
    bool started = backend.start(cfg, [&](cas::TelemetryEvent) {
        callback_called = true;
    });

    CHECK(!started);
    CHECK(!callback_called);
    backend.stop();
}

static void test_stop_is_idempotent() {
    cas::FanotifyBackend backend;
    backend.stop();
    backend.stop();
}

static void test_permission_response_write_retries_eintr() {
    char response[8] {};
    g_write_calls = 0;

    CHECK(cas::fanotify_detail::write_full_response(
        123, response, sizeof(response), eintr_then_success_write));
    CHECK(g_write_calls == 2);
}

static void test_permission_response_write_rejects_short_write() {
    char response[8] {};
    g_write_calls = 0;

    CHECK(!cas::fanotify_detail::write_full_response(
        123, response, sizeof(response), short_write));
    CHECK(g_write_calls == 1);
}

static void test_permission_response_write_rejects_unrecoverable_error() {
    char response[8] {};
    g_write_calls = 0;

    CHECK(!cas::fanotify_detail::write_full_response(
        123, response, sizeof(response), eio_write));
    CHECK(g_write_calls == 1);
}

// --- parse_proc_link_target tests ------------------------------------------

static void test_parse_proc_link_target_clean_path() {
    const char buf[] = "/foo/bar.txt";
    auto r = cas::fanotify_detail::parse_proc_link_target(
        buf, static_cast<ssize_t>(sizeof(buf) - 1), 4096);
    CHECK(r.status == cas::fanotify_detail::ProcLinkStatus::Ok);
    CHECK(r.path == "/foo/bar.txt");
}

static void test_parse_proc_link_target_strips_deleted_suffix() {
    const char buf[] = "/foo/bar.txt (deleted)";
    auto r = cas::fanotify_detail::parse_proc_link_target(
        buf, static_cast<ssize_t>(sizeof(buf) - 1), 4096);
    CHECK(r.status == cas::fanotify_detail::ProcLinkStatus::Deleted);
    CHECK(r.path == "/foo/bar.txt");
}

static void test_parse_proc_link_target_keeps_legitimate_deleted_substring() {
    // Path that legitimately ends with "(deleted)" but without the leading
    // single space sentinel format. Should NOT be stripped.
    const char buf[] = "/foo/bar(deleted)";
    auto r = cas::fanotify_detail::parse_proc_link_target(
        buf, static_cast<ssize_t>(sizeof(buf) - 1), 4096);
    CHECK(r.status == cas::fanotify_detail::ProcLinkStatus::Ok);
    CHECK(r.path == "/foo/bar(deleted)");
}

static void test_parse_proc_link_target_keeps_short_path() {
    // Path shorter than the 10-char " (deleted)" suffix must not be misread.
    const char buf[] = "/x";
    auto r = cas::fanotify_detail::parse_proc_link_target(
        buf, static_cast<ssize_t>(sizeof(buf) - 1), 4096);
    CHECK(r.status == cas::fanotify_detail::ProcLinkStatus::Ok);
    CHECK(r.path == "/x");
}

static void test_parse_proc_link_target_rejects_truncation() {
    // Simulate readlink returning len == buf_size (kernel truncated the result
    // because it didn't fit). The helper must flag this rather than accept
    // a partial path.
    char buf[8];
    std::memcpy(buf, "/abcdefg", 8);
    auto r = cas::fanotify_detail::parse_proc_link_target(buf, 8, 8);
    CHECK(r.status == cas::fanotify_detail::ProcLinkStatus::Truncated);
    CHECK(r.path.empty());
}

static void test_parse_proc_link_target_handles_readlink_error() {
    auto r = cas::fanotify_detail::parse_proc_link_target(nullptr, -1, 4096);
    CHECK(r.status == cas::fanotify_detail::ProcLinkStatus::Error);
    CHECK(r.path.empty());
}

static void test_parse_proc_link_target_deleted_suffix_only() {
    // Pathological case: path is *exactly* " (deleted)". Stripping leaves an
    // empty string, but the status must still be Deleted.
    const char buf[] = " (deleted)";
    auto r = cas::fanotify_detail::parse_proc_link_target(
        buf, static_cast<ssize_t>(sizeof(buf) - 1), 4096);
    CHECK(r.status == cas::fanotify_detail::ProcLinkStatus::Deleted);
    CHECK(r.path.empty());
}

int main() {
    test_name_and_capabilities();
    test_start_with_nonexistent_mount_fails_gracefully();
    test_stop_is_idempotent();
    test_permission_response_write_retries_eintr();
    test_permission_response_write_rejects_short_write();
    test_permission_response_write_rejects_unrecoverable_error();
    test_parse_proc_link_target_clean_path();
    test_parse_proc_link_target_strips_deleted_suffix();
    test_parse_proc_link_target_keeps_legitimate_deleted_substring();
    test_parse_proc_link_target_keeps_short_path();
    test_parse_proc_link_target_rejects_truncation();
    test_parse_proc_link_target_handles_readlink_error();
    test_parse_proc_link_target_deleted_suffix_only();
    std::fprintf(stderr, "test_fanotify_backend: all tests passed\n");
    return 0;
}
