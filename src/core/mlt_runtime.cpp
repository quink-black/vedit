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
    // prefix compiled into libmlt, and also respects the MLT_REPOSITORY
    // env var. On Windows, MLT's Win32 branch ignores MLT_REPOSITORY and
    // uses <exe_dir>/lib/mlt instead. Since MLT is installed in the
    // build tree (not next to the exe), pass the module directory
    // directly to Factory::init() so it finds avformat/xml/sdl2.
  #ifdef VEDIT_MLT_INSTALL_DIR
    {
        namespace fs = std::filesystem;
        fs::path prefix = VEDIT_MLT_INSTALL_DIR;
        std::string repo = (prefix / "lib" / "mlt").string();
        std::string data = (prefix / "share" / "mlt").string();
        _putenv_s("MLT_REPOSITORY", repo.c_str());
        _putenv_s("MLT_DATA", data.c_str());
        Mlt::Factory::init(repo.c_str());
        initialized_ = true;
        Logger::log(Logger::Level::Info, "MLT Factory initialized");
        return true;
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
