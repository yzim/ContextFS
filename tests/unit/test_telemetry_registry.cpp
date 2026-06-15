#include "telemetry_registry.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#define CHECK(expr)                                                           \
    do {                                                                      \
        if (!(expr)) {                                                        \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,    \
                         __LINE__, #expr);                                   \
            std::abort();                                                     \
        }                                                                     \
    } while (false)

class FakeBackend : public cas::TelemetryBackend {
public:
    std::string name_;
    cas::Capabilities caps_;
    cas::EventCallback cb_;
    bool started_ = false;
    int starts_ = 0;
    int stops_ = 0;
    int sessions_registered_ = 0;
    int sessions_unregistered_ = 0;
    int policies_installed_ = 0;
    bool start_result_ = true;
    bool register_result_ = true;
    bool unregister_result_ = true;
    std::function<void(FakeBackend&)> on_start_;
    std::function<void(FakeBackend&)> on_stop_;

    explicit FakeBackend(std::string name, cas::Capabilities caps)
        : name_(std::move(name)), caps_(caps) {}

    std::string name() const override { return name_; }

    bool start(const cas::BackendConfig&, cas::EventCallback cb) override {
        cb_ = std::move(cb);
        started_ = true;
        ++starts_;
        if (on_start_) {
            on_start_(*this);
        }
        return start_result_;
    }

    void stop() override {
        ++stops_;
        if (on_stop_) {
            on_stop_(*this);
        }
        started_ = false;
    }

    bool register_session(const cas::SessionInfo&) override {
        ++sessions_registered_;
        return register_result_;
    }

    bool unregister_session(const std::string&) override {
        ++sessions_unregistered_;
        --sessions_registered_;
        return unregister_result_;
    }

    bool install_policy(const cas::PolicyRules&) override {
        ++policies_installed_;
        return true;
    }

    cas::Capabilities capabilities() const override { return caps_; }

    void emit(cas::TelemetryEvent ev) {
        if (cb_) {
            cb_(std::move(ev));
        }
    }
};

class ProcessorBackend : public FakeBackend {
public:
    using Handler = std::function<cas::TelemetryEvent(const cas::TelemetryEvent&)>;

    explicit ProcessorBackend(std::string name, Handler handler = nullptr)
        : FakeBackend(std::move(name), cas::Capabilities{}),
          handler_(std::move(handler)) {}

    cas::TelemetryEvent process_event(const cas::TelemetryEvent& ev) override {
        ++processed_;
        if (handler_) {
            return handler_(ev);
        }
        cas::TelemetryEvent next = ev;
        next.verdict = cas::Verdict::SoftWatch;
        next.extra.emplace_back("processed_by", name_);
        return next;
    }

private:
    Handler handler_;
    int processed_ = 0;

public:
    int processed() const { return processed_; }
};

// Processor that throws from process_event(); used to verify the registry
// catches exceptions and skips the throwing processor without aborting the
// fanout chain.
class ThrowingProcessor : public FakeBackend {
public:
    enum class ThrowKind { Std, Unknown };

    explicit ThrowingProcessor(std::string name, ThrowKind kind = ThrowKind::Std)
        : FakeBackend(std::move(name), cas::Capabilities{}), kind_(kind) {}

    cas::TelemetryEvent process_event(const cas::TelemetryEvent& ev) override {
        ++processed_;
        if (kind_ == ThrowKind::Std) {
            throw std::runtime_error("processor exploded");
        }
        // Throw something that is not derived from std::exception so we can
        // exercise the catch(...) branch.
        throw 42;
        return ev;  // unreachable
    }

    int processed() const { return processed_; }

private:
    ThrowKind kind_;
    int processed_ = 0;
};

static cas::Capabilities source_caps() {
    cas::Capabilities caps{};
    caps.supported_ops = 1u << static_cast<unsigned>(cas::OpType::Read);
    return caps;
}

static cas::Capabilities processor_caps() {
    return cas::Capabilities{};
}

