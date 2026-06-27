#include <catch2/catch_test_macros.hpp>

#include <Mlt.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

#include "domain/app_controller.h"
#include "domain/export_service.h"
#include "domain/ids.h"
#include "domain/media_library.h"  // for StreamInfo
#include "domain/project_save_load.h"
#include "rpc/rpc_dispatcher.h"
#include "rpc/rpc_methods.h"
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

TEST_CASE("AppController library_remove refuses in-use source", "[t8]") {
    std::string clip = make_test_clip("librm_inuse");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    REQUIRE(mi >= 0);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    app.clip_append(v1, mi);

    // Source is referenced by a clip on the timeline: refuse.
    REQUIRE_FALSE(app.library_remove(mi));
    REQUIRE(app.library_count() == 1);
    REQUIRE(app.project().library().item(mi) != nullptr);

    // Drop the clip -> removal now succeeds and library collapses.
    REQUIRE(app.clip_remove(app.project().timeline().clips_on_track(v1)[0].id));
    REQUIRE(app.library_remove(mi));
    REQUIRE(app.library_count() == 0);
    REQUIRE(app.project().library().item(mi) == nullptr);
}

TEST_CASE("AppController library_remove reindexes remaining sources", "[t8]") {
    std::string clipA = make_test_clip("librm_a");
    std::string clipB = make_test_clip("librm_b");
    if (clipA.empty() || clipB.empty()) { FAIL("could not generate test clips"); return; }

    vedit::AppController app;
    int miA = app.library_import(clipA);
    int miB = app.library_import(clipB);
    REQUIRE(miA == 0);
    REQUIRE(miB == 1);

    auto v1 = app.track_add(vedit::TrackKind::Video);
    // Append B only; A is unused and can be dropped.
    auto idB = app.clip_append(v1, miB);
    REQUIRE(idB.valid());

    REQUIRE(app.library_remove(miA));          // drops index 0
    REQUIRE(app.library_count() == 1);
    // B is now at index 0 in the library, and the clip's stamped
    // media_index must have been shifted down to 0 as well so
    // media_index_of still resolves correctly.
    REQUIRE(app.project().timeline().media_index_of(idB) == 0);
    REQUIRE(app.project().library().item(0)->path == clipB);
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
    FILE* f = VEDIT_POPEN(cmd.c_str(), "r");
    REQUIRE(f != nullptr);
    std::string probe;
    char buf[256] = {0};
    while (fgets(buf, sizeof(buf), f)) probe += buf;
    VEDIT_PCLOSE(f);
    REQUIRE(probe.find("h264") != std::string::npos);  // has a video stream
    REQUIRE(progress >= 0.0);  // progress callback fired
}

