#include <catch2/catch_test_macros.hpp>

#include <Mlt.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdlib>
#include <thread>

#include "domain/app_controller.h"
#include "domain/export_service.h"
#include "domain/ids.h"
#include "domain/project_save_load.h"
#include "test_support.h"

using vedit::test::make_test_clip;
using vedit::test::make_test_clip_sized;
using vedit::test::unique_temp_path;

TEST_CASE("AppController imports and builds a timeline", "[t8]") {
    std::string clip = make_test_clip("import");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    REQUIRE(mi >= 0);
    REQUIRE(app.library_count() == 1);

    auto v1 = app.track_add(vedit::TrackKind::Video);
    REQUIRE(v1.valid());
    REQUIRE(app.track_count() == 1);

    auto id = app.clip_append(v1, mi);
    REQUIRE(id.valid());

    // The clip is there and resolvable through the timeline.
    const auto* ci = app.project().timeline().clip(id);
    REQUIRE(ci != nullptr);
    REQUIRE(ci->track_id == v1);
    REQUIRE(ci->media_index == mi);
    REQUIRE(ci->length > 0);
}

TEST_CASE("AppController transport: play/pause/seek", "[t8]") {
    std::string clip = make_test_clip("transport");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    app.clip_append(v1, mi);

    // Headless: use null consumer so no audio device is needed.
    app.player().set_audio_consumer("null");

    REQUIRE_FALSE(app.is_playing());
    app.seek(10);
    REQUIRE(app.position() == 10);
    REQUIRE(app.play());
    REQUIRE(app.is_playing());
    app.pause();
    REQUIRE_FALSE(app.is_playing());
}

TEST_CASE("AppController edit operations through the seam", "[t8]") {
    std::string clip = make_test_clip("edit");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    auto v1 = app.track_add(vedit::TrackKind::Video);

    auto id1 = app.clip_append(v1, mi);
    auto id2 = app.clip_append(v1, mi);
    REQUIRE(id1.valid());
    REQUIRE(id2.valid());
    REQUIRE(id1 != id2);

    const auto* c1 = app.project().timeline().clip(id1);
    REQUIRE(c1 != nullptr);
    int64_t orig_len = c1->length;

    // Split id1 in half — produces a new id, original id shrinks.
    auto right_id = app.clip_split(id1, c1->start + orig_len / 2);
    REQUIRE(right_id.valid());
    REQUIRE(right_id != id1);

    const auto* left = app.project().timeline().clip(id1);
    const auto* right = app.project().timeline().clip(right_id);
    REQUIRE(left != nullptr);
    REQUIRE(right != nullptr);
    REQUIRE(left->length + right->length == orig_len);

    // Move id2 (still unsplit) to position 0 on same track.
    REQUIRE(app.clip_move(id2, v1, 0));
    const auto* c2_after = app.project().timeline().clip(id2);
    REQUIRE(c2_after != nullptr);
    REQUIRE(c2_after->start == 0);

    // Remove the right half — id1 and id2 survive.
    REQUIRE(app.clip_remove(right_id));
    REQUIRE(app.project().timeline().clip(right_id) == nullptr);
    REQUIRE(app.project().timeline().clip(id1) != nullptr);
    REQUIRE(app.project().timeline().clip(id2) != nullptr);
}

TEST_CASE("AppController two-track composition through the seam", "[t8]") {
    std::string clip = make_test_clip("compose");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    auto v2 = app.track_add(vedit::TrackKind::Video);
    app.clip_append(v1, mi);
    app.clip_append(v2, mi);

    // The tractor (via ProjectModel) must produce a valid composed
    // frame — same test that proved T6's composite transition, now
    // going through AppController.
    Mlt::Producer* tractor = app.project().producer();
    REQUIRE(tractor != nullptr);
    Mlt::Frame* frame = tractor->get_frame();
    REQUIRE(frame != nullptr);
    REQUIRE(frame->is_valid());
    mlt_image_format fmt = mlt_image_none;
    int w = 0, h = 0;
    uint8_t* img = frame->get_image(fmt, w, h);
    REQUIRE(img != nullptr);
    REQUIRE(w > 0);
    REQUIRE(h > 0);
    delete frame;
}

