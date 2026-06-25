// Process-wide MLT initialization for the test suite.
//
// Each TEST_CASE runs as its own process under `ctest -jN`. MLT's
// Factory is process-global state that every producer/consumer needs;
// without init, constructing a ProjectModel and driving a consumer
// segfaults. Previously the first TEST_CASE in the single binary
// happened to init MLT for all the rest -- an ordering dependency that
// broke the moment cases were split into separate processes.
//
// Register a Catch2 listener that calls Factory::init() once before
// any test in the process runs. Teardown is deliberately skipped:
// MltRuntime::shutdown() is a no-op (Factory::close() can segfault in
// minimal configs) and the OS reclaims everything at process exit.
//
// MLT's Factory::init() performs one-time global allocations
// (repository, filter registry) that it never frees. LeakSanitizer
// flags these and fails the process even when the test logic passes.
// They are genuine one-time globals, not per-test leaks, so suppress
// them process-wide via __lsan_default_suppressions rather than
// chasing a free path MLT does not provide.

#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include "core/mlt_runtime.h"

// LeakSanitizer is a Clang/GCC feature; MSVC's /fsanitize=address has
// no LSan component and does not define __SANITIZE_ADDRESS__. Guard the
// suppression so MSVC builds skip the declaration entirely.
#if defined(__SANITIZE_ADDRESS__) && !defined(_MSC_VER)
extern "C" const char* __lsan_default_suppressions();
extern "C" const char* __lsan_default_suppressions() {
    // Match any leak whose stack passes through MLT or its plugin
    // loader. These are one-time process-global initializations.
    return "leak:libmlt\n"
           "leak:mlt_factory_init\n"
           "leak:mlt_repository_init\n"
           "leak:libmlt-7\n";
}
#endif

namespace {

class MltInitListener : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(const Catch::TestRunInfo&) override {
        runtime_.init();
    }

private:
    vedit::MltRuntime runtime_;
};

} // namespace

CATCH_REGISTER_LISTENER(MltInitListener)