static FakeBackend* add_fake(cas::TelemetryRegistry& registry, std::string name,
                             cas::Capabilities caps) {
    auto backend = std::make_unique<FakeBackend>(std::move(name), caps);
    FakeBackend* raw = backend.get();
    registry.add(std::move(backend));
    return raw;
}

static ProcessorBackend* add_processor(cas::TelemetryRegistry& registry,
                                       std::string name,
                                       ProcessorBackend::Handler handler = nullptr) {
    auto backend = std::make_unique<ProcessorBackend>(std::move(name),
                                                      std::move(handler));
    ProcessorBackend* raw = backend.get();
    registry.add(std::move(backend));
    return raw;
}

static void test_fanout_lifecycle_calls() {
    std::vector<cas::TelemetryEvent> drained;
    cas::TelemetryRegistry registry([&](cas::TelemetryEvent ev) {
        drained.push_back(std::move(ev));
    });

    FakeBackend* source = add_fake(registry, "source", source_caps());
    FakeBackend* processor = add_fake(registry, "processor", processor_caps());

    cas::BackendConfig cfg{};
    registry.start_all(cfg);
    CHECK(source->started_);
    CHECK(processor->started_);

    cas::SessionInfo session{};
    session.cgroup_path = "/sys/fs/cgroup/session";
    registry.register_session(session);
    CHECK(source->sessions_registered_ == 1);
    CHECK(processor->sessions_registered_ == 1);

    cas::PolicyRules rules{};
    rules.rules.push_back({"/tmp/*", 1u << static_cast<unsigned>(cas::OpType::Read)});
    registry.install_policy(rules);
    CHECK(source->policies_installed_ == 1);
    CHECK(processor->policies_installed_ == 1);

    registry.unregister_session(session.cgroup_path);
    CHECK(source->sessions_registered_ == 0);
    CHECK(processor->sessions_registered_ == 0);

    registry.stop_all();
    CHECK(!source->started_);
    CHECK(!processor->started_);
    CHECK(drained.empty());
}

static void test_source_events_reach_drain() {
    std::vector<cas::TelemetryEvent> drained;
    cas::TelemetryRegistry registry([&](cas::TelemetryEvent ev) {
        drained.push_back(std::move(ev));
    });

    FakeBackend* source = add_fake(registry, "source", source_caps());
    registry.start_all(cas::BackendConfig{});

    cas::TelemetryEvent ev{};
    ev.path = "/watched";
    ev.backend = "source";
    source->emit(ev);

    CHECK(drained.size() == 1);
    CHECK(drained[0].path == "/watched");
    CHECK(drained[0].backend == "source");
}

static void test_merge_verdicts() {
    CHECK(cas::TelemetryRegistry::merge_verdicts({}) == cas::Verdict::Allow);
    CHECK(cas::TelemetryRegistry::merge_verdicts(
               {cas::Verdict::Allow, cas::Verdict::Allow}) ==
           cas::Verdict::Allow);
    CHECK(cas::TelemetryRegistry::merge_verdicts(
               {cas::Verdict::Allow, cas::Verdict::SoftWatch}) ==
           cas::Verdict::SoftWatch);
    CHECK(cas::TelemetryRegistry::merge_verdicts(
               {cas::Verdict::SoftWatch, cas::Verdict::Deny}) ==
           cas::Verdict::Deny);
    CHECK(cas::TelemetryRegistry::merge_verdicts(
               {cas::Verdict::Allow, cas::Verdict::Deny,
                cas::Verdict::SoftWatch}) == cas::Verdict::Deny);
}

static void test_processors_route_source_events_before_drain() {
    std::vector<cas::TelemetryEvent> drained;
    cas::TelemetryRegistry registry([&](cas::TelemetryEvent ev) {
        drained.push_back(std::move(ev));
    });

    FakeBackend* source = add_fake(registry, "source", source_caps());
    add_processor(registry, "processor");
    registry.start_all(cas::BackendConfig{});

    cas::TelemetryEvent ev{};
    ev.path = "/needs-processing";
    ev.verdict = cas::Verdict::Allow;
    source->emit(ev);

    CHECK(drained.size() == 1);
    CHECK(drained[0].path == "/needs-processing");
    CHECK(drained[0].verdict == cas::Verdict::SoftWatch);
    CHECK(drained[0].extra.size() == 1);
    CHECK(drained[0].extra[0].first == "processed_by");
    CHECK(drained[0].extra[0].second == "processor");
}