TEST_CASE("AppController track remove cleans up clips", "[t8]") {
    std::string clip = make_test_clip("trackrm");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    auto id = app.clip_append(v1, mi);
    REQUIRE(app.track_count() == 1);
    REQUIRE(app.project().timeline().clip(id) != nullptr);

    REQUIRE(app.track_remove(v1));
    REQUIRE(app.track_count() == 0);
    REQUIRE(app.project().timeline().clip(id) == nullptr);
}

TEST_CASE("ExportService renders a single-track timeline to MP4", "[t9][export]") {
    std::string clip = make_test_clip("export");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    app.clip_append(v1, mi);

    // Regression: the default MLT profile is interlaced (progressive=0),
    // which made libx264 refuse to encode ("interlace + weightp is not
    // implemented") and produced an empty file. ProjectModel must force
    // progressive.
    REQUIRE(app.profile().progressive() == 1);

    std::string out = unique_temp_path("vedit-test", "t9_export", ".mp4");
    vedit::ExportService svc;
    double progress = -1;
    REQUIRE(svc.render(*app.project().producer(), app.profile(), out,
                       [&](double p) { progress = p; }));

    // The exported file must contain a video stream (not empty).
    std::string cmd = "ffprobe -v error -show_entries format=duration:stream=codec_name,width,height "
                      "-of csv " + out + " 2>&1";
    FILE* f = popen(cmd.c_str(), "r");
    REQUIRE(f != nullptr);
    std::string probe;
    char buf[256] = {0};
    while (fgets(buf, sizeof(buf), f)) probe += buf;
    pclose(f);
    REQUIRE(probe.find("h264") != std::string::npos);  // has a video stream
    REQUIRE(progress >= 0.0);  // progress callback fired
}

TEST_CASE("ExportService get_frame saves a single PNG", "[t9][export]") {
    std::string clip = make_test_clip("frame");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    app.clip_append(v1, mi);

    std::string out = unique_temp_path("vedit-test", "t9_frame", ".png");
    vedit::ExportService svc;
    REQUIRE(svc.get_frame(*app.project().producer(), app.profile(), 0, out));

    // Check the file exists and is non-empty.
    FILE* f = fopen(out.c_str(), "rb");
    REQUIRE(f != nullptr);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    REQUIRE(sz > 100);  // any valid PNG is > 100 bytes
}

// Regression: the project profile used to stay at the MLT default
// (DV/DVD PAL 720x576) regardless of source dimensions. Importing a
// 864x486 clip exported as 720x576. The profile must sync to the
// source so export preserves resolution.
TEST_CASE("ExportService preserves source resolution, not PAL default", "[t9][export]") {
    // 640x360 is not the MLT default (720x576), so a pass here proves
    // the profile was synced from the source rather than left default.
    std::string clip = make_test_clip_sized("resolution", 640, 360);
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    REQUIRE(mi >= 0);
    REQUIRE(app.profile().width() == 640);
    REQUIRE(app.profile().height() == 360);
    REQUIRE(app.profile().progressive() == 1);

    auto v1 = app.track_add(vedit::TrackKind::Video);
    app.clip_append(v1, mi);

    std::string out = unique_temp_path("vedit-test", "t9_resolution", ".mp4");
    vedit::ExportService svc;
    REQUIRE(svc.render(*app.project().producer(), app.profile(), out));

    // The exported file must carry the source dimensions, not 720x576.
    std::string cmd = "ffprobe -v error -show_entries stream=codec_name,width,height "
                      "-of csv " + out + " 2>&1";
    FILE* f = popen(cmd.c_str(), "r");
    REQUIRE(f != nullptr);
    std::string probe;
    char buf[256] = {0};
    while (fgets(buf, sizeof(buf), f)) probe += buf;
    pclose(f);
    REQUIRE(probe.find("h264") != std::string::npos);
    REQUIRE(probe.find("640") != std::string::npos);
    REQUIRE(probe.find("360") != std::string::npos);
}

