#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "core/preview_renderer.h"

// The preview path publishes one buffer that the GUI uploads with the
// SDL pixel format and pitch it derives from the reported byte count.
// Every frame that leaves PreviewRenderer has gone through
// copy_preview_frame(), so these cases pin the length and the byte
// layout that SDL_UpdateTexture() reads.
//
// MLT answers get_image() with the format it can produce, so the 24-bit
// case is reachable at runtime even though the headless tests only ever
// see 32-bit frames; it is exercised here instead.

TEST_CASE("copy_preview_frame keeps an RGBA frame at four bytes a pixel",
          "[preview]") {
    const uint8_t rgba[] = {1, 2, 3, 4, 5, 6, 7, 8};
    std::vector<uint8_t> out;
    REQUIRE(vedit::copy_preview_frame(rgba, mlt_image_rgba, 2, 1, out));
    REQUIRE(out == std::vector<uint8_t>{1, 2, 3, 4, 5, 6, 7, 8});
}

TEST_CASE("copy_preview_frame keeps an RGB frame at three bytes a pixel",
          "[preview]") {
    // Two pixels, red then green, three bytes each and no alpha.
    const uint8_t rgb[] = {255, 0, 0, 0, 255, 0};
    std::vector<uint8_t> out;
    REQUIRE(vedit::copy_preview_frame(rgb, mlt_image_rgb, 2, 1, out));
    REQUIRE(out.size() == 2u * 1u * 3u);
    // No alpha byte is invented: an upload sized from these six bytes
    // reads exactly the six bytes that exist.
    REQUIRE(out == std::vector<uint8_t>{255, 0, 0, 0, 255, 0});
}

TEST_CASE("copy_preview_frame rejects a format the GUI cannot upload",
          "[preview]") {
    const uint8_t yuv422[] = {1, 2, 3, 4, 5, 6, 7, 8};
    std::vector<uint8_t> out;
    REQUIRE_FALSE(
        vedit::copy_preview_frame(yuv422, mlt_image_yuv422, 2, 1, out));
    // Left alone, so a caller that keeps the previous frame on screen
    // still has that frame intact.
    REQUIRE(out.empty());
}

TEST_CASE("preview_bytes_per_pixel matches what copy_preview_frame copies",
          "[preview]") {
    REQUIRE(vedit::preview_bytes_per_pixel(mlt_image_rgba) == 4);
    REQUIRE(vedit::preview_bytes_per_pixel(mlt_image_rgb) == 3);
    REQUIRE(vedit::preview_bytes_per_pixel(mlt_image_yuv422) == 0);

    // The byte count the renderer reports and the count it copies are
    // the same number, so the GUI's upload can never read past the end.
    const uint8_t rgb[] = {1, 2, 3, 4, 5, 6};
    std::vector<uint8_t> out;
    REQUIRE(vedit::copy_preview_frame(rgb, mlt_image_rgb, 2, 1, out));
    REQUIRE(out.size() == 2u * 1u *
                              vedit::preview_bytes_per_pixel(mlt_image_rgb));
}
