#ifndef VEDIT_DOMAIN_TEXT_FILTER_H
#define VEDIT_DOMAIN_TEXT_FILTER_H

namespace vedit {

// Registers an in-process MLT filter named "vedit.text".
//
// MLT's own text filters route through qtext (Qt) or pango, and both are
// unusable from this process: see doc/tech-direction-text-and-mlt.md,
// where a headless render with dynamictext hangs and ignores SIGTERM.
// This filter receives the RGBA frame buffer in its process callback and
// draws into it directly, which sidesteps Qt entirely and keeps CJK
// correct on macOS through Core Text.
//
// Draws nothing on platforms without a rasterizer, so callers should
// check text_filter_available() before offering it.
void register_text_filter();

// True where a glyph rasterizer is compiled in.
bool text_filter_available();

} // namespace vedit

#endif