TEST_CASE("ProjectSaveLoad save and load XML round-trip", "[t10]") {
    std::string clip = make_test_clip("save");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    // Build a 2-track timeline.
    vedit::AppController app;
    int mi = app.library_import(clip);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    auto v2 = app.track_add(vedit::TrackKind::Video);
    auto id1 = app.clip_append(v1, mi);
    auto id2 = app.clip_append(v2, mi);
    REQUIRE(id1.valid());
    REQUIRE(id2.valid());
    REQUIRE(app.track_count() == 2);

    // Save.
    std::string xml_path = unique_temp_path("vedit-test", "t10_roundtrip", ".xml");
    REQUIRE(vedit::ProjectSaveLoad::save_xml(app.project(), xml_path));

    // Load into a fresh AppController.
    vedit::AppController app2;
    REQUIRE(vedit::ProjectSaveLoad::load_xml(app2.project(), xml_path));
    REQUIRE(app2.track_count() == 2);
    REQUIRE(app2.project().timeline().clip_count() == 2);
    REQUIRE(app2.project().timeline().duration() > 0);
}

TEST_CASE("ProjectSaveLoad state_json describes timeline", "[t10]") {
    std::string clip = make_test_clip("json");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    app.clip_append(v1, mi);

    std::string json = vedit::ProjectSaveLoad::state_json(app.project());
    REQUIRE(json.find("\"tracks\"") != std::string::npos);
    REQUIRE(json.find("\"clips\"") != std::string::npos);
    REQUIRE(json.find("\"start\"") != std::string::npos);
    REQUIRE(json.find("\"length\"") != std::string::npos);
    REQUIRE(json.find(clip) != std::string::npos);

    // The MCP agent needs library + clip ids to drive edits.
    REQUIRE(json.find("\"library\"") != std::string::npos);
    REQUIRE(json.find("\"media_index\"") != std::string::npos);
    REQUIRE(json.find("\"id\":1") != std::string::npos);

    // The JSON must be parseable (no malformed output).
    nlohmann::json parsed = nlohmann::json::parse(json);
    REQUIRE(parsed.contains("library"));
    REQUIRE(parsed.contains("tracks"));
    REQUIRE(parsed["library"].size() == 1);
    REQUIRE(parsed["library"][0]["index"] == 0);
    REQUIRE(parsed["tracks"].size() == 1);
    REQUIRE(parsed["tracks"][0].contains("id"));
    REQUIRE(parsed["tracks"][0]["clips"].size() == 1);
    REQUIRE(parsed["tracks"][0]["clips"][0].contains("id"));
    REQUIRE(parsed["tracks"][0]["clips"][0].contains("media_index"));
}