static void test_processor_verdicts_merge_most_restrictive_before_drain() {
    std::vector<cas::TelemetryEvent> drained;
    cas::TelemetryRegistry registry([&](cas::TelemetryEvent ev) {
        drained.push_back(std::move(ev));
    });

    FakeBackend* source = add_fake(registry, "source", source_caps());
    add_processor(registry, "deny", [](const cas::TelemetryEvent& ev) {
        cas::TelemetryEvent next = ev;
        next.verdict = cas::Verdict::Deny;
        next.extra.emplace_back("marker", "deny");
        return next;
    });
    add_processor(registry, "allow", [](const cas::TelemetryEvent& ev) {
        cas::TelemetryEvent next = ev;
        next.verdict = cas::Verdict::Allow;
        next.extra.emplace_back("marker", "allow");
        return next;
    });
    registry.start_all(cas::BackendConfig{});

    cas::TelemetryEvent ev{};
    ev.path = "/deny-must-win";
    ev.verdict = cas::Verdict::Allow;
    source->emit(ev);

    CHECK(drained.size() == 1);
    CHECK(drained[0].verdict == cas::Verdict::Deny);
    CHECK(drained[0].extra.size() == 2);
    CHECK(drained[0].extra[0].second == "deny");
    CHECK(drained[0].extra[1].second == "allow");
}

static void test_processors_run_in_registration_order() {
    std::vector<cas::TelemetryEvent> drained;
    cas::TelemetryRegistry registry([&](cas::TelemetryEvent ev) {
        drained.push_back(std::move(ev));
    });

    FakeBackend* source = add_fake(registry, "source", source_caps());
    add_processor(registry, "first", [](const cas::TelemetryEvent& ev) {
        cas::TelemetryEvent next = ev;
        next.extra.emplace_back("marker", "first");
        return next;
    });
    add_processor(registry, "second", [](const cas::TelemetryEvent& ev) {
        cas::TelemetryEvent next = ev;
        next.extra.emplace_back("marker", "second");
        return next;
    });
    registry.start_all(cas::BackendConfig{});

    source->emit(cas::TelemetryEvent{});

    CHECK(drained.size() == 1);
    CHECK(drained[0].extra.size() == 2);
    CHECK(drained[0].extra[0].second == "first");
    CHECK(drained[0].extra[1].second == "second");
}

static void test_drain_callback_can_register_session() {
    cas::TelemetryRegistry* registry_ptr = nullptr;
    int drained = 0;
    cas::TelemetryRegistry registry([&](cas::TelemetryEvent) {
        ++drained;
        cas::SessionInfo session{};
        session.cgroup_path = "/sys/fs/cgroup/reentrant";
        registry_ptr->register_session(session);
    });
    registry_ptr = &registry;

    FakeBackend* source = add_fake(registry, "source", source_caps());
    FakeBackend* processor = add_fake(registry, "processor", processor_caps());
    registry.start_all(cas::BackendConfig{});

    source->emit(cas::TelemetryEvent{});

    CHECK(drained == 1);
    CHECK(source->sessions_registered_ == 1);
    CHECK(processor->sessions_registered_ == 1);
}

static void test_register_session_aggregates_backend_failures() {
    cas::TelemetryRegistry registry(nullptr);
    FakeBackend* first = add_fake(registry, "first", source_caps());
    FakeBackend* second = add_fake(registry, "second", processor_caps());
    second->register_result_ = false;

    cas::SessionInfo session{};
    CHECK(!registry.register_session(session));
    CHECK(first->sessions_registered_ == 1);
    CHECK(second->sessions_registered_ == 1);
}

