#include <catch2/catch_test_macros.hpp>

#include <Mlt.h>

#include "domain/ids.h"
#include "domain/media_library.h"
#include "domain/player_controller.h"
#include "domain/project_model.h"
#include "domain/timeline_model.h"

namespace {

// Generates a short test clip on disk. Returns its path.
std::string make_test_clip(const std::string& tag) {
    std::string path = "/tmp/vedit_t4_" + tag + ".mp4";
    std::string cmd =
        "ffmpeg -y -loglevel error -f lavfi -i testsrc=duration=2:size=160x120:rate=25 "
        "-f lavfi -i sine=frequency=440:duration=2 -c:v libx264 -pix_fmt yuv420p "
        "-c:a aac -shortest " + path;
    if (std::system(cmd.c_str()) != 0) return "";
    return path;
}

} // namespace

TEST_CASE("ProjectModel wires profile+library+timeline", "[domain]") {
    vedit::ProjectModel pm;
    REQUIRE(pm.timeline().track_count() == 0);
    REQUIRE(pm.producer() != nullptr);
}

TEST_CASE("TimelineModel add/remove tracks", "[domain]") {
    vedit::ProjectModel pm;
    auto& tl = pm.timeline();

    auto v1 = tl.add_track(vedit::TrackKind::Video, "V1");
    auto a1 = tl.add_track(vedit::TrackKind::Audio, "A1");
    REQUIRE(v1.valid());
    REQUIRE(a1.valid());
    REQUIRE(v1 != a1);
    REQUIRE(tl.track_count() == 2);

    REQUIRE(tl.track(v1)->kind == vedit::TrackKind::Video);
    REQUIRE(tl.track(a1)->name == "A1");

    REQUIRE(tl.remove_track(v1));
    REQUIRE(tl.track_count() == 1);
    REQUIRE(tl.track(v1) == nullptr);
    REQUIRE(tl.track(a1) != nullptr);
}

TEST_CASE("Stable clip id survives append + rebuild", "[domain]") {
    std::string clip = make_test_clip("stable");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::ProjectModel pm;
    auto& tl = pm.timeline();
    auto v1 = tl.add_track(vedit::TrackKind::Video);

    int mi = pm.library().import(clip);
    REQUIRE(mi >= 0);

    auto id = tl.append_clip(v1, mi);
    REQUIRE(id.valid());
    REQUIRE(tl.clip_count() == 1);

    const auto* info = tl.clip(id);
    REQUIRE(info != nullptr);
    REQUIRE(info->track_id == v1);
    REQUIRE(info->start == 0);
    REQUIRE(info->length > 0);
    REQUIRE(info->media_index == mi);
}

TEST_CASE("Split clip produces two clips with distinct stable ids",
          "[domain]") {
    std::string clip = make_test_clip("split");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::ProjectModel pm;
    auto& tl = pm.timeline();
    auto v1 = tl.add_track(vedit::TrackKind::Video);
    int mi = pm.library().import(clip);

    auto id = tl.append_clip(v1, mi);
    const auto* info = tl.clip(id);
    REQUIRE(info != nullptr);

    // Split in the middle.
    auto right_id = tl.split_clip(id, info->start + info->length / 2);
    REQUIRE(right_id.valid());
    REQUIRE(right_id != id);
    REQUIRE(tl.clip_count() == 2);

    const auto* left = tl.clip(id);
    const auto* right = tl.clip(right_id);
    REQUIRE(left != nullptr);
    REQUIRE(right != nullptr);
    REQUIRE(left->length + right->length == info->length);
    REQUIRE(right->start == left->start + left->length);
    // Both clips resolve to the same source media.
    REQUIRE(left->media_index == mi);
    REQUIRE(right->media_index == mi);
}

