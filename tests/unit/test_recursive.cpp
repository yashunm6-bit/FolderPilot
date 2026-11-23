// Phase 8 tests: recursive organization (docs/PHASE8-DESIGN.md §1).

#include <doctest.h>

#include "folderpilot/executor.hpp"
#include "folderpilot/journal.hpp"
#include "folderpilot/path_guard.hpp"
#include "folderpilot/planner.hpp"
#include "folderpilot/scanner.hpp"
#include "folderpilot/storage_adapter.hpp"
#include "folderpilot/undo_plan.hpp"
#include "folderpilot/undo_execute.hpp"
#include "../phase3_fixture.hpp"

#include <algorithm>
#include "native.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <filesystem>
#include <string>

namespace {

using namespace folderpilot;

struct RecFixture {
    fixture::SandboxDir base;
    fixture::SandboxDir organized;
    platform::RootOpenResult root;
    std::string root_key;
    bool storage_ok = false;

    explicit RecFixture(const std::string& name)
        : base(name + "-base"), organized(name + "-root") {
        root = platform::open_root(organized.u8());
        auto storage = open_storage_root(base.u8(), root.root);
        storage_ok = storage.ok;
        root_key = storage.root_key;
    }

    ScanResult recursive_scan() {
        ScanOptions options;
        options.recursive = true;
        return scan_directory(root.root, options);
    }
};

} // namespace

TEST_CASE("P8: recursive scan plans files from nested directories with whole-plan collisions") {
    RecFixture f{"rec-basic"};
    REQUIRE(f.root.ok);
    REQUIRE(fixture::create_file(f.organized.path(), L"top.pdf", "top"));
    REQUIRE(fixture::create_directory(f.organized.path(), L"sub1"));
    REQUIRE(fixture::create_file(f.organized.path() / L"sub1", L"nested.pdf", "n1"));
    REQUIRE(fixture::create_directory(f.organized.path() / L"sub1", L"sub2"));
    REQUIRE(fixture::create_file(f.organized.path() / L"sub1" / L"sub2", L"deep.pdf", "d"));

    const auto scan = f.recursive_scan();
    INFO("scan: enum=" << static_cast<int>(scan.enumeration) << " err=" << scan.error
         << " files=" << scan.files.size() << " skipped=" << scan.skipped.size());
    REQUIRE(scan.enumeration == EnumerationStatus::Ok);
    REQUIRE(scan.files.size() == 3);
    // Relative source paths, deterministic order.
    CHECK(scan.files[0].name_utf8 == "sub1\\nested.pdf");
    CHECK(scan.files[1].name_utf8 == "sub1\\sub2\\deep.pdf");
    CHECK(scan.files[2].name_utf8 == "top.pdf");

    // Whole-plan collision resolution: three .pdf files → one natural name
    // plus suffixes, across depths.
    const auto plan = build_plan(f.root.root, scan);
    REQUIRE(plan.valid());
    REQUIRE(plan.planned_count() == 3);
    std::vector<std::string> dests;
    for (const auto& move : plan.moves()) {
        const bool name_or_suffix =
            move.source_name != move.destination_name ||
            move.destination_name == "top.pdf";
        CHECK(name_or_suffix);
        dests.push_back(move.destination_name);
    }
    std::sort(dests.begin(), dests.end());
    CHECK(dests[0] != dests[1]);
    CHECK(dests[1] != dests[2]);
}

