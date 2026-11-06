// Phase 3 tests: PathGuard - pure denylist matching plus sandboxed
// integration of the root bootstrap. No real system directory is ever
// touched: denylist tests run on synthetic paths only.

#include <doctest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "../phase3_fixture.hpp"
#include "folderpilot/path_guard.hpp"
#include "text.hpp"

using namespace folderpilot;

// ---------------------------------------------------------------------------
// Pure denylist tests (synthetic paths; no filesystem access).
// ---------------------------------------------------------------------------

TEST_CASE("component splitting is component-aware") {
    CHECK(split_path_components("C:\\") == std::vector<std::string>{"C:"});
    CHECK(split_path_components("C:\\a\\b\\c") ==
          std::vector<std::string>{"C:", "a", "b", "c"});
    CHECK(split_path_components("C:\\a\\\\b\\") ==
          std::vector<std::string>{"C:", "a", "b"});
}

TEST_CASE("drive roots are rejected") {
    const auto r = evaluate_denylist(split_path_components("C:\\"), "C:\\", std::nullopt);
    REQUIRE(r.has_value());
    CHECK(r->first == PathRejection::DriveRoot);
}

TEST_CASE("system subtrees are rejected at any depth") {
    const char* subtrees[] = {
        "Windows", "Program Files", "Program Files (x86)", "ProgramData",
        "Boot", "Recovery", "System Volume Information", "$Recycle.Bin",
    };
    for (const char* subtree : subtrees) {
        const std::string path = std::string{"C:\\"} + subtree;
        const auto direct = evaluate_denylist(split_path_components(path), path, std::nullopt);
        REQUIRE(direct.has_value());
        CHECK(direct->first == PathRejection::SystemSubtree);

        const std::string nested = path + "\\deep\\folder";
        const auto nested_result =
            evaluate_denylist(split_path_components(nested), nested, std::nullopt);
        REQUIRE(nested_result.has_value());
        CHECK(nested_result->first == PathRejection::SystemSubtree);
    }
}

TEST_CASE("denylist matching is case-insensitive") {
    const auto r = evaluate_denylist(split_path_components("c:\\wInDoWs\\x"),
                                     "c:\\wInDoWs\\x", std::nullopt);
    REQUIRE(r.has_value());
    CHECK(r->first == PathRejection::SystemSubtree);
}

TEST_CASE("matching is component-aware, never lexical-prefix") {
    // "WindowsX" and "Windows Defender" are NOT the "Windows" component.
    for (const char* raw : {"C:\\WindowsX\\stuff", "C:\\Windows Defender\\db",
                            "C:\\Program FilesX\\app", "C:\\UserX\\docs"}) {
        const std::string path{raw};
        const auto r = evaluate_denylist(split_path_components(path), path, std::nullopt);
        CHECK_FALSE(r.has_value());
    }
}

TEST_CASE("Users depth policy: node and profile roots rejected, deeper allowed") {
    const auto users_itself = evaluate_denylist(split_path_components("C:\\Users"),
                                                "C:\\Users", std::nullopt);
    REQUIRE(users_itself.has_value());
    CHECK(users_itself->first == PathRejection::UsersDepthPolicy);

    const auto profile_root = evaluate_denylist(split_path_components("C:\\Users\\alice"),
                                                "C:\\Users\\alice", std::nullopt);
    REQUIRE(profile_root.has_value());
    CHECK(profile_root->first == PathRejection::UsersDepthPolicy);

    const auto deeper = evaluate_denylist(split_path_components("C:\\Users\\alice\\projects\\fp"),
                                          "C:\\Users\\alice\\projects\\fp", std::nullopt);
    CHECK_FALSE(deeper.has_value());
}

TEST_CASE("the current user's home root itself is rejected") {
    // A home on another drive isolates the HomeRoot rule from the Users
    // depth policy (a real home under C:\Users is already rejected by the
    // profile-root rule, which wins first - both are safe rejections).
    const auto r = evaluate_denylist(split_path_components("D:\\home-alice"),
                                     "D:\\home-alice", std::string{"D:\\home-alice"});
    REQUIRE(r.has_value());
    CHECK(r->first == PathRejection::HomeRoot);

    const auto deeper = evaluate_denylist(split_path_components("D:\\home-alice\\projects"),
                                          "D:\\home-alice\\projects",
                                          std::string{"D:\\home-alice"});
    CHECK_FALSE(deeper.has_value());

    // Defense in depth: a Users-child home is already caught by the depth
    // policy even when it is also the configured home.
    const auto users_child = evaluate_denylist(split_path_components("C:\\Users\\alice"),
                                               "C:\\Users\\alice",
                                               std::string{"C:\\Users\\alice"});
    REQUIRE(users_child.has_value());
    CHECK(users_child->first == PathRejection::UsersDepthPolicy);
}

