// Phase 2 unit tests: pure filename classification.
// All tests are in-memory string operations; no filesystem is touched.

#include <doctest.h>

#include <string>

#include "../test_data.hpp"
#include "folderpilot/classifier.hpp"

using namespace folderpilot;
namespace data = folderpilot_test_data;

TEST_CASE("every documented extension classifies correctly") {
    for (const std::string_view ext : data::documents) {
        CHECK(classify_filename("file." + std::string{ext}) == Category::Documents);
    }
    for (const std::string_view ext : data::images) {
        CHECK(classify_filename("file." + std::string{ext}) == Category::Images);
    }
    for (const std::string_view ext : data::videos) {
        CHECK(classify_filename("file." + std::string{ext}) == Category::Videos);
    }
    for (const std::string_view ext : data::audio) {
        CHECK(classify_filename("file." + std::string{ext}) == Category::Audio);
    }
    for (const std::string_view ext : data::archives) {
        CHECK(classify_filename("file." + std::string{ext}) == Category::Archives);
    }
    for (const std::string_view ext : data::code) {
        CHECK(classify_filename("file." + std::string{ext}) == Category::Code);
    }
}

TEST_CASE("extension matching is ASCII case-insensitive") {
    CHECK(classify_filename("resume.PDF") == Category::Documents);
    CHECK(classify_filename("resume.pdf") == Category::Documents);
    CHECK(classify_filename("RESUME.PDF") == Category::Documents);
    CHECK(classify_filename("RePoRt.DoCx") == Category::Documents);
    CHECK(classify_filename("IMG_0001.JPG") == Category::Images);
    CHECK(classify_filename("clip.MKV") == Category::Videos);
    CHECK(classify_filename("song.FLAC") == Category::Audio);
    CHECK(classify_filename("main.CpP") == Category::Code);
    CHECK(classify_filename("BACKUP.TGZ") == Category::Archives);
}

TEST_CASE("compound archive extensions map to Archives") {
    CHECK(classify_filename("photos.tar.gz") == Category::Archives);
    CHECK(classify_filename("site.backup.tar.bz2") == Category::Archives);
    CHECK(classify_filename("release.tar.xz") == Category::Archives);
    CHECK(classify_filename("BACKUP.TAR.GZ") == Category::Archives);
    CHECK(classify_filename("data.TaR.Bz2") == Category::Archives);
}

TEST_CASE("compound check does not swallow ordinary multi-dot names") {
    CHECK(classify_filename("notes.tar.gz.txt") == Category::Documents);
    CHECK(classify_filename("dump.tar.gz2") == Category::Other); // "gz2" is unknown
}

TEST_CASE("no extension, dotfiles and empty input map to Other") {
    CHECK(classify_filename("README") == Category::Other);
    CHECK(classify_filename("Makefile") == Category::Other);
    CHECK(classify_filename(".gitignore") == Category::Other);
    CHECK(classify_filename(".pdf") == Category::Other); // hidden-style dotfile
    CHECK(classify_filename("") == Category::Other);
    CHECK(classify_filename(".") == Category::Other);
    CHECK(classify_filename("..") == Category::Other);
    CHECK(classify_filename("...") == Category::Other);
}

TEST_CASE("trailing dots have no usable extension") {
    CHECK(classify_filename("file.") == Category::Other);
    CHECK(classify_filename("file.pdf.") == Category::Other);
}

TEST_CASE("multiple dots use the last dot for the extension") {
    CHECK(classify_filename("report.final.v2.pdf") == Category::Documents);
    CHECK(classify_filename("my.album.cover.jpg") == Category::Images);
    CHECK(classify_filename("setup.build.py") == Category::Code);
}

TEST_CASE("unknown extensions map to Other") {
    CHECK(classify_filename("virus.exe") == Category::Other);
    CHECK(classify_filename("data.xyz") == Category::Other);
    CHECK(classify_filename("disk.dmg") == Category::Other);
}

TEST_CASE("unicode basenames with ASCII extensions classify correctly") {
    CHECK(classify_filename("résumé.pdf") == Category::Documents);
    CHECK(classify_filename("报告.txt") == Category::Documents);
    CHECK(classify_filename("фото.jpg") == Category::Images);
    CHECK(classify_filename("\xF0\x9F\x9A\x80party.mp4") == Category::Videos); // rocket emoji
    CHECK(classify_filename("café.mp3") == Category::Audio);
    CHECK(classify_filename("ФАЙЛ.PDF") == Category::Documents); // non-ASCII name, ASCII ext
}

TEST_CASE("separators are never mistaken for extensions (defensive basename use)") {
    CHECK(classify_filename("documents/file.pdf") == Category::Documents);
    CHECK(classify_filename("C:\\Users\\test\\img.png") == Category::Images);
    CHECK(classify_filename("folder\\notes.txt") == Category::Documents);
    CHECK(classify_filename("dir.sub/name") == Category::Other);
    CHECK(classify_filename("dir/") == Category::Other);
}

TEST_CASE("classification is deterministic") {
    const std::string name = "some.report.PDF";
    const Category first = classify_filename(name);
    for (int i = 0; i < 10; ++i) {
        CHECK(classify_filename(name) == first);
    }
}