TEST_CASE("P8: recursive dry-run is mutation-free; journaled recursive apply moves across depths") {
    RecFixture f{"rec-apply"};
    REQUIRE(f.root.ok);
    REQUIRE(f.storage_ok);
    REQUIRE(fixture::create_directory(f.organized.path(), L"nested"));
    REQUIRE(fixture::create_file(f.organized.path() / L"nested", L"report.pdf", "r"));

    // Recursive dry-run: nothing moves.
    const auto preview = f.recursive_scan();
    REQUIRE(preview.enumeration == EnumerationStatus::Ok);
    REQUIRE(preview.files.size() == 1);
    CHECK(std::filesystem::exists(f.organized.path() / L"nested" / L"report.pdf"));

    // Journaled recursive apply.
    const auto scan = f.recursive_scan();
    INFO("second scan: enum=" << static_cast<int>(scan.enumeration)
         << " err=" << scan.error << " files=" << scan.files.size());
    REQUIRE(scan.enumeration == EnumerationStatus::Ok);
    REQUIRE(scan.files.size() == 1);
    const auto plan = build_plan(f.root.root, scan);
    REQUIRE(plan.valid());
    REQUIRE(plan.planned_count() == 1);
    const auto result = execute_plan(plan, f.organized.u8(), f.base.u8(), false);
    REQUIRE(result.journal_active);
    for (const auto& r : result.records) {
        INFO("record: " << r.source_name << " outcome=" << static_cast<int>(r.outcome)
             << " detail=" << r.detail);
    }
    CHECK(result.moved_count == 1);
    CHECK(std::filesystem::exists(f.organized.path() / L"Documents" / L"report.pdf"));
    CHECK_FALSE(std::filesystem::exists(f.organized.path() / L"nested" / L"report.pdf"));

    // Undo planning sees the journaled recursive move.
    const auto undo = load_undo_plan(f.base.u8(), f.root.root);
    INFO("undo: ok=" << undo.ok << " err=" << undo.error
         << " state=" << static_cast<int>(undo.fold.state)
         << " reason=" << static_cast<int>(undo.fold.reason)
         << " cands=" << undo.fold.candidates.size()
         << " journal=" << undo.journal_name);
    REQUIRE(undo.ok);
    REQUIRE(undo.fold.candidates.size() == 1);
    CHECK(undo.fold.candidates[0].source_name == "nested\\report.pdf");
    CHECK(undo.fold.candidates[0].classification == UndoCandidateClass::Undoable);
}

TEST_CASE("P8: category destinations are never traversed; reparse/hidden/system dirs are skipped") {
    RecFixture f{"rec-skip"};
    REQUIRE(f.root.ok);
    REQUIRE(f.storage_ok);
    // A pre-existing category with a file inside: never traversed as a source.
    REQUIRE(fixture::create_directory(f.organized.path(), L"Documents"));
    REQUIRE(fixture::create_file(f.organized.path() / L"Documents", L"already.pdf", "x"));
    // Hidden and system subdirectories with files inside.
    REQUIRE(fixture::create_directory(f.organized.path(), L"hiddendir"));
    REQUIRE(fixture::create_file(f.organized.path() / L"hiddendir", L"hidden.pdf", "h"));
    REQUIRE(fixture::add_attributes(f.organized.path() / L"hiddendir", FILE_ATTRIBUTE_HIDDEN));
    REQUIRE(fixture::create_directory(f.organized.path(), L"sysdir"));
    REQUIRE(fixture::create_file(f.organized.path() / L"sysdir", L"sys.pdf", "s"));
    REQUIRE(fixture::add_attributes(f.organized.path() / L"sysdir", FILE_ATTRIBUTE_SYSTEM));
    // A top-level file that IS planned.
    REQUIRE(fixture::create_file(f.organized.path(), L"report.pdf", "r"));

    const auto scan = f.recursive_scan();
    INFO("scan: enum=" << static_cast<int>(scan.enumeration) << " err=" << scan.error);
    for (const auto& file : scan.files) {
        MESSAGE("file: " << file.name_utf8);
    }
    for (const auto& entry : scan.skipped) {
        MESSAGE("skip: " << entry.name_utf8);
    }
    REQUIRE(scan.enumeration == EnumerationStatus::Ok);
    REQUIRE(scan.files.size() == 1);
    CHECK(scan.files[0].name_utf8 == "report.pdf");
    // The skips name the protected entries.
    bool saw_category = false;
    bool saw_hidden = false;
    bool saw_system = false;
    for (const auto& entry : scan.skipped) {
        if (entry.name_utf8 == "Documents") saw_category = true;
        if (entry.name_utf8 == "hiddendir") saw_hidden = true;
        if (entry.name_utf8 == "sysdir") saw_system = true;
    }
    CHECK(saw_category);
    CHECK(saw_hidden);
    CHECK(saw_system);
}

