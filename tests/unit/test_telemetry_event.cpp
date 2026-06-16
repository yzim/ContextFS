#include "telemetry_event.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

static void test_event_to_json_basic() {
    cas::TelemetryEvent ev{};
    ev.timestamp_ns = 1000000;
    ev.session_id = 42;
    ev.branch_id = 1;
    ev.policy_version = 3;
    ev.op = cas::OpType::Read;
    ev.verdict = cas::Verdict::Allow;
    ev.dev = 8;
    ev.ino = 12345;
    ev.i_generation = 1;
    ev.path = "/foo/bar.txt";
    ev.pid = 100;
    ev.uid = 1000;
    ev.gid = 1000;
    ev.bytes = 4096;
    ev.latency_ns = 500;
    ev.backend = "ebpf";

    std::string json = cas::event_to_json(ev);
    const std::string expected =
        R"json({"ts":1000000,"session_id":42,"branch_id":1,"policy_version":3,"op":"read","verdict":"allow","dev":8,"ino":12345,"i_generation":1,"path":"/foo/bar.txt","pid":100,"uid":1000,"gid":1000,"bytes":4096,"latency_ns":500,"backend":"ebpf"})json";
    assert(json == expected);
    assert(json.find("\"extra\"") == std::string::npos);
}

static void test_event_to_json_with_extra() {
    cas::TelemetryEvent ev{};
    ev.op = cas::OpType::Write;
    ev.verdict = cas::Verdict::SoftWatch;
    ev.backend = "ptrace";
    ev.extra = {{"syscall_nr", "1"}, {"ret", "4096"}};

    std::string json = cas::event_to_json(ev);
    const std::string expected =
        R"json({"ts":0,"session_id":0,"branch_id":0,"policy_version":0,"op":"write","verdict":"soft_watch","dev":0,"ino":0,"i_generation":0,"path":"","pid":0,"uid":0,"gid":0,"bytes":0,"latency_ns":0,"backend":"ptrace","extra":{"syscall_nr":"1","ret":"4096"}})json";
    assert(json == expected);
}

static void test_event_to_json_deny_verdict() {
    cas::TelemetryEvent ev{};
    ev.op = cas::OpType::Unlink;
    ev.verdict = cas::Verdict::Deny;
    ev.backend = "fanotify";

    std::string json = cas::event_to_json(ev);
    assert(json.find("\"verdict\":\"deny\"") != std::string::npos);
    assert(json.find("\"op\":\"unlink\"") != std::string::npos);
}

static void test_event_to_json_escapes_strings() {
    cas::TelemetryEvent ev{};
    ev.path = "/tmp/quote\"/slash\\\nname";
    ev.backend = "ptrace\"\\\nbackend";
    ev.extra = {{"key\"\\\n", "value\"\\\n"}};

    std::string json = cas::event_to_json(ev);
    const std::string expected =
        R"json({"ts":0,"session_id":0,"branch_id":0,"policy_version":0,"op":"read","verdict":"allow","dev":0,"ino":0,"i_generation":0,"path":"/tmp/quote\"/slash\\\nname","pid":0,"uid":0,"gid":0,"bytes":0,"latency_ns":0,"backend":"ptrace\"\\\nbackend","extra":{"key\"\\\n":"value\"\\\n"}})json";
    assert(json == expected);
    assert(json.find('\n') == std::string::npos);
}

static void test_optype_to_string_all() {
    assert(std::strcmp(cas::optype_to_string(cas::OpType::Read), "read") == 0);
    assert(std::strcmp(cas::optype_to_string(cas::OpType::Write), "write") == 0);
    assert(std::strcmp(cas::optype_to_string(cas::OpType::Open), "open") == 0);
    assert(std::strcmp(cas::optype_to_string(cas::OpType::Close), "close") == 0);
    assert(std::strcmp(cas::optype_to_string(cas::OpType::Unlink), "unlink") == 0);
    assert(std::strcmp(cas::optype_to_string(cas::OpType::Rename), "rename") == 0);
    assert(std::strcmp(cas::optype_to_string(cas::OpType::Truncate), "truncate") == 0);
    assert(std::strcmp(cas::optype_to_string(cas::OpType::Stat), "stat") == 0);
    assert(std::strcmp(cas::optype_to_string(cas::OpType::Exec), "exec") == 0);
    assert(std::strcmp(cas::optype_to_string(cas::OpType::Create), "create") == 0);
}

static void test_verdict_to_string_all() {
    assert(std::strcmp(cas::verdict_to_string(cas::Verdict::Allow), "allow") == 0);
    assert(std::strcmp(cas::verdict_to_string(cas::Verdict::SoftWatch), "soft_watch") == 0);
    assert(std::strcmp(cas::verdict_to_string(cas::Verdict::Deny), "deny") == 0);
}

int main() {
    test_event_to_json_basic();
    test_event_to_json_with_extra();
    test_event_to_json_deny_verdict();
    test_event_to_json_escapes_strings();
    test_optype_to_string_all();
    test_verdict_to_string_all();
    std::fprintf(stderr, "test_telemetry_event: all tests passed\n");
    return 0;
}