static void test_unregister_session_aggregates_backend_failures() {
    cas::TelemetryRegistry registry(nullptr);
    FakeBackend* first = add_fake(registry, "first", source_caps());
    FakeBackend* second = add_fake(registry, "second", processor_caps());
    first->sessions_registered_ = 1;
    second->sessions_registered_ = 1;
    second->unregister_result_ = false;

    CHECK(!registry.unregister_session("/sys/fs/cgroup/session"));
    CHECK(first->sessions_registered_ == 0);
    CHECK(second->sessions_registered_ == 0);
}

static void test_add_nullptr_is_ignored() {
    cas::TelemetryRegistry registry(nullptr);

    registry.add(nullptr);

    CHECK(registry.backends().empty());
}

static void test_start_all_starts_processors_before_eager_sources() {
    bool processor_was_started = false;
    cas::TelemetryRegistry registry(nullptr);

    FakeBackend* source = add_fake(registry, "source", source_caps());
    source->on_start_ = [](FakeBackend& backend) {
        backend.emit(cas::TelemetryEvent{});
    };

    ProcessorBackend* processor = nullptr;
    processor = add_processor(registry, "processor",
                              [&](const cas::TelemetryEvent& ev) {
                                  processor_was_started = processor->started_;
                                  return ev;
                              });

    registry.start_all(cas::BackendConfig{});

    CHECK(processor_was_started);
}

static void test_stop_all_stops_sources_before_processors() {
    bool processor_was_started = false;
    cas::TelemetryRegistry registry(nullptr);

    ProcessorBackend* processor = nullptr;
    processor = add_processor(registry, "processor",
                              [&](const cas::TelemetryEvent& ev) {
                                  processor_was_started = processor->started_;
                                  return ev;
                              });
    FakeBackend* source = add_fake(registry, "source", source_caps());
    source->on_stop_ = [](FakeBackend& backend) {
        backend.emit(cas::TelemetryEvent{});
    };

    registry.start_all(cas::BackendConfig{});
    registry.stop_all();

    CHECK(processor_was_started);
}

static void test_start_stop_all_are_idempotent() {
    cas::TelemetryRegistry registry(nullptr);
    FakeBackend* source = add_fake(registry, "source", source_caps());
    FakeBackend* processor = add_fake(registry, "processor", processor_caps());

    registry.start_all(cas::BackendConfig{});
    registry.start_all(cas::BackendConfig{});
    registry.stop_all();
    registry.stop_all();

    CHECK(source->starts_ == 1);
    CHECK(processor->starts_ == 1);
    CHECK(source->stops_ == 1);
    CHECK(processor->stops_ == 1);
}