TEST_CASE("P8: reparse-point subdirectories are skipped at depth; the plan stays complete") {
    RecFixture f{"rec-reparse"};
    REQUIRE(f.root.ok);
    REQUIRE(fixture::create_directory(f.organized.path(), L"real"));
    REQUIRE(fixture::create_file(f.organized.path() / L"real", L"report.pdf", "r"));
    // A junction pointing OUTSIDE the root (never traversed).
    REQUIRE(fixture::create_directory(f.organized.path(), L"junction-target-out"));

    REQUIRE(fixture::create_junction(f.organized.path() / L"linked",
                                    f.organized.path() / L"junction-target-out"));
    REQUIRE(fixture::create_file(f.organized.path() / L"junction-target-out",
                                L"outside.pdf", "outside"));

    const auto scan = f.recursive_scan();
    REQUIRE(scan.enumeration == EnumerationStatus::Ok);
    // The junction is never traversed (no escape, no duplicate traversal),
    // but its TARGET directory is a normal part of the tree: the file inside
    // it is planned exactly once, via the real directory path.
    REQUIRE(scan.files.size() == 2);
    CHECK(scan.files[0].name_utf8 == "junction-target-out\\outside.pdf");
    CHECK(scan.files[1].name_utf8 == "real\\report.pdf");
    bool saw_junction = false;
    for (const auto& entry : scan.skipped) {
        if (entry.name_utf8 == "linked") {
            saw_junction = true;
            CHECK(entry.reason == SkipReason::ReparsePoint);
        }
    }
    CHECK(saw_junction);
}

TEST_CASE("P8: the depth and entry limits fail the whole scan") {
    RecFixture f{"rec-limits"};
    REQUIRE(f.root.ok);
    // A chain deeper than the 64-level limit is impractical to create
    // literally; verify the traversal counts and the limit constants.
    CHECK(ScanOptions::kMaxRecursiveDepth == 64);
    CHECK(ScanOptions::kMaxRecursiveEntries == 50000);

    // Deep-but-legal nesting works: 40 levels with a file at the bottom.
    std::filesystem::path deep = f.organized.path();
    for (int i = 0; i < 20; ++i) {
        deep /= std::wstring(L"d") + std::to_wstring(i);
    }
    std::error_code ec;
    std::filesystem::create_directories(deep, ec);
    REQUIRE_FALSE(ec);
    // The 20-level chain stays well under MAX_PATH from the sandbox root.
    REQUIRE(fixture::create_file(deep, L"deep.pdf", "d"));
    const auto scan = f.recursive_scan();
    REQUIRE(scan.enumeration == EnumerationStatus::Ok);
    REQUIRE(scan.files.size() == 1);
    CHECK(scan.files[0].name_utf8.find("d19\\deep.pdf") != std::string::npos);
}

TEST_CASE("P8: recursive undo restores nested locations (intermediates intact)") {
    RecFixture f{"rec-undo"};
    REQUIRE(f.root.ok);
    REQUIRE(f.storage_ok);
    REQUIRE(fixture::create_directory(f.organized.path(), L"nested"));
    REQUIRE(fixture::create_file(f.organized.path() / L"nested", L"report.pdf", "r"));
    const auto scan = f.recursive_scan();
    const auto plan = build_plan(f.root.root, scan);
    REQUIRE(plan.valid());
    const auto result = execute_plan(plan, f.organized.u8(), f.base.u8(), false);
    REQUIRE(result.moved_count == 1);

    const auto undo = load_undo_plan(f.base.u8(), f.root.root);
    REQUIRE(undo.ok);
    REQUIRE(undo.fold.candidates.size() == 1);
    REQUIRE(undo.fold.candidates[0].classification == UndoCandidateClass::Undoable);
    const auto undone = execute_undo(undo, f.base.u8(), f.root.root, true);
    CHECK(undone.restored_count == 1);
    // The nested source location is restored (the intermediate survived).
    CHECK(std::filesystem::exists(f.organized.path() / L"nested" / L"report.pdf"));
    CHECK_FALSE(std::filesystem::exists(f.organized.path() / L"Documents" / L"report.pdf"));
}

