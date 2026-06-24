#include "mlt_runtime.h"

#include <Mlt.h>

#include "util/logger.h"

namespace vedit {

bool MltRuntime::init() {
    if (initialized_) return true;
    // MLT_REPOSITORY is auto-discovered from the install prefix on this
    // platform; no env override needed for the apt-installed MLT.
    Mlt::Factory::init();
    initialized_ = true;
    Logger::log(Logger::Level::Info, "MLT Factory initialized");
    return true;
}

void MltRuntime::shutdown() {
    // Note: Mlt::Factory::close() can segfault in some minimal smoke
    // configs (a known teardown quirk); it is a no-op for the app's
    // real lifetime since the OS reclaims resources at exit. Skip it
    // deliberately unless we observe a leak in practice.
    if (!initialized_) return;
    initialized_ = false;
    Logger::log(Logger::Level::Info, "MLT runtime marked shut down");
}

MltRuntime::~MltRuntime() {
    shutdown();
}

} // namespace vedit
