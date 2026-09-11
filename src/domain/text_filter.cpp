#include "text_filter.h"

#include "util/logger.h"

#include <cstdlib>
#include <cstring>
#include <string>

#include <framework/mlt.h>
#include <framework/mlt_filter.h>
#include <framework/mlt_frame.h>
#include <framework/mlt_factory.h>
#include <framework/mlt_repository.h>

#ifdef __APPLE__
  #include <ApplicationServices/ApplicationServices.h>
  #include <CoreText/CoreText.h>
#endif

namespace vedit {

namespace {

constexpr const char* kServiceName = "vedit.text";

// Blend a premultiplied RGBA glyph bitmap over the frame buffer.
//
// The rasterizer writes premultiplied white glyphs; the requested colour
// is applied while compositing so "color" does not force a re-render.
void composite_glyphs(uint8_t* dst, int dst_w, int dst_h,
                      const uint8_t* src, int src_w, int src_h,
                      int x0, int y0,
                      uint8_t r, uint8_t g, uint8_t b) {
    for (int y = 0; y < src_h; ++y) {
        int dy = y0 + y;
        if (dy < 0 || dy >= dst_h) continue;
        for (int x = 0; x < src_w; ++x) {
            int dx = x0 + x;
            if (dx < 0 || dx >= dst_w) continue;
            const uint8_t* s = src + (static_cast<size_t>(y) * src_w + x) * 4;
            // Premultiplied source: alpha carries coverage.
            uint8_t a = s[3];
            if (a == 0) continue;
            uint8_t* d = dst + (static_cast<size_t>(dy) * dst_w + dx) * 4;
            if (a == 255) {
                d[0] = r; d[1] = g; d[2] = b;
            } else {
                // Straight alpha over.
                uint8_t ia = static_cast<uint8_t>(255 - a);
                d[0] = static_cast<uint8_t>((r * a + d[0] * ia) / 255);
                d[1] = static_cast<uint8_t>((g * a + d[1] * ia) / 255);
                d[2] = static_cast<uint8_t>((b * a + d[2] * ia) / 255);
            }
        }
    }
}

#ifdef __APPLE__

// Rasterize `text` to a premultiplied RGBA bitmap via Core Text.
// Ownership of *out_bitmap passes to the caller (malloc'd).
bool rasterize_coretext(const std::string& text, const std::string& family,
                        double size, uint8_t** out_bitmap,
                        int* out_w, int* out_h) {
    *out_bitmap = nullptr;
    *out_w = 0;
    *out_h = 0;
    if (text.empty()) return false;

    CFStringRef cf_text = CFStringCreateWithCString(
        kCFAllocatorDefault, text.c_str(), kCFStringEncodingUTF8);
    if (!cf_text) return false;

    CFStringRef cf_family = CFStringCreateWithCString(
        kCFAllocatorDefault, family.c_str(), kCFStringEncodingUTF8);
    CTFontRef font = nullptr;
    if (cf_family) {
        font = CTFontCreateWithName(cf_family, size, nullptr);
        CFRelease(cf_family);
    }
    // An empty family falls back to the system font, which is what makes
    // CJK work without shipping a font file.
    if (!font)
        font = CTFontCreateWithName(CFSTR("Helvetica"), size, nullptr);
    if (!font) { CFRelease(cf_text); return false; }

    CFStringRef keys[] = {kCTFontAttributeName};
    CFTypeRef values[] = {font};
    CFDictionaryRef attrs = CFDictionaryCreate(
        kCFAllocatorDefault, (const void**) keys, (const void**) values, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef attr = CFAttributedStringCreate(
        kCFAllocatorDefault, cf_text, attrs);
    CFRelease(cf_text);
    CFRelease(attrs);
    if (!attr) { CFRelease(font); return false; }

    CTLineRef line = CTLineCreateWithAttributedString(attr);
    CFRelease(attr);
    if (!line) { CFRelease(font); return false; }

    // Image bounds include the ink extents; pad slightly so descenders
    // and antialiasing are not clipped.
    CGRect bounds = CTLineGetImageBounds(line, nullptr);
    CGFloat ascent = 0, descent = 0;
    CTLineGetTypographicBounds(line, &ascent, &descent, nullptr);
    int w = static_cast<int>(bounds.size.width) + 4;
    int h = static_cast<int>(ascent + descent) + 4;
    if (w <= 0 || h <= 0) { CFRelease(line); CFRelease(font); return false; }

    size_t bytes = static_cast<size_t>(w) * h * 4;
    uint8_t* bitmap = static_cast<uint8_t*>(std::calloc(bytes, 1));
    if (!bitmap) { CFRelease(line); CFRelease(font); return false; }

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(
        bitmap, w, h, 8, w * 4, cs,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(cs);
    if (!ctx) {
        std::free(bitmap);
        CFRelease(line);
        CFRelease(font);
        return false;
    }

    // White fill; the colour is applied at composite time.
    CGContextSetRGBFillColor(ctx, 1.0, 1.0, 1.0, 1.0);
    CGContextSetTextPosition(ctx, 2.0 - bounds.origin.x, descent + 2.0);
    CTLineDraw(line, ctx);

    CGContextRelease(ctx);
    CFRelease(line);
    CFRelease(font);

    *out_bitmap = bitmap;
    *out_w = w;
    *out_h = h;
    return true;
}

#endif

int text_get_image(mlt_frame frame, uint8_t** image, mlt_image_format* format,
                   int* width, int* height, int writable) {
    mlt_filter filter = (mlt_filter) mlt_frame_pop_service(frame);
    mlt_properties properties = MLT_FILTER_PROPERTIES(filter);

    // Text needs RGBA; ask downstream for it.
    *format = mlt_image_rgba;
    int error = mlt_frame_get_image(frame, image, format, width, height, writable);
    if (error || !*image) return error;

    const char* text = mlt_properties_get(properties, "argument");
    if (!text || text[0] == '\0') return 0;

    const std::string family = mlt_properties_get(properties, "family")
                                   ? mlt_properties_get(properties, "family")
                                   : "";
    const double size = mlt_properties_get_double(properties, "size") > 0
                            ? mlt_properties_get_double(properties, "size")
                            : 48.0;
    // Geometry is expressed in normalised 0..1 with "0 0 1 1" as default,
    // matching how vedit expresses composite geometry elsewhere.
    const double geom_w = mlt_properties_get_double(properties, "geometry_w") > 0
                              ? mlt_properties_get_double(properties, "geometry_w")
                              : 1.0;
    const double halign = mlt_properties_get_double(properties, "halign");

    mlt_color color = mlt_properties_get_color(properties, "color");

    uint8_t* bitmap = nullptr;
    int bw = 0, bh = 0;
    bool have = false;
#ifdef __APPLE__
    have = rasterize_coretext(text, family, size, &bitmap, &bw, &bh);
#endif
    if (!have || !bitmap) {
        std::free(bitmap);
        return 0;
    }

    // Horizontal placement: halign 0 = left, 0.5 = centre, 1 = right,
    // within a band geom_w wide.
    double band = static_cast<double>(*width) * geom_w;
    double left = (static_cast<double>(*width) - band) * 0.5;
    int x0 = static_cast<int>(left + (band - bw) * halign);
    int y0 = static_cast<int>(static_cast<double>(*height) * 0.5 - bh * 0.5);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;

    composite_glyphs(*image, *width, *height, bitmap, bw, bh, x0, y0,
                     color.r, color.g, color.b);
    std::free(bitmap);
    return 0;
}

mlt_frame text_process(mlt_filter filter, mlt_frame frame) {
    mlt_frame_push_service(frame, filter);
    mlt_frame_push_get_image(frame, text_get_image);
    return frame;
}

void* text_create(mlt_profile, mlt_service_type, const char*, const void*) {
    mlt_filter filter = mlt_filter_new();
    if (!filter) return nullptr;
    filter->process = text_process;

    mlt_properties properties = MLT_FILTER_PROPERTIES(filter);
    mlt_properties_set(properties, "argument", "");
    mlt_properties_set(properties, "family", "");
    mlt_properties_set_double(properties, "size", 48.0);
    mlt_properties_set_double(properties, "geometry_w", 1.0);
    mlt_properties_set_double(properties, "halign", 0.5);
    mlt_properties_set(properties, "color", "0xFFFFFFFF");
    return filter;
}

} // namespace

void register_text_filter() {
    mlt_repository repository = mlt_factory_repository();
    if (!repository) {
        Logger::log(Logger::Level::Error,
                    "text_filter: no MLT repository, vedit.text not registered");
        return;
    }
    mlt_repository_register(repository, mlt_service_filter_type, kServiceName,
                            text_create);
}

bool text_filter_available() {
#ifdef __APPLE__
    return true;
#else
    return false;
#endif
}

} // namespace vedit
