#include <catch2/catch_test_macros.hpp>

#include <Mlt.h>

#include "domain/ids.h"
#include "domain/media_library.h"
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