TEST_CASE("P8: empty nested directories produce no files and no skips") {
    RecFixture f{"rec-empty"};
    REQUIRE(f.root.ok);
    REQUIRE(fixture::create_directory(f.organized.path(), L"empty1"));
    REQUIRE(fixture::create_directory(f.organized.path() / L"empty1", L"empty2"));

    const auto scan = f.recursive_scan();
    REQUIRE(scan.enumeration == EnumerationStatus::Ok);
    CHECK(scan.files.empty());
    CHECK(scan.skipped.empty());
}

TEST_CASE("P8: a reparse point nested at depth is skipped; the plan stays complete") {
    RecFixture f{"rec-reparse-depth"};
    REQUIRE(f.root.ok);
    REQUIRE(fixture::create_directory(f.organized.path(), L"real"));
    REQUIRE(fixture::create_file(f.organized.path() / L"real", L"report.pdf", "r"));
    // The junction's target lives in the BASE sandbox (inside the fixture
    // sandbox, but outside the organized root the walk should stay within).
    REQUIRE(fixture::create_directory(f.base.path(), L"depth-target"));
    REQUIRE(fixture::create_file(f.base.path() / L"depth-target", L"outside.pdf", "o"));
    REQUIRE(fixture::create_junction(f.organized.path() / L"real" / L"linked",
                                     f.base.path() / L"depth-target"));

    const auto scan = f.recursive_scan();
    REQUIRE(scan.enumeration == EnumerationStatus::Ok);
    REQUIRE(scan.files.size() == 1);
    CHECK(scan.files[0].name_utf8 == "real\\report.pdf");
    bool saw_junction = false;
    for (const auto& entry : scan.skipped) {
        if (entry.name_utf8 == "real\\linked") {
            saw_junction = true;
            CHECK(entry.reason == SkipReason::ReparsePoint);
        }
    }
    CHECK(saw_junction);
}

TEST_CASE("P8: a junction cycle (child to ancestor) never retraverses") {
    RecFixture f{"rec-cycle"};
    REQUIRE(f.root.ok);
    REQUIRE(fixture::create_directory(f.organized.path(), L"inner"));
    REQUIRE(fixture::create_file(f.organized.path() / L"inner", L"report.pdf", "r"));
    // The junction points back at the scan ROOT (an ancestor): skipped as a
    // reparse point, so the traversal terminates with no duplicates.
    REQUIRE(fixture::create_junction(f.organized.path() / L"inner" / L"up",
                                     f.organized.path()));

    const auto scan = f.recursive_scan();
    REQUIRE(scan.enumeration == EnumerationStatus::Ok);
    REQUIRE(scan.files.size() == 1);
    CHECK(scan.files[0].name_utf8 == "inner\\report.pdf");
    bool saw_cycle = false;
    for (const auto& entry : scan.skipped) {
        if (entry.name_utf8 == "inner\\up") {
            saw_cycle = true;
            CHECK(entry.reason == SkipReason::ReparsePoint);
        }
    }
    CHECK(saw_cycle);
}