static void test_failed_start_is_reported_and_excluded_from_activity() {
    std::vector<cas::TelemetryEvent> drained;
    cas::TelemetryRegistry registry([&](cas::TelemetryEvent ev) {
        drained.push_back(std::move(ev));
    });

    FakeBackend* failed_source =
        add_fake(registry, "failed-source", source_caps());
    failed_source->start_result_ = false;
    FakeBackend* active_source =
        add_fake(registry, "active-source", source_caps());
    ProcessorBackend* failed_processor =
        add_processor(registry, "failed-processor");
    failed_processor->start_result_ = false;
    ProcessorBackend* active_processor =
        add_processor(registry, "active-processor");

    registry.start_all(cas::BackendConfig{});

    std::vector<cas::TelemetryRegistry::BackendRuntimeStatus> statuses =
        registry.backend_statuses();
    CHECK(statuses.size() == 4);
    CHECK(statuses[0].name == "failed-source");
    CHECK(!statuses[0].started);
    CHECK(statuses[0].status == "start failed");
    CHECK(statuses[1].name == "active-source");
    CHECK(statuses[1].started);
    CHECK(statuses[1].status == "started");
    CHECK(statuses[2].name == "failed-processor");
    CHECK(!statuses[2].started);
    CHECK(statuses[2].status == "start failed");
    CHECK(statuses[3].name == "active-processor");
    CHECK(statuses[3].started);

    cas::TelemetryEvent failed_ev{};
    failed_ev.path = "/failed-source-event";
    failed_ev.backend = "failed-source";
    failed_source->emit(failed_ev);
    CHECK(drained.empty());

    cas::TelemetryEvent active_ev{};
    active_ev.path = "/active-source-event";
    active_ev.backend = "active-source";
    active_source->emit(active_ev);
    CHECK(drained.size() == 1);
    CHECK(drained[0].path == "/active-source-event");
    CHECK(active_processor->processed() == 1);
    CHECK(failed_processor->processed() == 0);

    cas::SessionInfo session{};
    session.cgroup_path = "/sys/fs/cgroup/session";
    CHECK(registry.register_session(session));
    CHECK(failed_source->sessions_registered_ == 0);
    CHECK(active_source->sessions_registered_ == 1);
    CHECK(failed_processor->sessions_registered_ == 0);
    CHECK(active_processor->sessions_registered_ == 1);

    cas::PolicyRules rules{};
    rules.rules.push_back({"/tmp/*", 1u << static_cast<unsigned>(cas::OpType::Write)});
    registry.install_policy(rules);
    CHECK(failed_source->policies_installed_ == 0);
    CHECK(active_source->policies_installed_ == 1);
    CHECK(failed_processor->policies_installed_ == 0);
    CHECK(active_processor->policies_installed_ == 1);

    CHECK(registry.unregister_session(session.cgroup_path));
    CHECK(failed_source->sessions_unregistered_ == 0);
    CHECK(active_source->sessions_unregistered_ == 1);
    CHECK(failed_processor->sessions_unregistered_ == 0);
    CHECK(active_processor->sessions_unregistered_ == 1);

    registry.stop_all();
    CHECK(failed_source->stops_ == 1);
    CHECK(active_source->stops_ == 1);
    CHECK(failed_processor->stops_ == 1);
    CHECK(active_processor->stops_ == 1);
}

// A throwing processor must not poison the registry: the drain still
// receives the event (with the throwing processor's mutation skipped), and
// subsequent events keep flowing.
static void test_throwing_processor_is_skipped_not_fatal() {
    std::vector<cas::TelemetryEvent> drained;
    cas::TelemetryRegistry registry([&](cas::TelemetryEvent ev) {
        drained.push_back(std::move(ev));
    });

    FakeBackend* source = add_fake(registry, "source", source_caps());
    auto thrower_owned = std::make_unique<ThrowingProcessor>("thrower");
    ThrowingProcessor* thrower = thrower_owned.get();
    registry.add(std::move(thrower_owned));
    auto downstream_owned = std::make_unique<ProcessorBackend>(
        "downstream", [](const cas::TelemetryEvent& ev) {
            cas::TelemetryEvent next = ev;
            next.extra.emplace_back("marker", "downstream");
            return next;
        });
    ProcessorBackend* downstream = downstream_owned.get();
    registry.add(std::move(downstream_owned));

    registry.start_all(cas::BackendConfig{});

    cas::TelemetryEvent ev{};
    ev.path = "/first";
    ev.verdict = cas::Verdict::Allow;
    source->emit(ev);

    CHECK(drained.size() == 1);
    CHECK(drained[0].path == "/first");
    // The throwing processor ran (and threw), but its mutation must not
    // appear in the drained event. The downstream processor still runs.
    CHECK(thrower->processed() == 1);
    CHECK(downstream->processed() == 1);
    CHECK(drained[0].extra.size() == 1);
    CHECK(drained[0].extra[0].first == "marker");
    CHECK(drained[0].extra[0].second == "downstream");

    // Subsequent events still route correctly — the registry is not poisoned.
    cas::TelemetryEvent ev2{};
    ev2.path = "/second";
    ev2.verdict = cas::Verdict::Allow;
    source->emit(ev2);

    CHECK(drained.size() == 2);
    CHECK(drained[1].path == "/second");
    CHECK(thrower->processed() == 2);
    CHECK(downstream->processed() == 2);
    CHECK(drained[1].extra.size() == 1);
    CHECK(drained[1].extra[0].second == "downstream");
}

