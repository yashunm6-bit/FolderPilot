// FolderPilot - PathGuard: safely establish and validate a user-selected
// target directory.
//
// Layering: PathGuard (core) implements the POLICY (attribute gates, the
// component-aware denylist, disclosure requirements). The platform layer
// performs the single path-based bootstrap open and all native queries.
// The denylist is defense in depth, not a complete security boundary.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "native.hpp"

namespace folderpilot {

// Structured, non-ambiguous rejection reasons (no raw strings as identifiers).
enum class PathRejection {
    MissingTarget,        // confirmed absent - never created
    InvalidPath,          // unsupported/malformed path form or encoding
    NotADirectory,        // target exists but is a regular file
    ReparseLeafDirectory, // the target itself is a reparse point (any tag)
    HiddenDirectory,      // target carries FILE_ATTRIBUTE_HIDDEN
    SystemDirectory,      // target carries FILE_ATTRIBUTE_SYSTEM
    DriveRoot,            // target is a drive root (C:\)
    SystemSubtree,        // target is under a denied system subtree
    UsersDepthPolicy,     // target is C:\Users itself or a direct child (profile root)
    HomeRoot,             // target is the current user's home directory itself
    AccessDenied,         // the OS refused access to the target
    SharingConflict,      // target is held by an incompatible existing handle
    PathResolutionFailed, // final path could not be resolved through the handle
    UnsupportedPlatform,  // required native capability unavailable
    OtherError,           // anything else, reported with a native code
};

struct PathGuardResult {
    bool ok = false;

    // Valid when !ok:
    PathRejection rejection = PathRejection::OtherError;
    std::string error;
    unsigned long native_code = 0;

    // Valid when ok - the retained root handle plus its verified payload:
    std::string requested_path;               // exactly what the user supplied
    std::string resolved_path;                // final path through the handle
    platform::EntryAttributes attributes{};   // attribute gate result
    platform::SourceIdentity identity{};      // directory identity
    platform::VolumeIdentity volume{};        // volume-GUID identity
    platform::RootDirectory root;             // move-only, retained while scanning
};

// Validates the target and opens its root handle. Never creates anything;
// a missing target is an error. On success the caller owns result.root and
// must keep it alive while scanning.
PathGuardResult validate_target(std::string_view utf8_path);

// ---- pure, unit-testable helpers (no filesystem access) --------------------

// Splits a UTF-8 path into components on '\' (the byte 0x5C can never occur
// inside a multi-byte UTF-8 sequence). Empty components are dropped.
std::vector<std::string> split_path_components(std::string_view path);

// Component-aware denylist evaluation over the RESOLVED path.
//   * exact drive roots are rejected,
//   * denied system subtrees: Windows, Program Files, Program Files (x86),
//     ProgramData, Boot, Recovery, System Volume Information, $Recycle.Bin,
//   * "Users" node + direct children (profile roots) are rejected; deeper
//     directories are allowed,
//   * the current user's home directory itself is rejected (when known).
// All matching is ordinal and case-insensitive (CompareStringOrdinal);
// there is no string-prefix matching anywhere. Returns nullopt when the
// target passes.
std::optional<std::pair<PathRejection, std::string>> evaluate_denylist(
    const std::vector<std::string>& components,
    const std::string& resolved_path,
    const std::optional<std::string>& home_path);

} // namespace folderpilot
