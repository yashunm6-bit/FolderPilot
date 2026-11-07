// Phase 3A tests: platform foundation - RAII handle semantics, structured
// error mapping, and handle cleanup on every failure path.

#include <doctest.h>

#include <string>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "../phase3_fixture.hpp"
#include "folderpilot/path_guard.hpp"
#include "unique_handle.hpp"

using namespace folderpilot;

TEST_CASE("unique_handle: default and null handles are invalid") {
    const platform::unique_handle empty;
    CHECK_FALSE(empty.valid());
    CHECK(empty.get() == nullptr);

    const platform::unique_handle from_null{nullptr};
    CHECK_FALSE(from_null.valid());
}

TEST_CASE("unique_handle: move construction transfers ownership") {
    fixture::SandboxDir dir{"h-move"};
    REQUIRE(fixture::create_file(dir.path(), L"h.txt", "x"));
    const std::wstring path = dir.path() / L"h.txt";

    HANDLE raw = ::CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(raw != INVALID_HANDLE_VALUE);

    platform::unique_handle first{raw};
    CHECK(first.valid());
    CHECK(first.get() == raw);

    platform::unique_handle second{std::move(first)};
    CHECK(second.valid());
    CHECK(second.get() == raw);
    CHECK_FALSE(first.valid()); // moved-from is inert
}

TEST_CASE("unique_handle: move assignment replaces the owned handle") {
    fixture::SandboxDir dir{"h-assign"};
    REQUIRE(fixture::create_file(dir.path(), L"a.txt", "x"));
    const std::wstring path = dir.path() / L"a.txt";

    platform::unique_handle target{::CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr,
                                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    platform::unique_handle source{::CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr,
                                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    REQUIRE(target.valid());
    REQUIRE(source.valid());
    CHECK(target.get() != source.get());

    target = std::move(source);
    CHECK(target.valid());
    CHECK_FALSE(source.valid()); // source emptied by the move
}

TEST_CASE("unique_handle: a destroyed handle is really closed") {
    fixture::SandboxDir dir{"h-close"};
    REQUIRE(fixture::create_file(dir.path(), L"close.txt", "x"));
    const std::wstring path = dir.path() / L"close.txt";
    {
        // Opened with NO sharing: while held, nothing else can open the file.
        platform::unique_handle held{::CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr,
                                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        REQUIRE(held.valid());
        HANDLE blocked = ::CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(blocked == INVALID_HANDLE_VALUE); // exclusive while held
    }
    // The wrapper is destroyed; the exclusive re-open must now succeed,
    // proving the RAII wrapper really closed the handle.
    HANDLE again = ::CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(again != INVALID_HANDLE_VALUE);
    ::CloseHandle(again);
}

TEST_CASE("error mapping follows the v2.2 taxonomy") {
    using platform::classify_ntstatus;
    using platform::NativeFailure;
    CHECK(classify_ntstatus(0xC0000034) == NativeFailure::ConfirmedNotFound); // OBJECT_NAME_NOT_FOUND
    CHECK(classify_ntstatus(0xC000003A) == NativeFailure::ConfirmedNotFound); // OBJECT_PATH_NOT_FOUND
    CHECK(classify_ntstatus(0xC0000022) == NativeFailure::AccessDenied);      // ACCESS_DENIED
    CHECK(classify_ntstatus(0xC0000043) == NativeFailure::SharingConflict);   // SHARING_VIOLATION
    CHECK(classify_ntstatus(0xC0000054) == NativeFailure::SharingConflict);   // LOCK_VIOLATION
    CHECK(classify_ntstatus(0xC0000056) == NativeFailure::DeletePending);     // DELETE_PENDING
    CHECK(classify_ntstatus(0xC0000103) == NativeFailure::NotADirectory);     // NOT_A_DIRECTORY
    CHECK(classify_ntstatus(0xC00000BA) == NativeFailure::IsADirectory);      // FILE_IS_A_DIRECTORY
    CHECK(classify_ntstatus(0xC0000033) == NativeFailure::InvalidPath);       // OBJECT_NAME_INVALID
    CHECK(classify_ntstatus(0xC000000D) == NativeFailure::InvalidPath);       // INVALID_PARAMETER
    CHECK(classify_ntstatus(0xC0000035) == NativeFailure::Conflict);          // NAME_COLLISION (Phase 5 taxonomy)
    CHECK(classify_ntstatus(0xC00000D4) == NativeFailure::CrossVolume);       // NOT_SAME_DEVICE
    CHECK(classify_ntstatus(0xE0001234) == NativeFailure::Other);             // unknown error severity
}

TEST_CASE("failed opens leave no retained handle behind") {
    fixture::SandboxDir dir{"h-cleanup"};
    REQUIRE(fixture::create_file(dir.path(), L"plain.txt", "x"));

    // Missing target: the bootstrap fails and retains nothing.
    auto open_failed = platform::open_root(dir.u8() + "\\does-not-exist");
    CHECK_FALSE(open_failed.ok);
    CHECK_FALSE(open_failed.root.valid());

    // File-as-target: same guarantee through PathGuard.
    auto guard_failed = validate_target(dir.u8() + "\\plain.txt");
    CHECK_FALSE(guard_failed.ok);
    CHECK_FALSE(guard_failed.root.valid());

    // A successful open DOES retain the handle for the caller.
    auto guard_ok = validate_target(dir.u8());
    REQUIRE(guard_ok.ok);
    CHECK(guard_ok.root.valid());
}