TEST_CASE("M1 end-to-end: RPC builds 2-track timeline and exports", "[t12][export]") {
    std::string clip = make_test_clip("e2e");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    // ---- Build a 2-track timeline through AppController (same path
    //      the RPC method handlers take). ----
    vedit::AppController app;
    int mi = app.library_import(clip);
    REQUIRE(mi >= 0);

    auto v1 = app.track_add(vedit::TrackKind::Video, "V1");
    auto v2 = app.track_add(vedit::TrackKind::Video, "V2");
    REQUIRE(v1.valid());
    REQUIRE(v2.valid());
    REQUIRE(app.track_count() == 2);

    // Track 0: two clips.
    auto id1 = app.clip_append(v1, mi);
    auto id2 = app.clip_append(v1, mi);
    REQUIRE(id1.valid());
    REQUIRE(id2.valid());

    // Track 1: one clip, then split.
    auto id3 = app.clip_append(v2, mi);
    REQUIRE(id3.valid());
    const auto* c3 = app.project().timeline().clip(id3);
    REQUIRE(c3 != nullptr);
    auto id4 = app.clip_split(id3, c3->start + c3->length / 2);
    REQUIRE(id4.valid());
    REQUIRE(id4 != id3);

    // ---- Save to XML and reload into a fresh AppController ----
    std::string xml_path = unique_temp_path("vedit-test", "m1_e2e", ".xml");
    REQUIRE(vedit::ProjectSaveLoad::save_xml(app.project(), xml_path));

    vedit::AppController app2;
    REQUIRE(vedit::ProjectSaveLoad::load_xml(app2.project(), xml_path));
    REQUIRE(app2.track_count() == 2);
    // Clip count: v1 had 2, v2 had 2 (split) = 4 total.
    REQUIRE(app2.project().timeline().clip_count() == 4);

    // ---- Export both projects and verify they produce valid files ----
    vedit::ExportService svc;
    std::string out1 = unique_temp_path("vedit-test", "m1_e2e_orig", ".mp4");
    std::string out2 = unique_temp_path("vedit-test", "m1_e2e_reload", ".mp4");
    REQUIRE(svc.render(*app.project().producer(), app.profile(), out1));
    REQUIRE(svc.render(*app2.project().producer(), app2.profile(), out2));

    // Verify both outputs exist and have the same duration.
    auto probe_dur = [](const std::string& path) -> double {
        std::string cmd = "ffprobe -v error -show_entries format=duration "
                          "-of csv=p=0 " + path + " 2>&1";
        FILE* f = popen(cmd.c_str(), "r");
        if (!f) return -1;
        char buf[64] = {0};
        double d = -1;
        if (fgets(buf, sizeof(buf), f)) d = std::atof(buf);
        pclose(f);
        return d;
    };
    double d1 = probe_dur(out1);
    double d2 = probe_dur(out2);
    REQUIRE(d1 > 0);
    // Reloaded project should produce same-duration output (within
    // rounding tolerance of the probe).
    REQUIRE(std::abs(d1 - d2) < 0.1);

    // ---- state.json describes the timeline completely ----
    std::string json = vedit::ProjectSaveLoad::state_json(app.project());
    REQUIRE(json.find("\"tracks\"") != std::string::npos);
    REQUIRE(json.find("\"clips\"") != std::string::npos);
    REQUIRE(json.find("\"V1\"") != std::string::npos);
    REQUIRE(json.find("\"V2\"") != std::string::npos);
}

// Regression: the GUI's core interaction was broken — after adding a
// clip, the preview stayed blank because frames only appeared during
// playback. These tests lock in the "always a picture" behaviour:
// current_frame() must return a frame while PAUSED.

TEST_CASE("Preview shows a frame while paused (no playback)", "[t13][preview]") {
    std::string clip = make_test_clip("preview_paused");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    app.player().set_audio_consumer("null");
    int mi = app.library_import(clip);
    REQUIRE(mi >= 0);
    auto track = app.track_add(vedit::TrackKind::Video, "V1");
    auto cid = app.clip_append(track, mi);
    REQUIRE(cid.valid());
    REQUIRE(app.project().timeline().duration() > 0);

    // Not playing — but a preview frame must still be available.
    REQUIRE_FALSE(app.is_playing());
    int w = 0, h = 0;
    const uint8_t* img = nullptr;
    REQUIRE(app.player().current_frame(w, h, img));
    REQUIRE(w > 0);
    REQUIRE(h > 0);
    REQUIRE(img != nullptr);
}

TEST_CASE("Scrubbing while paused updates the preview", "[t13][preview]") {
    std::string clip = make_test_clip("preview_scrub");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    app.player().set_audio_consumer("null");
    int mi = app.library_import(clip);
    auto track = app.track_add(vedit::TrackKind::Video, "V1");
    app.clip_append(track, mi);

    int w = 0, h = 0;
    const uint8_t* img = nullptr;

    app.seek(0);
    REQUIRE(app.player().current_frame(w, h, img));
    REQUIRE(app.position() == 0);

    app.seek(20);
    REQUIRE(app.position() == 20);
    REQUIRE(app.player().current_frame(w, h, img));
    REQUIRE(img != nullptr);
}

