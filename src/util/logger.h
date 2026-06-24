#ifndef VEDIT_UTIL_LOGGER_H
#define VEDIT_UTIL_LOGGER_H

#include <cstdio>

namespace vedit {

// Minimal logger. Real backends (file sink, rotating) come later.
// Keep the surface tiny so call sites do not change when the backend grows.
class Logger {
public:
    enum class Level { Debug, Info, Warn, Error };

    static void log(Level level, const char* msg) {
        const char* tag = "INFO";
        switch (level) {
            case Level::Debug: tag = "DBG"; break;
            case Level::Info:  tag = "INF"; break;
            case Level::Warn:  tag = "WRN"; break;
            case Level::Error: tag = "ERR"; break;
        }
        std::fprintf(stderr, "[vedit %s] %s\n", tag, msg);
    }
};

} // namespace vedit

#endif