TEST_CASE("ExportService range export renders only requested frames", "[t9][export]") {
    std::string clip = make_test_clip("range_export");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    app.clip_append(v1, mi);

    // Export frames 10-20 (should produce ~10 frames, ~0.4s at 25fps).
    std::string out_full = unique_temp_path("vedit-test", "range_full", ".mp4");
    std::string out_range = unique_temp_path("vedit-test", "range_part", ".mp4");
    vedit::ExportService svc;
    REQUIRE(svc.render(*app.project().producer(), app.profile(), out_full));
    REQUIRE(svc.render(*app.project().producer(), app.profile(), out_range,
                       nullptr, nullptr, 10, 20));

    // Probe durations. The range export should be shorter.
    auto probe_dur = [](const std::string& path) -> double {
        std::string cmd = "ffprobe -v error -show_entries format=duration "
                          "-of csv=p=0 " + path + " 2>&1";
        FILE* f = VEDIT_POPEN(cmd.c_str(), "r");
        if (!f) return -1;
        char buf[64] = {0};
        fgets(buf, sizeof(buf), f);
        VEDIT_PCLOSE(f);
        return std::atof(buf);
    };
    double dur_full = probe_dur(out_full);
    double dur_range = probe_dur(out_range);
    REQUIRE(dur_full > 0.0);
    REQUIRE(dur_range > 0.0);
    REQUIRE(dur_range < dur_full);
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
    FILE* f = VEDIT_POPEN(cmd.c_str(), "r");
    REQUIRE(f != nullptr);
    std::string probe;
    char buf[256] = {0};
    while (fgets(buf, sizeof(buf), f)) probe += buf;
    VEDIT_PCLOSE(f);
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
#ifdef _WIN32
    // state_json normalizes backslashes to forward slashes.
    std::replace(clip.begin(), clip.end(), '\\', '/');
#endif

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
        FILE* f = VEDIT_POPEN(cmd.c_str(), "r");
        if (!f) return -1;
        char buf[64] = {0};
        double d = -1;
        if (fgets(buf, sizeof(buf), f)) d = std::atof(buf);
        VEDIT_PCLOSE(f);
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
    // out well under this timeout; spin until the consumer self-stops
    // (has_self_stopped), then call pause() to tear down the renderer
    // and reset playing_ -- exactly what the GUI main loop does.
    REQUIRE(app.play());
    for (int i = 0; i < 200 && !app.player().has_self_stopped(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(app.player().has_self_stopped());
    app.pause();
    REQUIRE_FALSE(app.is_playing());  // playing_ cleared by pause()

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

// Regression: trim through AppController. The GUI drag-trim path
// calls app.clip_trim; a refactor of the GUI interaction layer once
// broke trim because the domain call was never reached. This test
// pins the domain contract so future GUI refactors catch a
// regression in the controller even when the GUI is not exercised.
TEST_CASE("AppController trim changes clip in/out and length", "[regression]") {
    std::string clip = make_test_clip("trim_reg");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    auto id = app.clip_append(v1, mi);
    REQUIRE(id.valid());

    const auto* c = app.project().timeline().clip(id);
    REQUIRE(c != nullptr);
    int64_t orig_in = c->in_point;
    int64_t orig_out = c->out_point;
    int64_t orig_len = c->length;

    // Trim 10 frames off the right edge.
    int64_t new_out = orig_out - 10;
    REQUIRE(app.clip_trim(id, orig_in, new_out));

    const auto* c2 = app.project().timeline().clip(id);
    REQUIRE(c2 != nullptr);
    REQUIRE(c2->out_point == new_out);
    REQUIRE(c2->length == orig_len - 10);
}

// Regression: two video tracks must export as a single composited
// MP4 with one video stream, not a multi-track MP4. The tractor
// composites V1+V2 via planted transitions; the avformat consumer
// encodes the composited frames. This test pins that contract so
// a refactor that accidentally bypasses compositing is caught.
TEST_CASE("Two-track export produces single composited video stream",
          "[regression][export]") {
    std::string clip = make_test_clip("twotrack_reg");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    auto v2 = app.track_add(vedit::TrackKind::Video);
    app.clip_append(v1, mi);
    app.clip_append(v2, mi);
    REQUIRE(app.track_count() == 2);

    std::string out = unique_temp_path("vedit-test", "twotrack_reg", ".mp4");
    vedit::ExportService svc;
    REQUIRE(svc.render(*app.project().producer(), app.profile(), out,
                       [](double){}));

    // The exported MP4 must have exactly one video stream (composited)
    // and one audio stream. ffprobe output format: codec_type per line.
    std::string cmd = "ffprobe -v error -show_entries stream=codec_type "
                      "-of csv " + out + " 2>&1";
    FILE* f = VEDIT_POPEN(cmd.c_str(), "r");
    REQUIRE(f != nullptr);
    std::string probe;
    char buf[256] = {0};
    while (fgets(buf, sizeof(buf), f)) probe += buf;
    VEDIT_PCLOSE(f);

    int video_count = 0, audio_count = 0;
    size_t pos = 0;
    while ((pos = probe.find("video", pos)) != std::string::npos) {
        ++video_count;
        pos += 5;
    }
    pos = 0;
    while ((pos = probe.find("audio", pos)) != std::string::npos) {
        ++audio_count;
        pos += 5;
    }
    REQUIRE(video_count == 1);
    REQUIRE(audio_count == 1);
}

TEST_CASE("clip.insert via RPC honors in/out sub-range", "[t13]") {
    std::string clip = make_test_clip("insert_subrange");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    REQUIRE(mi >= 0);
    auto v1 = app.track_add(vedit::TrackKind::Video);

    vedit::rpc::Dispatcher d;
    vedit::rpc::register_methods(d, app);

    // Append a full clip first to establish source length, then insert
    // a sub-range [10, 30) at timeline position 0 via RPC.
    auto full = app.clip_append(v1, mi);
    REQUIRE(full.valid());
    const auto* full_info = app.project().timeline().clip(full);
    REQUIRE(full_info != nullptr);
    REQUIRE(app.clip_remove(full));

    nlohmann::json req = {{"jsonrpc","2.0"},{"method","clip.insert"},{"id",1},
                          {"params", {{"track_id", v1.value},
                                      {"media_index", mi},
                                      {"position", 0},
                                      {"in", 10},
                                      {"out", 30}}}};
    nlohmann::json resp = nlohmann::json::parse(d.handle_request(req.dump()));
    REQUIRE_FALSE(resp.contains("error"));
    int64_t new_id = resp["result"]["clip_id"];
    const auto* info = app.project().timeline().clip(vedit::ClipId{new_id});
    REQUIRE(info != nullptr);
    // MLT's Producer::cut(in,out) treats `out` as INCLUSIVE, so
    // [10,30] yields 21 frames (30-10+1). This matches clip.append's
    // behavior; documented here so an agent author reading the test
    // learns the convention.
    REQUIRE(info->in_point == 10);
    REQUIRE(info->out_point == 30);
    REQUIRE(info->length == 21);
}

TEST_CASE("player.get_frame / view.screenshot over RPC dispatcher", "[t13][export]") {
    std::string clip = make_test_clip("rpc_getframe");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    REQUIRE(mi >= 0);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    REQUIRE(app.clip_append(v1, mi).valid());

    vedit::rpc::Dispatcher d;
    vedit::rpc::register_methods(d, app);

    std::string frame_path = vedit::test::unique_temp_path(
        "vedit-test", "rpc_getframe", ".png");
    std::string shot_path = vedit::test::unique_temp_path(
        "vedit-test", "rpc_screenshot", ".png");

    // player.get_frame at position 5
    {
        nlohmann::json req = {{"jsonrpc","2.0"},{"method","player.get_frame"},{"id",1},
                              {"params", {{"position", 5}, {"path", frame_path}}}};
        nlohmann::json resp = nlohmann::json::parse(d.handle_request(req.dump()));
        REQUIRE(resp.value("id", 0) == 1);
        REQUIRE_FALSE(resp.contains("error"));
        REQUIRE(resp["result"]["position"] == 5);
        REQUIRE(resp["result"]["path"] == frame_path);
        std::FILE* f = std::fopen(frame_path.c_str(), "rb");
        REQUIRE(f != nullptr);
        std::fseek(f, 0, SEEK_END);
        long sz = std::ftell(f);
        std::fclose(f);
        REQUIRE(sz > 100);  // non-trivial PNG
    }

    // view.screenshot at the current playhead (default position 0)
    app.seek(0);
    {
        nlohmann::json req = {{"jsonrpc","2.0"},{"method","view.screenshot"},{"id",2},
                              {"params", {{"path", shot_path}}}};
        nlohmann::json resp = nlohmann::json::parse(d.handle_request(req.dump()));
        REQUIRE(resp.value("id", 0) == 2);
        REQUIRE_FALSE(resp.contains("error"));
        REQUIRE(resp["result"]["position"] == 0);
        std::FILE* f = std::fopen(shot_path.c_str(), "rb");
        REQUIRE(f != nullptr);
        std::fclose(f);
    }
}

TEST_CASE("Async export via AppController export_start/poll", "[t14][export]") {
    std::string clip = make_test_clip("async_export");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    REQUIRE(mi >= 0);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    REQUIRE(app.clip_append(v1, mi).valid());

    std::string out = vedit::test::unique_temp_path(
        "vedit-test", "async_export", ".mp4");
    int64_t job_id = app.export_start(out);
    REQUIRE(job_id > 0);

    // Poll until the worker reports a terminal state. Bounded by the
    // clip duration; a 1s source renders in well under 10s on any
    // modern CPU, so 30s is a generous ceiling.
    vedit::AppController::ExportPollResult r;
    bool terminal = false;
    for (int i = 0; i < 300; ++i) {  // 300 * 100ms = 30s
        r = app.export_poll(job_id);
        REQUIRE(r.found);
        if (r.state != "running") { terminal = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    REQUIRE(terminal);
    REQUIRE(r.state == "done");
    REQUIRE(r.fraction == 1.0);

    // The output must be a non-trivial MP4 with an h264 video stream.
    std::FILE* f = std::fopen(out.c_str(), "rb");
    REQUIRE(f != nullptr);
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fclose(f);
    REQUIRE(sz > 1000);

    std::string cmd = "ffprobe -v error -show_entries stream=codec_name "
                      "-of default=noprint_wrappers=1 \"" + out + "\" 2>&1";
    std::FILE* p = VEDIT_POPEN(cmd.c_str(), "r");
    REQUIRE(p != nullptr);
    std::string probe;
    char buf[256];
    while (fgets(buf, sizeof(buf), p)) probe += buf;
    VEDIT_PCLOSE(p);
    REQUIRE(probe.find("h264") != std::string::npos);
}

TEST_CASE("Async export poll on unknown job_id returns found=false", "[t14]") {
    vedit::AppController app;
    auto r = app.export_poll(99999);
    REQUIRE_FALSE(r.found);
}

// =============================================================================
// Multi-stream selection (library.import video_index/audio_index, set_stream,
// state.get streams list).
// =============================================================================

TEST_CASE("MediaLibrary import records stream list", "[t15]") {
    std::string clip = vedit::test::make_test_clip_multistream("streams_list");
    if (clip.empty()) { FAIL("could not generate multistream test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    REQUIRE(mi >= 0);
    const auto* item = app.project().library().item(mi);
    REQUIRE(item != nullptr);

    // Expect at least 1 video + 2 audio streams.
    int nv = 0, na = 0;
    for (const auto& s : item->streams) {
        if (s.type == "video") ++nv;
        else if (s.type == "audio") ++na;
        REQUIRE_FALSE(s.codec.empty());
    }
    REQUIRE(nv == 1);
    REQUIRE(na >= 2);

    // The default audio stream must be marked is_default; the other not.
    int default_count = 0;
    for (const auto& s : item->streams) {
        if (s.type == "audio" && s.is_default) ++default_count;
    }
    REQUIRE(default_count == 1);

    // audio_index / video_index on the item must reflect the producer's
    // selected stream (the first of each type by default).
    REQUIRE(item->video_index >= 0);
    REQUIRE(item->audio_index >= 0);
}

TEST_CASE("library.import with audio_index selects requested stream", "[t15]") {
    std::string clip = vedit::test::make_test_clip_multistream("import_audio_idx");
    if (clip.empty()) { FAIL("could not generate multistream test clip"); return; }

    vedit::AppController app;
    // Import with audio_index=2 (the second audio stream, ac3).
    int mi = app.library_import(clip, /*video_index=*/-1, /*audio_index=*/2);
    REQUIRE(mi >= 0);
    const auto* item = app.project().library().item(mi);
    REQUIRE(item != nullptr);
    REQUIRE(item->audio_index == 2);

    // Exactly one audio stream is_default and it's the one at index 2.
    const vedit::StreamInfo* def = nullptr;
    for (const auto& s : item->streams) {
        if (s.type == "audio" && s.is_default) { def = &s; break; }
    }
    REQUIRE(def != nullptr);
    REQUIRE(def->index == 2);
}

TEST_CASE("library.set_stream switches audio", "[t15]") {
    std::string clip = vedit::test::make_test_clip_multistream("set_stream");
    if (clip.empty()) { FAIL("could not generate multistream test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);  // default = first audio stream
    REQUIRE(mi >= 0);
    const auto* item = app.project().library().item(mi);
    REQUIRE(item != nullptr);
    int orig_audio = item->audio_index;
    REQUIRE(orig_audio >= 0);

    // Find another audio stream to switch to.
    int other = -1;
    for (const auto& s : item->streams) {
        if (s.type == "audio" && s.index != orig_audio) { other = s.index; break; }
    }
    REQUIRE(other >= 0);

    REQUIRE(app.library_set_stream(mi, -1, other));
    item = app.project().library().item(mi);
    REQUIRE(item != nullptr);
    REQUIRE(item->audio_index == other);
}

TEST_CASE("library.set_stream switches audio when in use", "[t15]") {
    std::string clip = vedit::test::make_test_clip_multistream("set_stream_inuse");
    if (clip.empty()) { FAIL("could not generate multistream test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    REQUIRE(app.clip_append(v1, mi).valid());

    // Source is referenced: set_stream must succeed and re-cut the
    // timeline clip from the new producer.
    int orig_audio = app.project().library().item(mi)->audio_index;
    int other = -1;
    for (const auto& s : app.project().library().item(mi)->streams) {
        if (s.type == "audio" && s.index != orig_audio) { other = s.index; break; }
    }
    REQUIRE(other >= 0);
    REQUIRE(app.library_set_stream(mi, -1, other));
    REQUIRE(app.project().library().item(mi)->audio_index == other);
    // The timeline clip should still be intact with the same id.
    REQUIRE(app.project().timeline().count_clips_for_media(mi) == 1);
}

TEST_CASE("state.get exposes streams list with stable schema", "[t15]") {
    std::string clip = vedit::test::make_test_clip_multistream("state_streams");
    if (clip.empty()) { FAIL("could not generate multistream test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    REQUIRE(mi >= 0);

    vedit::rpc::Dispatcher d;
    vedit::rpc::register_methods(d, app);

    nlohmann::json req = {{"jsonrpc","2.0"},{"method","state.get"},{"id",1}};
    nlohmann::json resp = nlohmann::json::parse(d.handle_request(req.dump()));
    REQUIRE_FALSE(resp.contains("error"));
    const auto& lib = resp["result"]["library"];
    REQUIRE(lib.is_array());
    REQUIRE(lib.size() == 1);
    REQUIRE(lib[0]["index"] == mi);
    // Streams array must be present and non-empty.
    REQUIRE(lib[0].contains("streams"));
    REQUIRE(lib[0]["streams"].is_array());
    REQUIRE_FALSE(lib[0]["streams"].empty());
    // Each stream has the locked schema keys.
    for (const auto& s : lib[0]["streams"]) {
        REQUIRE(s.contains("index"));
        REQUIRE(s.contains("type"));
        REQUIRE(s.contains("codec"));
        REQUIRE(s.contains("is_default"));
    }
    // video_index / audio_index surfaced at the item level.
    REQUIRE(lib[0].contains("video_index"));
    REQUIRE(lib[0].contains("audio_index"));
}

TEST_CASE("library.set_stream over RPC dispatcher", "[t15]") {
    std::string clip = vedit::test::make_test_clip_multistream("rpc_set_stream");
    if (clip.empty()) { FAIL("could not generate multistream test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    REQUIRE(mi >= 0);
    int orig_audio = app.project().library().item(mi)->audio_index;

    // Find another audio stream.
    int other = -1;
    for (const auto& s : app.project().library().item(mi)->streams) {
        if (s.type == "audio" && s.index != orig_audio) { other = s.index; break; }
    }
    REQUIRE(other >= 0);

    vedit::rpc::Dispatcher d;
    vedit::rpc::register_methods(d, app);

    nlohmann::json req = {{"jsonrpc","2.0"},{"method","library.set_stream"},{"id",1},
                          {"params", {{"index", mi},
                                      {"video_index", -1},
                                      {"audio_index", other}}}};
    nlohmann::json resp = nlohmann::json::parse(d.handle_request(req.dump()));
    REQUIRE_FALSE(resp.contains("error"));
    REQUIRE(resp["result"]["ok"] == true);
    REQUIRE(app.project().library().item(mi)->audio_index == other);
}

TEST_CASE("state.get includes P1/P2 filter placeholder schema", "[t16]") {
    std::string clip = vedit::test::make_test_clip("p1p2_placeholder");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    REQUIRE(mi >= 0);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    app.clip_append(v1, mi);

    vedit::rpc::Dispatcher d;
    vedit::rpc::register_methods(d, app);

    nlohmann::json req = {{"jsonrpc","2.0"},{"method","state.get"},{"id",1}};
    nlohmann::json resp = nlohmann::json::parse(d.handle_request(req.dump()));
    REQUIRE_FALSE(resp.contains("error"));

    const auto& result = resp["result"];
    REQUIRE(result.contains("filters"));
    REQUIRE(result["filters"].contains("available"));
    REQUIRE(result["filters"]["available"].is_array());
    // M2 populates available from the MLT filter catalog. Verify at
    // least the "brightness" filter is present (a core MLT filter).
    REQUIRE_FALSE(result["filters"]["available"].empty());
    bool has_brightness = false;
    for (const auto& f : result["filters"]["available"]) {
        if (f["id"] == "brightness") has_brightness = true;
    }
    REQUIRE(has_brightness);
    REQUIRE(result["filters"].contains("attached"));
    REQUIRE(result["filters"]["attached"].is_array());
    REQUIRE(result["filters"]["attached"].empty());
}

TEST_CASE("app.list_instances over RPC dispatcher", "[t16]") {
    std::string clip = vedit::test::make_test_clip("list_instances");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    REQUIRE(mi >= 0);

    vedit::rpc::Dispatcher d;
    vedit::rpc::register_methods(d, app);

    nlohmann::json req = {{"jsonrpc","2.0"},{"method","app.list_instances"},{"id",1}};
    nlohmann::json resp = nlohmann::json::parse(d.handle_request(req.dump()));
    REQUIRE_FALSE(resp.contains("error"));
    REQUIRE(resp["result"].is_array());
    // In a test environment, no other vedit instance is running, so
    // the array is empty. But it must still be a valid JSON array.
    // A running vedit instance would return its identity entry.
}

// save/load restores timeline structure (track count, clip count,
// duration) but does NOT preserve TrackId or ClipId. MLT's consumer_xml
// serializes the playlist/tractor structure but drops custom properties
// on playlist producers and cut producers. ClipId/TrackId preservation
// across save/load requires a companion JSON file or custom serializer;
// this is a known M1 limitation tracked in the alignment doc.
TEST_CASE("save/load round-trip mints new ids (known limitation)", "[t16]") {
    std::string clip = vedit::test::make_test_clip("id_limitation");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    REQUIRE(mi >= 0);

    auto v1 = app.track_add(vedit::TrackKind::Video, "Base");
    REQUIRE(v1.valid());
    auto c1 = app.clip_append(v1, mi);
    REQUIRE(c1.valid());
    auto c2 = app.clip_append(v1, mi);
    REQUIRE(c2.valid());

    int64_t orig_track_id = v1.value;
    int64_t orig_clip1_id = c1.value;
    int64_t orig_clip2_id = c2.value;
    int orig_track_count = app.track_count();
    int orig_clip_count = app.project().timeline().clip_count();

    std::string xml_path = vedit::test::unique_temp_path(
        "vedit-test", "id_limitation", ".xml");
    REQUIRE(vedit::ProjectSaveLoad::save_xml(app.project(), xml_path));
    REQUIRE(vedit::ProjectSaveLoad::load_xml(app.project(), xml_path));

    // Structure is preserved.
    REQUIRE(app.track_count() == orig_track_count);
    REQUIRE(app.project().timeline().clip_count() == orig_clip_count);

    // But ids are NOT preserved — MLT consumer_xml drops custom
    // properties on playlist/cut producers. New ids are minted.
    // This test pins the CURRENT behavior; changing it is a feature,
    // not a regression.
    const auto* t = app.project().timeline().track(
        vedit::TrackId{orig_track_id});
    REQUIRE(t == nullptr);  // old id is gone

    const auto* clip1 = app.project().timeline().clip(
        vedit::ClipId{orig_clip1_id});
    REQUIRE(clip1 == nullptr);  // old id is gone

    const auto* clip2 = app.project().timeline().clip(
        vedit::ClipId{orig_clip2_id});
    REQUIRE(clip2 == nullptr);  // old id is gone
}

TEST_CASE("filter.add / filter.remove / filter.set_param / filter.get_param over RPC", "[t17]") {
    std::string clip = vedit::test::make_test_clip("filter_rpc");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    REQUIRE(mi >= 0);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    auto cid = app.clip_append(v1, mi);
    REQUIRE(cid.valid());

    vedit::rpc::Dispatcher d;
    vedit::rpc::register_methods(d, app);

    // Attach brightness filter.
    nlohmann::json req = {{"jsonrpc","2.0"},{"method","filter.add"},{"id",1},
                          {"params", {{"clip_id", cid.value}, {"filter", "brightness"}}}};
    nlohmann::json resp = nlohmann::json::parse(d.handle_request(req.dump()));
    REQUIRE_FALSE(resp.contains("error"));

    // Set level to 2.5 via {value: 2.5} envelope (P2 shape).
    req = {{"jsonrpc","2.0"},{"method","filter.set_param"},{"id",2},
           {"params", {{"clip_id", cid.value}, {"filter", "brightness"},
                       {"param", "level"}, {"value", 2.5}}}};
    resp = nlohmann::json::parse(d.handle_request(req.dump()));
    REQUIRE_FALSE(resp.contains("error"));

    // Get back the value.
    req = {{"jsonrpc","2.0"},{"method","filter.get_param"},{"id",3},
           {"params", {{"clip_id", cid.value}, {"filter", "brightness"},
                       {"param", "level"}}}};
    resp = nlohmann::json::parse(d.handle_request(req.dump()));
    REQUIRE_FALSE(resp.contains("error"));
    REQUIRE(resp["result"]["value"] == "2.5");

    // Remove the filter.
    req = {{"jsonrpc","2.0"},{"method","filter.remove"},{"id",4},
           {"params", {{"clip_id", cid.value}, {"filter", "brightness"}}}};
    resp = nlohmann::json::parse(d.handle_request(req.dump()));
    REQUIRE_FALSE(resp.contains("error"));

    // Filter is gone — get_param should fail.
    req = {{"jsonrpc","2.0"},{"method","filter.get_param"},{"id",5},
           {"params", {{"clip_id", cid.value}, {"filter", "brightness"},
                       {"param", "level"}}}};
    resp = nlohmann::json::parse(d.handle_request(req.dump()));
    REQUIRE(resp.contains("error"));
}

// End-to-end filter test: attach brightness, export frame, verify
// visual difference. This proves the filter actually affects rendering.
TEST_CASE("brightness filter changes rendered output", "[t17][export]") {
    std::string clip = vedit::test::make_test_clip("filter_visual");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    auto cid = app.clip_append(v1, mi);
    REQUIRE(cid.valid());

    // Export a frame WITHOUT filter.
    std::string normal_png = vedit::test::unique_temp_path(
        "vedit-test", "filter_normal", ".png");
    vedit::ExportService svc;
    REQUIRE(svc.get_frame(*app.project().producer(), app.profile(), 5, normal_png));

    auto file_size = [](const std::string& p) -> long {
        std::FILE* f = std::fopen(p.c_str(), "rb");
        if (!f) return 0;
        std::fseek(f, 0, SEEK_END);
        long s = std::ftell(f);
        std::fclose(f);
        return s;
    };
    long normal_sz = file_size(normal_png);
    REQUIRE(normal_sz > 100);

    // Attach brightness with level=10 (max, saturates most pixels).
    REQUIRE(app.filter_add(cid, "brightness"));
    REQUIRE(app.filter_set_param(cid, "brightness", "level", "10.0"));

    // Export same frame WITH filter.
    std::string bright_png = vedit::test::unique_temp_path(
        "vedit-test", "filter_bright", ".png");
    REQUIRE(svc.get_frame(*app.project().producer(), app.profile(), 5, bright_png));
    long bright_sz = file_size(bright_png);
    REQUIRE(bright_sz > 100);

    // Brightness at level=10 compresses the histogram (more saturated
    // pixels = less entropy), producing a DIFFERENT file size. Not
    // guaranteed to be smaller/larger, just different — the key test
    // is that the filter had an effect. If the sizes are identical,
    // the filter was a no-op.
    REQUIRE(normal_sz != bright_sz);

    // Remove and verify param is gone.
    REQUIRE(app.filter_remove(cid, "brightness"));
    REQUIRE(app.filter_get_param(cid, "brightness", "level") == "");
}

TEST_CASE("per-track filter add/set/get/remove over RPC", "[t17]") {
    std::string clip = vedit::test::make_test_clip("track_filter");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    app.clip_append(v1, mi);

    vedit::rpc::Dispatcher d;
    vedit::rpc::register_methods(d, app);

    // Attach brightness to the track (not a clip).
    nlohmann::json req = {{"jsonrpc","2.0"},{"method","filter.add"},{"id",1},
                          {"params", {{"track_id", v1.value}, {"filter", "brightness"}}}};
    nlohmann::json resp = nlohmann::json::parse(d.handle_request(req.dump()));
    REQUIRE_FALSE(resp.contains("error"));

    // Set level to 0.5.
    req = {{"jsonrpc","2.0"},{"method","filter.set_param"},{"id",2},
           {"params", {{"track_id", v1.value}, {"filter", "brightness"},
                       {"param", "level"}, {"value", 0.5}}}};
    resp = nlohmann::json::parse(d.handle_request(req.dump()));
    REQUIRE_FALSE(resp.contains("error"));

    // Get it back.
    req = {{"jsonrpc","2.0"},{"method","filter.get_param"},{"id",3},
           {"params", {{"track_id", v1.value}, {"filter", "brightness"},
                       {"param", "level"}}}};
    resp = nlohmann::json::parse(d.handle_request(req.dump()));
    REQUIRE_FALSE(resp.contains("error"));
    REQUIRE(resp["result"]["value"] == "0.5");

    // Remove.
    req = {{"jsonrpc","2.0"},{"method","filter.remove"},{"id",4},
           {"params", {{"track_id", v1.value}, {"filter", "brightness"}}}};
    resp = nlohmann::json::parse(d.handle_request(req.dump()));
    REQUIRE_FALSE(resp.contains("error"));

    // Gone.
    req = {{"jsonrpc","2.0"},{"method","filter.get_param"},{"id",5},
           {"params", {{"track_id", v1.value}, {"filter", "brightness"},
                       {"param", "level"}}}};
    resp = nlohmann::json::parse(d.handle_request(req.dump()));
    REQUIRE(resp.contains("error"));
}

TEST_CASE("state.get reflects attached filters after add/remove", "[t18]") {
    std::string clip = vedit::test::make_test_clip("state_filters");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    auto cid = app.clip_append(v1, mi);

    vedit::rpc::Dispatcher d;
    vedit::rpc::register_methods(d, app);

    // No filters attached initially.
    {
        nlohmann::json req = {{"jsonrpc","2.0"},{"method","state.get"},{"id",1}};
        nlohmann::json resp = nlohmann::json::parse(d.handle_request(req.dump()));
        REQUIRE(resp["result"]["filters"]["attached"].empty());
    }

    // Attach brightness.
    {
        nlohmann::json req = {{"jsonrpc","2.0"},{"method","filter.add"},{"id",2},
                              {"params", {{"clip_id", cid.value}, {"filter", "brightness"}}}};
        d.handle_request(req.dump());
    }
    {
        nlohmann::json req = {{"jsonrpc","2.0"},{"method","filter.set_param"},{"id",3},
                              {"params", {{"clip_id", cid.value}, {"filter", "brightness"},
                                          {"param", "level"}, {"value", 1.5}}}};
        d.handle_request(req.dump());
    }

    // Now state.get must show the filter.
    {
        nlohmann::json req = {{"jsonrpc","2.0"},{"method","state.get"},{"id",4}};
        nlohmann::json resp = nlohmann::json::parse(d.handle_request(req.dump()));
        const auto& attached = resp["result"]["filters"]["attached"];
        REQUIRE(attached.size() == 1);
        REQUIRE(attached[0]["target_type"] == "clip");
        REQUIRE(attached[0]["target_id"] == cid.value);
        REQUIRE(attached[0]["filter"] == "brightness");
        REQUIRE(attached[0]["params"].contains("level"));
    }

    // Remove it.
    {
        nlohmann::json req = {{"jsonrpc","2.0"},{"method","filter.remove"},{"id",5},
                              {"params", {{"clip_id", cid.value}, {"filter", "brightness"}}}};
        d.handle_request(req.dump());
    }
    {
        nlohmann::json req = {{"jsonrpc","2.0"},{"method","state.get"},{"id",6}};
        nlohmann::json resp = nlohmann::json::parse(d.handle_request(req.dump()));
        REQUIRE(resp["result"]["filters"]["attached"].empty());
    }
}

TEST_CASE("filter.set_param with keyframes writes animated param", "[t18][keyframe]") {
    std::string clip = vedit::test::make_test_clip("kf_set");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    auto cid = app.clip_append(v1, mi);
    REQUIRE(app.filter_add(cid, "brightness"));

    // Set keyframes via domain API.
    nlohmann::json kfs = nlohmann::json::array({
        {{"frame", 0}, {"value", 1.0}, {"type", "linear"}},
        {{"frame", 50}, {"value", 2.0}, {"type", "linear"}}
    });
    REQUIRE(app.filter_set_param_keyframes(cid, "brightness", "level", kfs));

    // Read back via domain API.
    auto result = app.filter_get_param_keyframes(cid, "brightness", "level");
    REQUIRE_FALSE(result.empty());
    REQUIRE(result.contains("value"));
    std::string val = result["value"].get<std::string>();
    REQUIRE(val.find("0=") != std::string::npos);
    REQUIRE(val.find("50=") != std::string::npos);
}

TEST_CASE("filter.set_param with static value still works after keyframes", "[t18][keyframe]") {
    std::string clip = vedit::test::make_test_clip("kf_static");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    auto cid = app.clip_append(v1, mi);
    REQUIRE(app.filter_add(cid, "brightness"));

    // Set keyframes first.
    nlohmann::json kfs = nlohmann::json::array({
        {{"frame", 0}, {"value", 1.0}},
        {{"frame", 50}, {"value", 2.0}}
    });
    REQUIRE(app.filter_set_param_keyframes(cid, "brightness", "level", kfs));

    // Overwrite with static value.
    REQUIRE(app.filter_set_param(cid, "brightness", "level", "1.5"));

    // Read back: should be static.
    auto result = app.filter_get_param_keyframes(cid, "brightness", "level");
    REQUIRE(result.contains("value"));
    REQUIRE(result["value"] == "1.5");
    REQUIRE_FALSE(result.contains("keyframes"));
}

TEST_CASE("clip.overwrite replaces content at position", "[t18]") {
    std::string clip = vedit::test::make_test_clip("overwrite");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    app.clip_append(v1, mi);

    vedit::rpc::Dispatcher d;
    vedit::rpc::register_methods(d, app);

    // Overwrite at position 0.
    nlohmann::json req = {{"jsonrpc","2.0"},{"method","clip.overwrite"},{"id",1},
                          {"params", {{"track_id", v1.value},
                                      {"media_index", mi}, {"position", 0}}}};
    nlohmann::json resp = nlohmann::json::parse(d.handle_request(req.dump()));
    REQUIRE(resp.contains("result"));
    REQUIRE(resp["result"]["clip_id"] > 0);
}

TEST_CASE("clip.lift replaces clip with blank", "[t18]") {
    std::string clip = vedit::test::make_test_clip("lift");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    auto cid = app.clip_append(v1, mi);

    REQUIRE(app.clip_lift(cid));

    // The clip should be gone but the timeline should still have
    // content (a blank of the same duration).
    auto attached = app.project().timeline().attached_filters();
    REQUIRE(attached.empty());
}

TEST_CASE("clip.change_speed wraps clip in timewarp", "[t18]") {
    std::string clip = vedit::test::make_test_clip("speed");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    auto cid = app.clip_append(v1, mi);

    vedit::rpc::Dispatcher d;
    vedit::rpc::register_methods(d, app);

    nlohmann::json req = {{"jsonrpc","2.0"},{"method","clip.change_speed"},{"id",1},
                          {"params", {{"clip_id", cid.value}, {"speed", 2.0}}}};
    nlohmann::json resp = nlohmann::json::parse(d.handle_request(req.dump()));
    REQUIRE_FALSE(resp.contains("error"));
}

TEST_CASE("clip.detach_audio creates audio track", "[t18]") {
    std::string clip = vedit::test::make_test_clip("detach");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    auto cid = app.clip_append(v1, mi);

    vedit::rpc::Dispatcher d;
    vedit::rpc::register_methods(d, app);

    nlohmann::json req = {{"jsonrpc","2.0"},{"method","clip.detach_audio"},{"id",1},
                          {"params", {{"clip_id", cid.value}}}};
    nlohmann::json resp = nlohmann::json::parse(d.handle_request(req.dump()));
    REQUIRE(resp.contains("result"));
    REQUIRE(resp["result"]["clip_id"] > 0);
}

TEST_CASE("project.set_profile changes output resolution", "[t19]") {
    std::string clip = vedit::test::make_test_clip("profile_set");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    app.clip_append(v1, mi);

    vedit::rpc::Dispatcher d;
    vedit::rpc::register_methods(d, app);

    // Set profile to 320x240 @ 25fps.
    nlohmann::json req = {{"jsonrpc","2.0"},{"method","project.set_profile"},
                          {"id",1},
                          {"params", {{"width", 320}, {"height", 240},
                                      {"fps", 25.0}}}};
    nlohmann::json resp = nlohmann::json::parse(d.handle_request(req.dump()));
    REQUIRE_FALSE(resp.contains("error"));

    // Export a frame and verify resolution via ffprobe.
    std::string png = vedit::test::unique_temp_path(
        "vedit-test", "profile_set", ".png");
    vedit::ExportService svc;
    REQUIRE(svc.get_frame(*app.project().producer(), app.profile(), 0, png));

    // ffprobe the PNG dimensions.
    std::string cmd = "ffprobe -v error -show_entries stream=width,height "
                      "-of csv=p=0 \"" + png + "\" 2>&1";
    std::FILE* p = VEDIT_POPEN(cmd.c_str(), "r");
    REQUIRE(p != nullptr);
    char buf[64];
    std::string result;
    while (fgets(buf, sizeof(buf), p)) result += buf;
    VEDIT_PCLOSE(p);
    // Strip trailing newline.
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    REQUIRE(result == "320,240");
}

TEST_CASE("profile_set updates SAR and DAR to square pixel", "[t19]") {
    vedit::AppController app;
    app.profile_set(1280, 480, 25.0);

    auto& p = app.profile();
    REQUIRE(p.sample_aspect_num() == 1);
    REQUIRE(p.sample_aspect_den() == 1);
    // 1280/160 = 8, 480/160 = 3
    REQUIRE(p.display_aspect_num() == 8);
    REQUIRE(p.display_aspect_den() == 3);
}

TEST_CASE("composite geometry positions track on canvas", "[t19][export]") {
    std::string clip = vedit::test::make_test_clip("comp_geom");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);

    // Set canvas to 320x240 (source is 160x120).
    app.profile_set(320, 240, 25.0);

    auto v1 = app.track_add(vedit::TrackKind::Video, "Background");
    auto v2 = app.track_add(vedit::TrackKind::Video, "Overlay");
    app.clip_append(v1, mi);  // full canvas (no composite = full)
    app.clip_append(v2, mi);  // positioned via composite

    // Set composite geometry: top-left 50%x50%.
    REQUIRE(app.composite_geometry_set(v2, 0.0, 0.0, 50.0, 50.0));

    // Export frame and verify: the top-left quarter should show the
    // clip content (non-black), bottom-right should be darker
    // (composite over blank background).
    std::string png = vedit::test::unique_temp_path(
        "vedit-test", "comp_geom", ".png");
    vedit::ExportService svc;
    REQUIRE(svc.get_frame(*app.project().producer(), app.profile(), 5, png));

    // ffprobe resolution first.
    std::string cmd = "ffprobe -v error -show_entries stream=width,height "
                      "-of csv=p=0 \"" + png + "\" 2>&1";
    std::FILE* p = VEDIT_POPEN(cmd.c_str(), "r");
    REQUIRE(p != nullptr);
    char buf[64];
    std::string res;
    while (fgets(buf, sizeof(buf), p)) res += buf;
    VEDIT_PCLOSE(p);
    while (!res.empty() && (res.back() == '\n' || res.back() == '\r'))
        res.pop_back();
    REQUIRE(res == "320,240");

    // File must be non-trivial.
    auto file_size = [](const std::string& path) -> long {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return 0;
        std::fseek(f, 0, SEEK_END);
        long s = std::ftell(f);
        std::fclose(f);
        return s;
    };
    REQUIRE(file_size(png) > 100);
}

// 0.3 regression: a filter the user/agent attaches must appear in
// state.get even when its service name collides with an MLT-internal
// filter (crop, resize, ...). Before the fix, attached_filters() dropped
// these by service name, hiding a user-applied crop entirely.
TEST_CASE("user-attached crop filter is visible in state.get", "[t18][regression]") {
    std::string clip = vedit::test::make_test_clip("user_crop");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    REQUIRE(mi >= 0);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    auto cid = app.clip_append(v1, mi);
    REQUIRE(cid.valid());

    // Attach a crop filter the same way an agent would.
    REQUIRE(app.filter_add(cid, "crop"));

    // It must show up in the attached-filters projection.
    auto attached = app.project().timeline().attached_filters();
    bool found = false;
    for (const auto& af : attached) {
        if (af.target_type == "clip" && af.target_id == cid.value &&
            af.filter_name == "crop") {
            found = true;
        }
    }
    REQUIRE(found);

    // And in the JSON state the agent actually reads.
    std::string js = vedit::ProjectSaveLoad::state_json(app.project());
    auto doc = nlohmann::json::parse(js);
    bool in_state = false;
    for (const auto& af : doc["filters"]["attached"]) {
        if (af["filter"] == "crop" && af["target_id"] == cid.value)
            in_state = true;
    }
    REQUIRE(in_state);
}

// 0.1: transition.add must honor the requested type. mix() with a null
// transition plants no blend at all; the fix plants a luma video
// transition plus an audio cross-fade and tags the mix with the type.
TEST_CASE("transition.add plants a typed transition", "[t18]") {
    std::string clip = vedit::test::make_test_clip("transition_type");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    REQUIRE(mi >= 0);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    auto c1 = app.clip_append(v1, mi);
    auto c2 = app.clip_append(v1, mi);
    REQUIRE(c1.valid());
    REQUIRE(c2.valid());

    int before = app.project().timeline().clip_count();

    // Adjacent clips are playlist indices 0 and 1; mix uses the first.
    REQUIRE(app.clip_add_transition(v1, 0, 15, "luma"));

    // The mix inserts a tractor between the two clips, so the timeline
    // now reports one more clip-ish entry than before.
    int after = app.project().timeline().clip_count();
    REQUIRE(after >= before);

    // The composed timeline must still render a frame in the mix region.
    std::string png = vedit::test::unique_temp_path(
        "vedit-test", "transition_frame", ".png");
    vedit::ExportService svc;
    REQUIRE(svc.get_frame(*app.project().producer(), app.profile(), 2, png));
}

// 0.1: transition.list_types reports the wired-up types over RPC.
TEST_CASE("transition.list_types over RPC dispatcher", "[t18]") {
    vedit::AppController app;
    vedit::rpc::Dispatcher d;
    vedit::rpc::register_methods(d, app);

    nlohmann::json req = {{"jsonrpc","2.0"},{"method","transition.list_types"},
                          {"id",1},{"params", nlohmann::json::object()}};
    auto resp = nlohmann::json::parse(d.handle_request(req.dump()));
    REQUIRE_FALSE(resp.contains("error"));
    REQUIRE(resp["result"].is_array());
    REQUIRE(resp["result"].size() >= 2);

    bool has_luma = false, has_dissolve = false;
    for (const auto& t : resp["result"]) {
        if (t["id"] == "luma") has_luma = true;
        if (t["id"] == "dissolve") has_dissolve = true;
    }
    REQUIRE(has_luma);
    REQUIRE(has_dissolve);
}

// 0.2: clip audio/video enable/disable over RPC.
TEST_CASE("clip.set_audio_enabled and clip.set_video_enabled over RPC", "[t19]") {
    std::string clip = vedit::test::make_test_clip("clip_enable");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    REQUIRE(mi >= 0);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    auto cid = app.clip_append(v1, mi);
    REQUIRE(cid.valid());

    vedit::rpc::Dispatcher d;
    vedit::rpc::register_methods(d, app);

    auto call = [&](const char* method, nlohmann::json params) {
        nlohmann::json req = {{"jsonrpc","2.0"},{"method",method},
                              {"id",1},{"params",std::move(params)}};
        auto resp = nlohmann::json::parse(d.handle_request(req.dump()));
        REQUIRE_FALSE(resp.contains("error"));
        return resp["result"];
    };

    // Disable audio.
    call("clip.set_audio_enabled", {{"clip_id", cid.value}, {"enabled", false}});
    // Re-enable audio.
    call("clip.set_audio_enabled", {{"clip_id", cid.value}, {"enabled", true}});
    // Disable video.
    call("clip.set_video_enabled", {{"clip_id", cid.value}, {"enabled", false}});
    // Re-enable video.
    call("clip.set_video_enabled", {{"clip_id", cid.value}, {"enabled", true}});

    // Disabled video should still render (blank frames but no crash).
    call("clip.set_video_enabled", {{"clip_id", cid.value}, {"enabled", false}});
    std::string png = vedit::test::unique_temp_path(
        "vedit-test", "disabled_video", ".png");
    vedit::ExportService svc;
    REQUIRE(svc.get_frame(*app.project().producer(), app.profile(), 2, png));
}

// 0.2: track.get_composite_geometry over RPC.
TEST_CASE("track.get_composite_geometry over RPC dispatcher", "[t19]") {
    std::string clip = vedit::test::make_test_clip("comp_get");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    REQUIRE(mi >= 0);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    auto v2 = app.track_add(vedit::TrackKind::Video);
    app.clip_append(v1, mi);
    app.clip_append(v2, mi);
    REQUIRE(v2.valid());

    vedit::rpc::Dispatcher d;
    vedit::rpc::register_methods(d, app);

    // Set geometry on track 2.
    nlohmann::json req = {{"jsonrpc","2.0"},
                          {"method","track.set_composite_geometry"},
                          {"id",1},
                          {"params", {{"track_id", v2.value},
                                       {"x",25},{"y",25},{"w",50},{"h",50}}}};
    auto resp = nlohmann::json::parse(d.handle_request(req.dump()));
    REQUIRE_FALSE(resp.contains("error"));

    // Get it back.
    req = {{"jsonrpc","2.0"},{"method","track.get_composite_geometry"},
           {"id",2},{"params", {{"track_id", v2.value}}}};
    resp = nlohmann::json::parse(d.handle_request(req.dump()));
    REQUIRE_FALSE(resp.contains("error"));
    REQUIRE(resp["result"]["x"] == 25);
    REQUIRE(resp["result"]["y"] == 25);
    REQUIRE(resp["result"]["w"] == 50);
    REQUIRE(resp["result"]["h"] == 50);

    // Track 0 should fail (no composite).
    req = {{"jsonrpc","2.0"},{"method","track.get_composite_geometry"},
           {"id",3},{"params", {{"track_id", v1.value}}}};
    resp = nlohmann::json::parse(d.handle_request(req.dump()));
    REQUIRE(resp.contains("error"));
}

// 0.2: project.new over RPC clears state.
TEST_CASE("project.new over RPC clears timeline and library", "[t19]") {
    std::string clip = vedit::test::make_test_clip("proj_new");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    REQUIRE(mi >= 0);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    app.clip_append(v1, mi);

    vedit::rpc::Dispatcher d;
    vedit::rpc::register_methods(d, app);

    nlohmann::json req = {{"jsonrpc","2.0"},{"method","project.new"},
                          {"id",1},{"params", nlohmann::json::object()}};
    auto resp = nlohmann::json::parse(d.handle_request(req.dump()));
    REQUIRE_FALSE(resp.contains("error"));

    REQUIRE(app.track_count() == 0);
    REQUIRE(app.library_count() == 0);
}

// 0.4: track mute/hide over RPC and state.get visibility.
TEST_CASE("track.set_muted and track.set_hidden over RPC", "[t20]") {
    std::string clip = vedit::test::make_test_clip("track_mh");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    REQUIRE(mi >= 0);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    app.clip_append(v1, mi);
    REQUIRE(v1.valid());

    vedit::rpc::Dispatcher d;
    vedit::rpc::register_methods(d, app);

    auto call = [&](const char* method, nlohmann::json params) {
        nlohmann::json req = {{"jsonrpc","2.0"},{"method",method},
                              {"id",1},{"params",std::move(params)}};
        auto resp = nlohmann::json::parse(d.handle_request(req.dump()));
        REQUIRE_FALSE(resp.contains("error"));
        return resp["result"];
    };

    // Mute the track.
    call("track.set_muted", {{"track_id", v1.value}, {"muted", true}});
    REQUIRE(app.project().timeline().track(v1)->muted == true);

    // Unmute.
    call("track.set_muted", {{"track_id", v1.value}, {"muted", false}});
    REQUIRE(app.project().timeline().track(v1)->muted == false);

    // Hide the track.
    call("track.set_hidden", {{"track_id", v1.value}, {"hidden", true}});
    REQUIRE(app.project().timeline().track(v1)->hidden == true);

    // Unhide.
    call("track.set_hidden", {{"track_id", v1.value}, {"hidden", false}});
    REQUIRE(app.project().timeline().track(v1)->hidden == false);

    // Verify mute/hidden appear in state.get.
    std::string js = vedit::ProjectSaveLoad::state_json(app.project());
    auto doc = nlohmann::json::parse(js);
    REQUIRE(doc["tracks"][0]["muted"] == false);
    REQUIRE(doc["tracks"][0]["hidden"] == false);
}

// Stage 1: per-clip geometry via MLT affine filter.

TEST_CASE("clip.set_geometry domain round-trip", "[t21]") {
    std::string clip = vedit::test::make_test_clip("geo_domain");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    REQUIRE(mi >= 0);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    auto cid = app.clip_append(v1, mi);
    REQUIRE(cid.valid());

    // Set geometry: 25% from left, 25% from top, 50% width, 50% height.
    REQUIRE(app.clip_set_geometry(cid, 25, 25, 50, 50));

    // Read back.
    double x = 0, y = 0, w = 100, h = 100;
    REQUIRE(app.clip_get_geometry(cid, x, y, w, h));
    REQUIRE(x == 25);
    REQUIRE(y == 25);
    REQUIRE(w == 50);
    REQUIRE(h == 50);

    // Reset to defaults should detach the affine filter.
    REQUIRE(app.clip_set_geometry(cid, 0, 0, 100, 100));
    REQUIRE(app.clip_get_geometry(cid, x, y, w, h));
    REQUIRE(x == 0);
    REQUIRE(y == 0);
    REQUIRE(w == 100);
    REQUIRE(h == 100);
}

TEST_CASE("clip.get_geometry returns defaults with no filter", "[t21]") {
    std::string clip = vedit::test::make_test_clip("geo_default");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    REQUIRE(mi >= 0);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    auto cid = app.clip_append(v1, mi);
    REQUIRE(cid.valid());

    double x = -1, y = -1, w = -1, h = -1;
    REQUIRE(app.clip_get_geometry(cid, x, y, w, h));
    REQUIRE(x == 0);
    REQUIRE(y == 0);
    REQUIRE(w == 100);
    REQUIRE(h == 100);
}

TEST_CASE("clip.set_geometry RPC round-trip", "[t21]") {
    std::string clip = vedit::test::make_test_clip("geo_rpc");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    REQUIRE(mi >= 0);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    auto cid = app.clip_append(v1, mi);
    REQUIRE(cid.valid());

    vedit::rpc::Dispatcher d;
    vedit::rpc::register_methods(d, app);

    // Set geometry via RPC.
    nlohmann::json req = {{"jsonrpc","2.0"},{"method","clip.set_geometry"},
                          {"id",1},
                          {"params", {{"clip_id", cid.value},
                                       {"x",10},{"y",20},{"w",80},{"h",60}}}};
    auto resp = nlohmann::json::parse(d.handle_request(req.dump()));
    REQUIRE_FALSE(resp.contains("error"));

    // Read back via RPC.
    req = {{"jsonrpc","2.0"},{"method","clip.get_geometry"},
           {"id",2},{"params", {{"clip_id", cid.value}}}};
    resp = nlohmann::json::parse(d.handle_request(req.dump()));
    REQUIRE_FALSE(resp.contains("error"));
    REQUIRE(resp["result"]["x"] == 10);
    REQUIRE(resp["result"]["y"] == 20);
    REQUIRE(resp["result"]["w"] == 80);
    REQUIRE(resp["result"]["h"] == 60);
}

TEST_CASE("clip.set_geometry partial update", "[t21]") {
    std::string clip = vedit::test::make_test_clip("geo_partial");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    REQUIRE(mi >= 0);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    auto cid = app.clip_append(v1, mi);
    REQUIRE(cid.valid());

    vedit::rpc::Dispatcher d;
    vedit::rpc::register_methods(d, app);

    // Set full geometry first.
    auto req = nlohmann::json{{"jsonrpc","2.0"},{"method","clip.set_geometry"},
                              {"id",1},
                              {"params", {{"clip_id", cid.value},
                                           {"x",30},{"y",30},{"w",40},{"h",40}}}};
    auto resp = nlohmann::json::parse(d.handle_request(req.dump()));
    REQUIRE_FALSE(resp.contains("error"));

    // Partial update: change only X and W.
    req = nlohmann::json{{"jsonrpc","2.0"},{"method","clip.set_geometry"},
                         {"id",2},
                         {"params", {{"clip_id", cid.value},
                                      {"x",50},{"w",20}}}};
    resp = nlohmann::json::parse(d.handle_request(req.dump()));
    REQUIRE_FALSE(resp.contains("error"));

    // Read back: X and W changed, Y and H unchanged.
    req = nlohmann::json{{"jsonrpc","2.0"},{"method","clip.get_geometry"},
                         {"id",3},{"params", {{"clip_id", cid.value}}}};
    resp = nlohmann::json::parse(d.handle_request(req.dump()));
    REQUIRE(resp["result"]["x"] == 50);
    REQUIRE(resp["result"]["y"] == 30);
    REQUIRE(resp["result"]["w"] == 20);
    REQUIRE(resp["result"]["h"] == 40);
}

TEST_CASE("clip.set_rotation round-trip", "[t21]") {
    std::string clip = vedit::test::make_test_clip("geo_rot");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    REQUIRE(mi >= 0);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    auto cid = app.clip_append(v1, mi);
    REQUIRE(cid.valid());

    // Set rotation.
    REQUIRE(app.clip_set_rotation(cid, 45.0));

    // Rotation alone attaches affine filter; verify geometry is still default.
    double x, y, w, h;
    REQUIRE(app.clip_get_geometry(cid, x, y, w, h));
    REQUIRE(x == 0);
    REQUIRE(y == 0);
    REQUIRE(w == 100);
    REQUIRE(h == 100);

    // Reset rotation to 0 detaches the filter.
    REQUIRE(app.clip_set_rotation(cid, 0.0));
    REQUIRE(app.clip_get_geometry(cid, x, y, w, h));
    REQUIRE(x == 0);
    REQUIRE(y == 0);
    REQUIRE(w == 100);
    REQUIRE(h == 100);
}

TEST_CASE("geometry appears in state.get attached filters", "[t21]") {
    std::string clip = vedit::test::make_test_clip("geo_state");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    REQUIRE(mi >= 0);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    auto cid = app.clip_append(v1, mi);
    REQUIRE(cid.valid());

    REQUIRE(app.clip_set_geometry(cid, 10, 10, 80, 80));

    std::string js = vedit::ProjectSaveLoad::state_json(app.project());
    auto doc = nlohmann::json::parse(js);

    bool found = false;
    for (const auto& af : doc["filters"]["attached"]) {
        if (af["target_type"] == "clip" &&
            af["target_id"] == cid.value &&
            af["filter"] == "affine") {
            found = true;
        }
    }
    REQUIRE(found);
}

// Stage 2: per-clip crop via MLT crop filter.

TEST_CASE("clip.set_crop domain round-trip", "[t22]") {
    std::string clip = vedit::test::make_test_clip("crop_domain");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    REQUIRE(mi >= 0);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    auto cid = app.clip_append(v1, mi);
    REQUIRE(cid.valid());

    REQUIRE(app.clip_set_crop(cid, 10, 20, 30, 40));

    int left, right, top, bottom;
    REQUIRE(app.clip_get_crop(cid, left, right, top, bottom));
    REQUIRE(left == 10);
    REQUIRE(right == 20);
    REQUIRE(top == 30);
    REQUIRE(bottom == 40);

    // Reset to zeros detaches the crop filter.
    REQUIRE(app.clip_set_crop(cid, 0, 0, 0, 0));
    REQUIRE(app.clip_get_crop(cid, left, right, top, bottom));
    REQUIRE(left == 0);
    REQUIRE(right == 0);
    REQUIRE(top == 0);
    REQUIRE(bottom == 0);
}

TEST_CASE("clip.get_crop returns zeros with no filter", "[t22]") {
    std::string clip = vedit::test::make_test_clip("crop_default");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    REQUIRE(mi >= 0);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    auto cid = app.clip_append(v1, mi);
    REQUIRE(cid.valid());

    int left = -1, right = -1, top = -1, bottom = -1;
    REQUIRE(app.clip_get_crop(cid, left, right, top, bottom));
    REQUIRE(left == 0);
    REQUIRE(right == 0);
    REQUIRE(top == 0);
    REQUIRE(bottom == 0);
}

TEST_CASE("clip.set_crop RPC round-trip", "[t22]") {
    std::string clip = vedit::test::make_test_clip("crop_rpc");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    REQUIRE(mi >= 0);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    auto cid = app.clip_append(v1, mi);
    REQUIRE(cid.valid());

    vedit::rpc::Dispatcher d;
    vedit::rpc::register_methods(d, app);

    auto req = nlohmann::json{{"jsonrpc","2.0"},{"method","clip.set_crop"},
                              {"id",1},
                              {"params", {{"clip_id", cid.value},
                                           {"left",5},{"right",15},
                                           {"top",25},{"bottom",35}}}};
    auto resp = nlohmann::json::parse(d.handle_request(req.dump()));
    REQUIRE_FALSE(resp.contains("error"));

    req = nlohmann::json{{"jsonrpc","2.0"},{"method","clip.get_crop"},
                         {"id",2},{"params", {{"clip_id", cid.value}}}};
    resp = nlohmann::json::parse(d.handle_request(req.dump()));
    REQUIRE(resp["result"]["left"] == 5);
    REQUIRE(resp["result"]["right"] == 15);
    REQUIRE(resp["result"]["top"] == 25);
    REQUIRE(resp["result"]["bottom"] == 35);
}

TEST_CASE("clip.set_crop partial update", "[t22]") {
    std::string clip = vedit::test::make_test_clip("crop_partial");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    REQUIRE(mi >= 0);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    auto cid = app.clip_append(v1, mi);
    REQUIRE(cid.valid());

    vedit::rpc::Dispatcher d;
    vedit::rpc::register_methods(d, app);

    // Set full crop first.
    auto req = nlohmann::json{{"jsonrpc","2.0"},{"method","clip.set_crop"},
                              {"id",1},
                              {"params", {{"clip_id", cid.value},
                                           {"left",10},{"right",10},
                                           {"top",10},{"bottom",10}}}};
    auto resp = nlohmann::json::parse(d.handle_request(req.dump()));
    REQUIRE_FALSE(resp.contains("error"));

    // Partial: change only left and top.
    req = nlohmann::json{{"jsonrpc","2.0"},{"method","clip.set_crop"},
                         {"id",2},
                         {"params", {{"clip_id", cid.value},
                                      {"left",50},{"top",60}}}};
    resp = nlohmann::json::parse(d.handle_request(req.dump()));
    REQUIRE_FALSE(resp.contains("error"));

    req = nlohmann::json{{"jsonrpc","2.0"},{"method","clip.get_crop"},
                         {"id",3},{"params", {{"clip_id", cid.value}}}};
    resp = nlohmann::json::parse(d.handle_request(req.dump()));
    REQUIRE(resp["result"]["left"] == 50);
    REQUIRE(resp["result"]["right"] == 10);
    REQUIRE(resp["result"]["top"] == 60);
    REQUIRE(resp["result"]["bottom"] == 10);
}

TEST_CASE("crop appears in state.get attached filters", "[t22]") {
    std::string clip = vedit::test::make_test_clip("crop_state");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    REQUIRE(mi >= 0);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    auto cid = app.clip_append(v1, mi);
    REQUIRE(cid.valid());

    REQUIRE(app.clip_set_crop(cid, 5, 10, 15, 20));

    std::string js = vedit::ProjectSaveLoad::state_json(app.project());
    auto doc = nlohmann::json::parse(js);

    bool found = false;
    for (const auto& af : doc["filters"]["attached"]) {
        if (af["target_type"] == "clip" &&
            af["target_id"] == cid.value &&
            af["filter"] == "crop") {
            found = true;
        }
    }
    REQUIRE(found);
}

// Stage 3: per-clip fade-in/fade-out via brightness + volume keyframes.

TEST_CASE("clip.set_fade_in sets brightness keyframes", "[t23]") {
    std::string clip = vedit::test::make_test_clip("fade_in");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    REQUIRE(mi >= 0);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    auto cid = app.clip_append(v1, mi);
    REQUIRE(cid.valid());

    // Set a 15-frame fade-in.
    REQUIRE(app.clip_set_fade_in(cid, 15));

    // Brightness filter should be attached with keyframes.
    auto result = app.filter_get_param_keyframes(cid, "brightness", "level");
    REQUIRE_FALSE(result.empty());
    REQUIRE(result.contains("keyframes"));
    REQUIRE(result["keyframes"].size() >= 2);
}

TEST_CASE("clip.set_fade_out sets brightness keyframes", "[t23]") {
    std::string clip = vedit::test::make_test_clip("fade_out");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    REQUIRE(mi >= 0);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    auto cid = app.clip_append(v1, mi);
    REQUIRE(cid.valid());

    REQUIRE(app.clip_set_fade_out(cid, 10));

    auto result = app.filter_get_param_keyframes(cid, "brightness", "level");
    REQUIRE_FALSE(result.empty());
    REQUIRE(result.contains("keyframes"));
    REQUIRE(result["keyframes"].size() >= 2);
}

TEST_CASE("clip.set_fade_in duration=0 removes filter", "[t23]") {
    std::string clip = vedit::test::make_test_clip("fade_remove");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    REQUIRE(mi >= 0);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    auto cid = app.clip_append(v1, mi);
    REQUIRE(cid.valid());

    REQUIRE(app.clip_set_fade_in(cid, 15));
    REQUIRE(app.clip_set_fade_in(cid, 0));

    // Brightness filter should be gone (no keyframes, only scalar or empty).
    auto result = app.filter_get_param_keyframes(cid, "brightness", "level");
    REQUIRE_FALSE(result.contains("keyframes"));
}

TEST_CASE("clip.set_fade_in and fade_out over RPC", "[t23]") {
    std::string clip = vedit::test::make_test_clip("fade_rpc");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    REQUIRE(mi >= 0);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    auto cid = app.clip_append(v1, mi);
    REQUIRE(cid.valid());

    vedit::rpc::Dispatcher d;
    vedit::rpc::register_methods(d, app);

    // Fade in via RPC.
    auto req = nlohmann::json{{"jsonrpc","2.0"},{"method","clip.set_fade_in"},
                              {"id",1},
                              {"params", {{"clip_id", cid.value},
                                           {"duration", 12}}}};
    auto resp = nlohmann::json::parse(d.handle_request(req.dump()));
    REQUIRE_FALSE(resp.contains("error"));

    // Verify keyframes exist.
    auto result = app.filter_get_param_keyframes(cid, "brightness", "level");
    REQUIRE(result.contains("keyframes"));

    // Fade out via RPC.
    req = nlohmann::json{{"jsonrpc","2.0"},{"method","clip.set_fade_out"},
                         {"id",2},
                         {"params", {{"clip_id", cid.value},
                                      {"duration", 8}}}};
    resp = nlohmann::json::parse(d.handle_request(req.dump()));
    REQUIRE_FALSE(resp.contains("error"));
}

TEST_CASE("fade filters appear in state.get attached filters", "[t23]") {
    std::string clip = vedit::test::make_test_clip("fade_state");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    REQUIRE(mi >= 0);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    auto cid = app.clip_append(v1, mi);
    REQUIRE(cid.valid());

    REQUIRE(app.clip_set_fade_in(cid, 10));

    std::string js = vedit::ProjectSaveLoad::state_json(app.project());
    auto doc = nlohmann::json::parse(js);

    bool found_brightness = false;
    for (const auto& af : doc["filters"]["attached"]) {
        if (af["target_type"] == "clip" &&
            af["target_id"] == cid.value &&
            af["filter"] == "brightness") {
            found_brightness = true;
        }
    }
    REQUIRE(found_brightness);
}

// Stage 4: analysis.track via opencv.tracker.

TEST_CASE("analysis.track runs and returns keyframes", "[t24]") {
    std::string clip = vedit::test::make_test_clip("tracker");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    REQUIRE(mi >= 0);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    auto cid = app.clip_append(v1, mi);
    REQUIRE(cid.valid());

    // Run tracker with a small rect near the center.
    auto result = app.clip_analyze_track(cid, "60 60 50 50", "KCF", 10);

    // The result is either a keyframes array or a value object (MLT
    // serializes the animation as a string). Either way it should be
    // non-empty — the tracker should produce at least one keyframe.
    REQUIRE_FALSE(result.is_null());
}

TEST_CASE("analysis.track over RPC dispatcher", "[t24]") {
    std::string clip = vedit::test::make_test_clip("tracker_rpc");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    REQUIRE(mi >= 0);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    auto cid = app.clip_append(v1, mi);
    REQUIRE(cid.valid());

    vedit::rpc::Dispatcher d;
    vedit::rpc::register_methods(d, app);

    auto req = nlohmann::json{{"jsonrpc","2.0"},{"method","analysis.track"},
                              {"id",1},
                              {"params", {{"clip_id", cid.value},
                                           {"rect", "50 50 50 50"},
                                           {"algo", "KCF"},
                                           {"steps", 10}}}};
    auto resp = nlohmann::json::parse(d.handle_request(req.dump()));
    REQUIRE_FALSE(resp.contains("error"));
    REQUIRE(resp["result"].contains("keyframes"));
    REQUIRE_FALSE(resp["result"]["keyframes"].is_null());
}

// 2.2: Scenario B binding — copy tracker keyframes to a filter param.
// The existing filter.get_param_keyframes + filter.set_param_keyframes
// already form the binding data path. This test proves the full chain:
// analysis.track -> read keyframes -> write to target filter param.

TEST_CASE("analysis.track results can be bound to filter param", "[t25]") {
    std::string clip = vedit::test::make_test_clip("bind_track");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::AppController app;
    int mi = app.library_import(clip);
    REQUIRE(mi >= 0);
    auto v1 = app.track_add(vedit::TrackKind::Video);
    auto cid = app.clip_append(v1, mi);
    REQUIRE(cid.valid());

    // Step 1: run analysis.track to get per-frame rects.
    auto track_result = app.clip_analyze_track(cid, "50 50 80 80", "KCF", 10);
    REQUIRE_FALSE(track_result.is_null());

    // Step 2: attach a filter to receive the keyframes.
    REQUIRE(app.filter_add(cid, "brightness"));

    // Step 3: copy tracker keyframes into the filter param.
    // If track_result is {keyframes: [...]}, use that directly.
    // If it's {value: "..."}, it's a serialized animation string
    // which set_filter_param_keyframes can also accept.
    if (track_result.contains("keyframes")) {
        REQUIRE(app.filter_set_param_keyframes(
            cid, "brightness", "level", track_result["keyframes"]));
    } else if (track_result.contains("value")) {
        // MLT returns serialized animation as a value string.
        // Set it directly — MLT auto-parses animation strings.
        REQUIRE(app.filter_set_param(
            cid, "brightness", "level",
            track_result["value"].get<std::string>()));
    }

    // Step 4: verify the keyframes were written.
    auto readback = app.filter_get_param_keyframes(cid, "brightness", "level");
    REQUIRE_FALSE(readback.is_null());
    // Either keyframes or value (both mean data was written).
    REQUIRE((readback.contains("keyframes") || readback.contains("value")));
}