// ---------------------------------------------------------------------------
// Integration tests (sandboxed; the ONLY filesystem-touching section).
// ---------------------------------------------------------------------------

TEST_CASE("PathGuard accepts a valid existing directory and captures its identity") {
    fixture::SandboxDir dir{"pg-valid"};
    fixture::create_file(dir.path(), L"marker.txt", "content");

    const auto result = validate_target(dir.u8());
    REQUIRE(result.ok);
    CHECK(result.requested_path == dir.u8());
    CHECK(result.attributes.valid);
    CHECK(result.attributes.directory());
    CHECK_FALSE(result.attributes.reparse_point());
    // Resolved path ends with the fixture name.
    CHECK(result.resolved_path.find("pg-valid") != std::string::npos);
    // Volume GUID has the documented form.
    CHECK(result.volume.volume_guid_path.starts_with("\\\\?\\Volume{"));
    CHECK(result.volume.volume_guid_path.ends_with("}\\"));
    // Identity is captured and stable across a second open.
    const auto second = validate_target(dir.u8());
    REQUIRE(second.ok);
    CHECK(platform::identity_matches(result.identity, second.identity));
}

TEST_CASE("PathGuard rejects a missing target without creating it") {
    fixture::SandboxDir dir{"pg-missing"};
    const std::string target = dir.u8() + "\\does-not-exist";

    const auto result = validate_target(target);
    CHECK_FALSE(result.ok);
    CHECK(result.rejection == PathRejection::MissingTarget);
    CHECK_FALSE(std::filesystem::exists(std::filesystem::path{dir.u8()} / "does-not-exist"));
}

TEST_CASE("PathGuard rejects a regular file supplied as target") {
    fixture::SandboxDir dir{"pg-file"};
    REQUIRE(fixture::create_file(dir.path(), L"plain.txt", "x"));

    const auto result = validate_target(dir.u8() + "\\plain.txt");
    CHECK_FALSE(result.ok);
    CHECK(result.rejection == PathRejection::NotADirectory);
}

TEST_CASE("PathGuard rejects hidden and system target directories") {
    {
        fixture::SandboxDir dir{"pg-hidden"};
        REQUIRE(fixture::add_attributes(dir.path(), FILE_ATTRIBUTE_HIDDEN));
        const auto result = validate_target(dir.u8());
        CHECK_FALSE(result.ok);
        CHECK(result.rejection == PathRejection::HiddenDirectory);
    }
    {
        fixture::SandboxDir dir{"pg-system"};
        REQUIRE(fixture::add_attributes(dir.path(), FILE_ATTRIBUTE_SYSTEM));
        const auto result = validate_target(dir.u8());
        CHECK_FALSE(result.ok);
        CHECK(result.rejection == PathRejection::SystemDirectory);
    }
}

TEST_CASE("PathGuard rejects a target that is a reparse point") {
    fixture::SandboxDir dir{"pg-reparse"};
    fixture::SandboxDir real{"pg-reparse-real"};
    const bool junction_ok = fixture::create_junction(dir.path() / L"junction",
                                                      real.path());
    if (!junction_ok) {
        MESSAGE("SKIPPED (capability unavailable): junction creation on this machine");
        return;
    }
    const auto result = validate_target(dir.u8() + "\\junction");
    CHECK_FALSE(result.ok);
    CHECK(result.rejection == PathRejection::ReparseLeafDirectory);
}

TEST_CASE("PathGuard handles Unicode target directories") {
    fixture::SandboxDir dir{"pg-unicode"};
    const std::wstring sub = L"\x00FCn\x00EFcode \x6587\x4EF6\x5939"; // ünïcode 文件夹
    REQUIRE(fixture::create_directory(dir.path(), sub));
    const auto sub_u8 = platform::to_utf8(sub);
    REQUIRE(sub_u8.ok);

    const auto result = validate_target(dir.u8() + "\\" + sub_u8.value);
    REQUIRE(result.ok);
    CHECK(result.resolved_path.find(sub_u8.value) != std::string::npos);
}

TEST_CASE("root handle moves correctly and the moved-from value is inert") {
    fixture::SandboxDir dir{"pg-move"};
    auto first = validate_target(dir.u8());
    REQUIRE(first.ok);
    CHECK(first.root.valid());

    platform::RootDirectory moved = std::move(first.root);
    CHECK(moved.valid());
    CHECK_FALSE(first.root.valid());

    platform::RootDirectory moved_again = std::move(moved);
    CHECK(moved_again.valid());
    CHECK_FALSE(moved.valid());
}
