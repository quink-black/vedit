#ifndef VEDIT_TESTS_TEST_SUPPORT_H
#define VEDIT_TESTS_TEST_SUPPORT_H

// Shared helpers for the test suite.
//
// Every test runs as its own process under `ctest -jN` (registered via
// catch_discover_tests). Two consequences drive this header:
//
//   1. MLT must be initialized per process. Tests used to rely on the
//      first TEST_CASE in the single binary calling Factory::init();
//      once each case is its own process, every process must init MLT
//      itself. test_main.cpp registers a listener that does this.
//
//   2. Temp files must not collide across concurrent processes. A
//      fixed path like /tmp/vedit_t8_import.mp4 is shared by every
//      process and corrupts under parallelism. unique_temp_path()
//      stamps the pid so each process gets its own file.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

namespace vedit::test {

// Build a process-unique temp path: /tmp/<prefix>-<pid>-<tag><suffix>.
// `tag` distinguishes paths within one process; the pid keeps
// concurrent processes from clobbering each other.
inline std::string unique_temp_path(const std::string& prefix,
                                    const std::string& tag,
                                    const std::string& suffix) {
    return "/tmp/" + prefix + "-" + std::to_string(::getpid())
         + "-" + tag + suffix;
}

// Generate a short A/V test clip on disk. Copies from a pre-generated
// reference clip in tests/data/ instead of running ffmpeg, so parallel
// test processes don't contend for the encoder. The path is
// process-unique so concurrent processes never clobber each other.
inline std::string make_test_clip(const std::string& tag) {
    std::string path = unique_temp_path("vedit-testclip", tag, ".mp4");
    std::string ref = std::string(VEDIT_TEST_DATA_DIR) + "/test_clip_160x120.mp4";
    std::string cmd = "cp " + ref + " " + path;
    if (std::system(cmd.c_str()) != 0) return "";
    return path;
}

// Like make_test_clip, but with an explicit size. Used by tests that
// must verify the export preserves source dimensions (regression for
// the profile-not-synced-to-source bug, where a 864x486 source
// exported as 720x576 PAL).
inline std::string make_test_clip_sized(const std::string& tag,
                                        int width, int height) {
    std::string path = unique_temp_path("vedit-testclip", tag, ".mp4");
    // Match against pre-generated reference clips.
    std::string ref;
    if (width == 640 && height == 360)
        ref = std::string(VEDIT_TEST_DATA_DIR) + "/test_clip_640x360.mp4";
    if (!ref.empty()) {
        std::string cmd = "cp " + ref + " " + path;
        if (std::system(cmd.c_str()) != 0) return "";
        return path;
    }
    // Fallback: generate via ffmpeg for non-standard sizes.
    char size[32];
    std::snprintf(size, sizeof(size), "%dx%d", width, height);
    std::string cmd =
        "ffmpeg -y -loglevel error -f lavfi "
        "-i testsrc=duration=2:size=";
    cmd += size;
    cmd += ":rate=25 -f lavfi -i sine=frequency=440:duration=2 "
           "-c:v libx264 -preset ultrafast -pix_fmt yuv420p "
           "-c:a aac -shortest " + path;
    if (std::system(cmd.c_str()) != 0) return "";
    return path;
}

} // namespace vedit::test

#endif // VEDIT_TESTS_TEST_SUPPORT_H