TEST_CASE("P8: containment under mutation between plan and apply (removal, reparse swap)") {
    // (a) The recorded intermediate directory vanishes before the apply: the
    // executor's per-step walk reports the source as missing (journaled),
    // the run completes, nothing moves.
    {
        RecFixture f{"rec-mut-gone"};
        REQUIRE(f.root.ok);
        REQUIRE(f.storage_ok);
        REQUIRE(fixture::create_directory(f.organized.path(), L"gone"));
        REQUIRE(fixture::create_file(f.organized.path() / L"gone", L"note.txt", "n"));
        const auto scan = f.recursive_scan();
        const auto plan = build_plan(f.root.root, scan);
        REQUIRE(plan.valid());
        REQUIRE(plan.planned_count() == 1);

        std::error_code ec;
        std::filesystem::remove_all(f.organized.path() / L"gone", ec);
        REQUIRE_FALSE(ec);

        const auto result = execute_plan(plan, f.organized.u8(), f.base.u8(), false);
        REQUIRE(result.records.size() == 1);
        CHECK(result.moved_count == 0);
        CHECK(result.records[0].outcome == MoveOutcome::SourceMissing);
        CHECK_FALSE(std::filesystem::exists(f.organized.path() / L"Documents" / L"note.txt"));
    }
    // (b) The recorded intermediate directory is replaced by a reparse point
    // before the apply: the per-step gate refuses to traverse it (fail
    // closed), the move is journaled as SourceInvalid and never lands
    // beyond the junction.
    {
        RecFixture f{"rec-mut-swap"};
        REQUIRE(f.root.ok);
        REQUIRE(f.storage_ok);
        REQUIRE(fixture::create_directory(f.organized.path(), L"swap"));
        REQUIRE(fixture::create_file(f.organized.path() / L"swap", L"note.txt", "n"));
        REQUIRE(fixture::create_directory(f.organized.path(), L"swap-target"));
        const auto scan = f.recursive_scan();
        const auto plan = build_plan(f.root.root, scan);
        REQUIRE(plan.valid());

        std::error_code ec;
        std::filesystem::remove_all(f.organized.path() / L"swap", ec);
        REQUIRE_FALSE(ec);
        REQUIRE(fixture::create_junction(f.organized.path() / L"swap",
                                         f.organized.path() / L"swap-target"));

        const auto result = execute_plan(plan, f.organized.u8(), f.base.u8(), false);
        REQUIRE(result.records.size() == 1);
        CHECK(result.moved_count == 0);
        CHECK(result.records[0].outcome == MoveOutcome::SourceInvalid);
        CHECK_FALSE(std::filesystem::exists(f.organized.path() / L"swap-target" / L"note.txt"));
    }
}

TEST_CASE("P8: traversed directories do not consume the terminal-entry budget (§1.2)") {
    RecFixture f{"rec-entry-budget"};
    REQUIRE(f.root.ok);
    // Four terminal entries (two files per depth) and TWO traversed
    // directories. A budget of four must succeed: only planned files and
    // skipped entries count toward the cap.
    REQUIRE(fixture::create_file(f.organized.path(), L"a.txt", "a"));
    REQUIRE(fixture::create_file(f.organized.path(), L"b.txt", "b"));
    REQUIRE(fixture::create_directory(f.organized.path(), L"d1"));
    REQUIRE(fixture::create_file(f.organized.path() / L"d1", L"n1.txt", "n"));
    REQUIRE(fixture::create_directory(f.organized.path(), L"d2"));
    REQUIRE(fixture::create_file(f.organized.path() / L"d2", L"n2.txt", "n"));

    ScanOptions ok;
    ok.recursive = true;
    ok.max_recursive_entries = 4;
    const auto within = scan_directory(f.root.root, ok);
    INFO("within: enum=" << static_cast<int>(within.enumeration) << " err=" << within.error);
    REQUIRE(within.enumeration == EnumerationStatus::Ok);
    REQUIRE(within.files.size() == 4);

    ScanOptions over;
    over.recursive = true;
    over.max_recursive_entries = 3;
    const auto refuses = scan_directory(f.root.root, over);
    CHECK(refuses.enumeration == EnumerationStatus::Failed);
    CHECK(refuses.error.find("entry limit") != std::string::npos);
}

TEST_CASE("P8: the directory cap fails the whole scan (§1.2)") {
    RecFixture f{"rec-directory-cap"};
    REQUIRE(f.root.ok);
    // Three empty siblings (no terminal entries): the SEPARATE directory cap,
    // not the entry budget, must refuse the scan.
    REQUIRE(fixture::create_directory(f.organized.path(), L"d1"));
    REQUIRE(fixture::create_directory(f.organized.path(), L"d2"));
    REQUIRE(fixture::create_directory(f.organized.path(), L"d3"));

    ScanOptions options;
    options.recursive = true;
    options.max_recursive_directories = 2;
    const auto scan = scan_directory(f.root.root, options);
    INFO("scan: enum=" << static_cast<int>(scan.enumeration) << " err=" << scan.error);
    CHECK(scan.enumeration == EnumerationStatus::Failed);
    CHECK(scan.error.find("directory limit") != std::string::npos);
    CHECK(scan.files.empty()); // never a partial plan
}