// A processor that throws something that is not derived from std::exception
// must also be caught (catch(...) branch), and the chain must continue.
static void test_throwing_processor_unknown_exception_is_skipped() {
    std::vector<cas::TelemetryEvent> drained;
    cas::TelemetryRegistry registry([&](cas::TelemetryEvent ev) {
        drained.push_back(std::move(ev));
    });

    FakeBackend* source = add_fake(registry, "source", source_caps());
    auto thrower_owned = std::make_unique<ThrowingProcessor>(
        "thrower-int", ThrowingProcessor::ThrowKind::Unknown);
    registry.add(std::move(thrower_owned));

    registry.start_all(cas::BackendConfig{});

    cas::TelemetryEvent ev{};
    ev.path = "/unknown-throw";
    source->emit(ev);

    CHECK(drained.size() == 1);
    CHECK(drained[0].path == "/unknown-throw");
}

// add() called after start_all() must be rejected. In debug builds the
// assertion fires (the call aborts); in NDEBUG builds the call returns
// silently after logging and the backend list stays unchanged. Use a
// fork-based death-test so the parent can observe whichever outcome the
// build produces.
static void test_add_after_start_all_is_rejected() {
    pid_t pid = fork();
    if (pid == 0) {
        // Child: try the misuse and exit with status reflecting whether
        // the backend list grew. If the assertion fires, the child
        // aborts (signal kill) and the parent observes WIFSIGNALED.
        cas::TelemetryRegistry registry(nullptr);
        add_fake(registry, "source", source_caps());
        registry.start_all(cas::BackendConfig{});

        const std::size_t before = registry.backends().size();
        auto late = std::make_unique<FakeBackend>("late", processor_caps());
        registry.add(std::move(late));
        const std::size_t after = registry.backends().size();
        // Survived without abort (NDEBUG path). Verify the backend was
        // rejected — exit 0 if it was, non-zero otherwise.
        std::_Exit(after == before ? 0 : 2);
    }

    CHECK(pid > 0);
    int status = 0;
    pid_t w = waitpid(pid, &status, 0);
    CHECK(w == pid);
    // Either the child aborted on the assertion (debug build) or it exited
    // 0 because it observed the rejection (release build). Anything else is
    // a bug — silently growing the backend list, or the child crashed for
    // a different reason.
    if (WIFEXITED(status)) {
        CHECK(WEXITSTATUS(status) == 0);
    } else {
        CHECK(WIFSIGNALED(status));
        // SIGABRT (6) is what assert() raises. Accept any signal here —
        // the contract is "the misuse is loud" — but log for visibility.
        std::fprintf(stderr,
                     "test_add_after_start_all_is_rejected: child died with "
                     "signal %d (assert() fired in debug build)\n",
                     WTERMSIG(status));
    }
}

int main() {
    test_fanout_lifecycle_calls();
    test_source_events_reach_drain();
    test_merge_verdicts();
    test_processors_route_source_events_before_drain();
    test_processor_verdicts_merge_most_restrictive_before_drain();
    test_processors_run_in_registration_order();
    test_drain_callback_can_register_session();
    test_register_session_aggregates_backend_failures();
    test_unregister_session_aggregates_backend_failures();
    test_add_nullptr_is_ignored();
    test_start_all_starts_processors_before_eager_sources();
    test_stop_all_stops_sources_before_processors();
    test_start_stop_all_are_idempotent();
    test_failed_start_is_reported_and_excluded_from_activity();
    test_throwing_processor_is_skipped_not_fatal();
    test_throwing_processor_unknown_exception_is_skipped();
    test_add_after_start_all_is_rejected();
    std::fprintf(stderr, "test_telemetry_registry: all tests passed\n");
    return 0;
}