TEST_CASE("Adding a second clip refreshes the paused preview", "[t13][preview]") {
    std::string clip = make_test_clip("preview_refresh");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    app.player().set_audio_consumer("null");
    int mi = app.library_import(clip);
    auto track = app.track_add(vedit::TrackKind::Video, "V1");
    app.clip_append(track, mi);
    int64_t dur1 = app.project().timeline().duration();

    app.clip_append(track, mi);
    int64_t dur2 = app.project().timeline().duration();
    REQUIRE(dur2 > dur1);  // timeline grew

    // Preview still works after the edit (refresh() was called).
    int w = 0, h = 0;
    const uint8_t* img = nullptr;
    REQUIRE(app.player().current_frame(w, h, img));
    REQUIRE(img != nullptr);
}

// Regression: after playback reached EOF (consumer self-stopped with
// terminate_on_pause), the producer was left with speed 0 at the last
// frame. Export then ran the avformat consumer with terminate_on_pause
// too, which saw "already paused" and produced a valid-but-empty
// 262-byte MP4. Export must rewind and restore forward playback so the
// full timeline renders regardless of where preview left the playhead.
TEST_CASE("Export works after playback reached EOF", "[t9][export]") {
    std::string clip = make_test_clip("eof");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    app.player().set_audio_consumer("null");
    int mi = app.library_import(clip);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    app.clip_append(v1, mi);

    // Play to EOF. The 2s test clip under "null" audio consumer runs
    // out well under this timeout; spin until the consumer self-stops.
    REQUIRE(app.play());
    for (int i = 0; i < 200 && app.is_playing(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE_FALSE(app.is_playing());  // reached EOF
    app.pause();

    // Now export from this state. Before the fix this produced an
    // empty 262-byte file; it must produce a real, non-empty MP4.
    std::string out = unique_temp_path("vedit-test", "t9_eof", ".mp4");
    vedit::ExportService svc;
    REQUIRE(svc.render(*app.project().producer(), app.profile(), out));

    FILE* f = fopen(out.c_str(), "rb");
    REQUIRE(f != nullptr);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    REQUIRE(sz > 10000);  // real video, not the 262-byte empty shell

    // And playback must be restartable from this state (Play after EOF
    // was flaky: the first click did nothing, the second worked).
    REQUIRE(app.play());
    REQUIRE(app.is_playing());
    app.pause();
}

TEST_CASE("Tracks are named per type (V1/V2, A1/A2)", "[t14][multitrack]") {
    vedit::AppController app;
    auto v1 = app.track_add(vedit::TrackKind::Video);
    auto v2 = app.track_add(vedit::TrackKind::Video);
    auto a1 = app.track_add(vedit::TrackKind::Audio);
    auto v3 = app.track_add(vedit::TrackKind::Video);
    auto& tl = app.project().timeline();
    REQUIRE(tl.track(v1)->name == "V1");
    REQUIRE(tl.track(v2)->name == "V2");
    REQUIRE(tl.track(a1)->name == "A1");
    REQUIRE(tl.track(v3)->name == "V3");
}

TEST_CASE("Clips can be added to a non-first track", "[t14][multitrack]") {
    std::string clip = make_test_clip("nonfirst");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    auto v2 = app.track_add(vedit::TrackKind::Video);
    auto v3 = app.track_add(vedit::TrackKind::Video);

    // Add to V2 and V3, not V1 — the user's broken case.
    auto c2 = app.clip_append(v2, mi);
    auto c3 = app.clip_append(v3, mi);
    REQUIRE(c2.valid());
    REQUIRE(c3.valid());

    auto& tl = app.project().timeline();
    REQUIRE(tl.clips_on_track(v1).empty());
    REQUIRE(tl.clips_on_track(v2).size() == 1);
    REQUIRE(tl.clips_on_track(v3).size() == 1);
}

TEST_CASE("Clip moves across tracks via the controller", "[t14][multitrack]") {
    std::string clip = make_test_clip("xtrack");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    auto v2 = app.track_add(vedit::TrackKind::Video);
    auto c = app.clip_append(v2, mi);
    REQUIRE(c.valid());

    REQUIRE(app.clip_move(c, v1, 0));
    auto& tl = app.project().timeline();
    REQUIRE(tl.clips_on_track(v1).size() == 1);
    REQUIRE(tl.clips_on_track(v2).empty());
    REQUIRE(tl.clip(c)->track_id == v1);
}