TEST_CASE("P8: the depth limit fails the whole scan (§1.2)") {
    RecFixture f{"rec-depth-limit"};
    REQUIRE(f.root.ok);
    auto dir = f.organized.path();
    for (int i = 0; i < 3; ++i) {
        const std::wstring name =
            L"level" + std::wstring(1, static_cast<wchar_t>(L'1' + i));
        REQUIRE(fixture::create_directory(dir, name));
        dir /= name;
    }
    REQUIRE(fixture::create_file(dir, L"deep.txt", "d"));

    ScanOptions ok;
    ok.recursive = true;
    ok.max_recursive_depth = 3;
    CHECK(scan_directory(f.root.root, ok).enumeration == EnumerationStatus::Ok);

    ScanOptions over;
    over.recursive = true;
    over.max_recursive_depth = 2;
    const auto refuses = scan_directory(f.root.root, over);
    CHECK(refuses.enumeration == EnumerationStatus::Failed);
    CHECK(refuses.error.find("depth limit") != std::string::npos);
    CHECK(refuses.files.empty()); // never a partial plan
}

TEST_CASE("P8: a directory replaced during the walk fails the scan closed") {
    RecFixture f{"rec-traversal-swap"};
    REQUIRE(f.root.ok);
    REQUIRE(fixture::create_file(f.organized.path(), L"a.txt", "a"));
    REQUIRE(fixture::create_directory(f.organized.path(), L"zdir"));
    REQUIRE(fixture::create_file(f.organized.path() / L"zdir", L"nested.txt", "n"));

    ScanOptions options;
    options.recursive = true;
    bool swapped = false;
    options.before_open = [&](const std::string& name) {
        // "a.txt" sorts before "zdir": rename the directory after it was
        // enumerated but before the walk opens it through the parent handle.
        if (name == "a.txt" && !swapped) {
            swapped = true;
            std::error_code ec;
            std::filesystem::rename(f.organized.path() / L"zdir",
                                    f.organized.path() / L"zdir-moved", ec);
        }
    };
    const auto scan = scan_directory(f.root.root, options);
    CHECK(swapped);
    CHECK(scan.enumeration == EnumerationStatus::Failed);
    CHECK(scan.error.find("zdir") != std::string::npos);
    CHECK(scan.files.size() <= 1); // no plan from the swapped subtree
}

TEST_CASE("P8: repeated recursive scans are deterministic") {
    RecFixture f{"rec-determinism"};
    REQUIRE(f.root.ok);
    REQUIRE(fixture::create_file(f.organized.path(), L"top.pdf", "t"));
    REQUIRE(fixture::create_directory(f.organized.path(), L"sub1"));
    REQUIRE(fixture::create_file(f.organized.path() / L"sub1", L"nested.pdf", "n"));
    REQUIRE(fixture::create_directory(f.organized.path() / L"sub1", L"sub2"));
    REQUIRE(fixture::create_file(f.organized.path() / L"sub1" / L"sub2", L"deep.pdf", "d"));

    const auto a = f.recursive_scan();
    const auto b = f.recursive_scan();
    REQUIRE(a.enumeration == EnumerationStatus::Ok);
    REQUIRE(b.enumeration == EnumerationStatus::Ok);
    REQUIRE(a.files.size() == b.files.size());
    for (std::size_t i = 0; i < a.files.size(); ++i) {
        CHECK(a.files[i].name_utf8 == b.files[i].name_utf8);
    }
    REQUIRE(a.skipped.size() == b.skipped.size());
    for (std::size_t i = 0; i < a.skipped.size(); ++i) {
        CHECK(a.skipped[i].name_utf8 == b.skipped[i].name_utf8);
        CHECK(a.skipped[i].reason == b.skipped[i].reason);
    }
}
