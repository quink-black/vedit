#ifndef VEDIT_CORE_MLT_RUNTIME_H
#define VEDIT_CORE_MLT_RUNTIME_H

#include <string>

namespace vedit {

// Owns the MLT process-wide lifecycle: Factory::init / Repository /
// close. One instance per process. GUI and RPC layers both rely on
// this being alive for the whole app run; MLT objects are not safe to
// use before init() or after shutdown().
class MltRuntime {
public:
    MltRuntime() = default;
    ~MltRuntime();

    MltRuntime(const MltRuntime&) = delete;
    MltRuntime& operator=(const MltRuntime&) = delete;

    // Initializes MLT. Returns false on failure. Idempotent.
    bool init();

    // True between init() and shutdown().
    bool ok() const { return initialized_; }

    // Releases MLT. Called by the destructor; safe to call explicitly.
    void shutdown();

private:
    bool initialized_ = false;
};

} // namespace vedit

#endif