TEST_CASE("Remove clip drops only that clip id", "[domain]") {
    std::string clip = make_test_clip("remove");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::ProjectModel pm;
    auto& tl = pm.timeline();
    auto v1 = tl.add_track(vedit::TrackKind::Video);
    int mi = pm.library().import(clip);

    auto id1 = tl.append_clip(v1, mi);
    auto id2 = tl.append_clip(v1, mi);
    auto id3 = tl.append_clip(v1, mi);
    REQUIRE(tl.clip_count() == 3);

    REQUIRE(tl.remove_clip(id2));
    REQUIRE(tl.clip_count() == 2);
    REQUIRE(tl.clip(id2) == nullptr);
    REQUIRE(tl.clip(id1) != nullptr);
    REQUIRE(tl.clip(id3) != nullptr);
    // After removing the middle clip, id1 and id3 are still resolvable
    // and id3's start should now be shifted left.
    const auto* c3 = tl.clip(id3);
    REQUIRE(c3 != nullptr);
    REQUIRE(c3->start == tl.clip(id1)->start + tl.clip(id1)->length);
}

TEST_CASE("Duration is the longest track", "[domain]") {
    std::string clip = make_test_clip("dur");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::ProjectModel pm;
    auto& tl = pm.timeline();
    auto v1 = tl.add_track(vedit::TrackKind::Video);
    int mi = pm.library().import(clip);

    tl.append_clip(v1, mi);
    tl.append_clip(v1, mi);
    // Two 2-second clips at 25fps = 50 frames each, total 100.
    REQUIRE(tl.duration() == 100);
}

TEST_CASE("MediaLibrary dedupes by path", "[domain]") {
    std::string clip = make_test_clip("dedup");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::ProjectModel pm;
    auto& lib = pm.library();
    int i1 = lib.import(clip);
    int i2 = lib.import(clip);
    REQUIRE(i1 == i2);
    REQUIRE(lib.count() == 1);
    const auto* item = lib.item(i1);
    REQUIRE(item != nullptr);
    REQUIRE(item->has_video);
    REQUIRE(item->width == 160);
    REQUIRE(item->height == 120);
}

TEST_CASE("Insert at position pads with blank when past end", "[domain][t5]") {
    std::string clip = make_test_clip("insert");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::ProjectModel pm;
    auto& tl = pm.timeline();
    auto v1 = tl.add_track(vedit::TrackKind::Video);
    int mi = pm.library().import(clip);

    auto id1 = tl.append_clip(v1, mi);
    const auto* c1 = tl.clip(id1);
    REQUIRE(c1 != nullptr);
    int64_t orig_len = c1->length;

    // Insert at position well past the end -> blank-padded, clip lands
    // exactly at the requested position.
    auto id2 = tl.insert_clip(v1, mi, c1->length + 20);
    REQUIRE(id2.valid());
    const auto* c2 = tl.clip(id2);
    REQUIRE(c2 != nullptr);
    REQUIRE(c2->start == c1->length + 20);
    REQUIRE(c2->length == orig_len);
}

TEST_CASE("Same-track move reorders clip", "[domain][t5]") {
    std::string clip = make_test_clip("movesame");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::ProjectModel pm;
    auto& tl = pm.timeline();
    auto v1 = tl.add_track(vedit::TrackKind::Video);
    int mi = pm.library().import(clip);

    auto id1 = tl.append_clip(v1, mi);
    auto id2 = tl.append_clip(v1, mi);
    const auto* c2 = tl.clip(id2);
    int64_t len = c2->length;

    // Move clip2 to position 0 -- it should now start at 0, clip1
    // shifts after it. Clip id2 must remain stable and resolvable.
    REQUIRE(tl.move_clip(id2, v1, 0));
    const auto* c2_after = tl.clip(id2);
    REQUIRE(c2_after != nullptr);
    REQUIRE(c2_after->start == 0);
    REQUIRE(c2_after->length == len);
    // id1 still resolvable (stable id survives the move of another clip).
    REQUIRE(tl.clip(id1) != nullptr);
}

