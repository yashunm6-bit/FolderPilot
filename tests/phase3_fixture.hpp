// FolderPilot - Phase 3 test fixture helpers.
//
// All filesystem integration fixtures live under the workspace-relative
// sandbox build/phase3-test-sandbox/ (never %TEMP%, never user folders).
// Fixture mutation helpers use wide Win32 calls so Unicode names are exact.
// TEST CODE ONLY - none of this is product code.

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <vector>
#include <string>

namespace fixture {

// RAII test directory: build/<sandbox>/<name>-<counter>.
// The destructor removes ONLY this directory, after verifying (via its
// canonical path) that it really is inside the sandbox.
enum class SandboxRoot {
    Phase3, // build/phase3-test-sandbox/  (read-only phase fixtures)
    Phase5, // build/phase5-test-sandbox/  (movement fixtures)
};

class SandboxDir {
public:
    explicit SandboxDir(const std::string& name, SandboxRoot root = SandboxRoot::Phase3);
    ~SandboxDir();
    SandboxDir(const SandboxDir&) = delete;
    SandboxDir& operator=(const SandboxDir&) = delete;

    const std::filesystem::path& path() const { return path_; }
    // UTF-8, backslash-separated absolute path (suitable for PathGuard input).
    std::string u8() const;

private:
    SandboxDir(const std::string& name, const std::filesystem::path& base);
    std::filesystem::path sandbox_root_;
    std::filesystem::path path_;
};

// Creates dir/name with the given bytes. Wide-name safe. Returns success.
bool create_file(const std::filesystem::path& dir, const std::wstring& name,
                 const std::string& bytes);

bool create_directory(const std::filesystem::path& dir, const std::wstring& name);

// Adds the given attributes to an existing entry (e.g. FILE_ATTRIBUTE_HIDDEN).
bool add_attributes(const std::filesystem::path& target, unsigned long attributes);

// Creates a directory junction link -> target. Both must be inside the
// sandbox. Returns false when the capability is unavailable (tests skip).
bool create_junction(const std::filesystem::path& link, const std::filesystem::path& target);

// Read-only snapshot of a fixture tree (used for the no-mutation guarantee).
// Reparse points are recorded but never followed.
struct SnapshotEntry {
    std::uintmax_t size = 0;
    long long mtime = 0;
    bool reparse = false;
    bool operator==(const SnapshotEntry&) const = default;
};
using Snapshot = std::map<std::string, SnapshotEntry>; // relative UTF-8 path -> entry

Snapshot take_snapshot(const std::filesystem::path& dir);
bool snapshots_equal(const Snapshot& a, const Snapshot& b);

// Returns true when the file exists and its bytes equal  exactly.
bool file_content_matches(const std::filesystem::path& p, const std::string& expected);

// One-time capability probe (cached): does the approved RootDirectory-
// relative handle rename work on this OS build? On build 26300 it returns
// ERROR_INVALID_PARAMETER for every RootDirectory-relative form while
// absolute paths succeed - the approved mechanism is BLOCKED there.
bool root_relative_rename_supported();
extern const char kMovementSkipReason[];

// Spawns the built folderpilot.exe with the given arguments (each quoted
// individually) and captures its exit code and stdout. TEST CODE ONLY.
struct CliRun {
    int exit_code = -1;
    std::string output;
};
CliRun run_folderpilot_cli(const std::vector<std::wstring>& arguments);

} // namespace fixture
