#include "mlt_runtime.h"

#include <Mlt.h>

#include "util/logger.h"

#ifdef _WIN32
  #include <cstdlib>   // _putenv_s
  #include <filesystem>
#else
  #include <fcntl.h>
  #include <sys/file.h>
  #include <unistd.h>
#endif

namespace vedit {

namespace {

// MLT's Factory::init() scans and dlopen's every plugin in the
// repository directory. When many test processes (ctest -jN) call
// Factory::init() concurrently, dyld's global loader lock serializes
// the dlopen calls but can leave stragglers waiting long enough that
// ctest's pipe-read timeout triggers and the process appears to hang.
//
// Serialize Factory::init() across processes with a file lock so only
// one process runs the expensive plugin scan at a time. The lock file
// lives in TMPDIR so it is per-machine, not per-checkout.
class MltInitLock {
public:
    MltInitLock() {
        const char* tmp = getenv("TMPDIR");
        std::string path = (tmp ? tmp : "/tmp");
        path += "/vedit_mlt_init.lock";
        fd_ = open(path.c_str(), O_CREAT | O_RDWR, 0666);
        if (fd_ >= 0) {
            flock(fd_, LOCK_EX);
        }
    }
    ~MltInitLock() {
        if (fd_ >= 0) {
            flock(fd_, LOCK_UN);
            close(fd_);
        }
    }
    MltInitLock(const MltInitLock&) = delete;
    MltInitLock& operator=(const MltInitLock&) = delete;
private:
    int fd_ = -1;
};

} // namespace

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
        MltInitLock lock;
        Mlt::Factory::init(repo.c_str());
        initialized_ = true;
        Logger::log(Logger::Level::Info, "MLT Factory initialized");
        return true;
    }
  #endif
#endif
    MltInitLock lock;
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