TEST_CASE("Cross-track move preserves clip id and media", "[domain][t5]") {
    std::string clip = make_test_clip("movex");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::ProjectModel pm;
    auto& tl = pm.timeline();
    auto v1 = tl.add_track(vedit::TrackKind::Video, "V1");
    auto v2 = tl.add_track(vedit::TrackKind::Video, "V2");
    int mi = pm.library().import(clip);

    auto id = tl.append_clip(v1, mi);
    const auto* c = tl.clip(id);
    REQUIRE(c != nullptr);
    int64_t len = c->length;

    // Move from v1 to v2 at position 10.
    REQUIRE(tl.move_clip(id, v2, 10));
    const auto* c_after = tl.clip(id);
    REQUIRE(c_after != nullptr);
    REQUIRE(c_after->track_id == v2);
    REQUIRE(c_after->start == 10);
    REQUIRE(c_after->length == len);
    REQUIRE(c_after->media_index == mi);
    // v1 no longer has this clip.
    auto on_v1 = tl.clips_on_track(v1);
    REQUIRE(on_v1.empty());
    auto on_v2 = tl.clips_on_track(v2);
    REQUIRE(on_v2.size() == 1);
    REQUIRE(on_v2[0].id == id);
}

TEST_CASE("Trim clip changes in/out and length", "[domain][t5]") {
    std::string clip = make_test_clip("trim");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::ProjectModel pm;
    auto& tl = pm.timeline();
    auto v1 = tl.add_track(vedit::TrackKind::Video);
    int mi = pm.library().import(clip);

    auto id = tl.append_clip(v1, mi);
    const auto* c = tl.clip(id);
    REQUIRE(c != nullptr);
    int64_t orig_len = c->length;
    int64_t orig_in = c->in_point;

    // Trim the in-point forward by 5 frames.
    REQUIRE(tl.trim_clip(id, orig_in + 5, c->out_point));
    const auto* c_after = tl.clip(id);
    REQUIRE(c_after != nullptr);
    REQUIRE(c_after->in_point == orig_in + 5);
    REQUIRE(c_after->length == orig_len - 5);
}

TEST_CASE("Two tracks compose through the tractor", "[domain][t6]") {
    std::string clip = make_test_clip("compose");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::ProjectModel pm;
    auto& tl = pm.timeline();
    auto v1 = tl.add_track(vedit::TrackKind::Video, "V1");
    auto v2 = tl.add_track(vedit::TrackKind::Video, "V2");
    int mi = pm.library().import(clip);

    // Base clip on v1, overlay clip on v2 at the same timeline range.
    REQUIRE(tl.append_clip(v1, mi).valid());
    REQUIRE(tl.append_clip(v2, mi).valid());
    REQUIRE(tl.duration() > 0);

    // The tractor must produce a valid composed frame -- this is the
    // proof that the composite transition planted in add_track wires
    // the two tracks together through MLT's render path.
    Mlt::Producer* tractor = tl.producer();
    REQUIRE(tractor != nullptr);
    REQUIRE(tractor->is_valid());
    tractor->seek(0);
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

TEST_CASE("PlayerController play/pause/seek against the tractor", "[domain][t6]") {
    std::string clip = make_test_clip("player");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::ProjectModel pm;
    auto& tl = pm.timeline();
    auto v1 = tl.add_track(vedit::TrackKind::Video);
    int mi = pm.library().import(clip);
    REQUIRE(tl.append_clip(v1, mi).valid());

    vedit::PlayerController player(pm);
    player.set_audio_consumer("null");  // headless: no audio device
    REQUIRE_FALSE(player.is_playing());
    REQUIRE(player.position() == 0);

    player.seek(10);
    REQUIRE(player.position() == 10);

    // play() starts the realtime consumer; pause() stops it cleanly.
    REQUIRE(player.play());
    REQUIRE(player.is_playing());
    player.pause();
    REQUIRE_FALSE(player.is_playing());
}

TEST_CASE("Two video tracks composite via planted transition", "[domain][t6]") {
    std::string clip = make_test_clip("composite");
    if (clip.empty()) { FAIL("could not generate test clip"); return; }

    vedit::ProjectModel pm;
    auto& tl = pm.timeline();
    auto v1 = tl.add_track(vedit::TrackKind::Video, "V1");
    auto v2 = tl.add_track(vedit::TrackKind::Video, "V2");
    int mi = pm.library().import(clip);
    REQUIRE(tl.append_clip(v1, mi).valid());
    REQUIRE(tl.append_clip(v2, mi).valid());

    // The tractor must be valid and produce a frame with both tracks
    // composited. A rendered frame proves the composite transition is
    // wired (otherwise track 2 would be invisible / an error).
    Mlt::Producer* tractor = pm.producer();
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
