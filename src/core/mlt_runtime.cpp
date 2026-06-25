#include "mlt_runtime.h"

#include <Mlt.h>

#include "util/logger.h"

#ifdef _WIN32
  #include <cstdlib>   // _putenv_s
  #include <filesystem>
#endif

namespace vedit {

bool MltRuntime::init() {
    if (initialized_) return true;
#ifdef _WIN32
    // On Linux/macOS MLT auto-discovers its modules from the install
    // prefix compiled into libmlt. On Windows the ExternalProject_Add
    // install lives in the build tree (VEDIT_MLT_INSTALL_DIR), so point
    // MLT_REPOSITORY and MLT_DATA there before Factory::init() or the
    // avformat/xml/sdl2 modules will not load.
  #ifdef VEDIT_MLT_INSTALL_DIR
    {
        namespace fs = std::filesystem;
        fs::path prefix = VEDIT_MLT_INSTALL_DIR;
        std::string repo = (prefix / "lib" / "mlt-7").string();
        std::string data = (prefix / "share" / "mlt-7").string();
        _putenv_s("MLT_REPOSITORY", repo.c_str());
        _putenv_s("MLT_DATA", data.c_str());
    }
  #endif
#endif
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
