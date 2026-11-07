// FolderPilot - non-recursive, handle-anchored directory scanner.
//
// Enumerates ONLY the immediate children of the retained root handle
// (no std::filesystem iterators, no absolute paths, no recursion), applies
// the protection filters with structured skip reasons, opens each candidate
// relative to the root handle to capture its canonical identity, and closes
// every source handle before returning. Phase 3 is strictly read-only.

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "native.hpp"

namespace folderpilot {

// Structured skip reasons (never bare strings as identifiers).
enum class SkipReason {
    ReservedName,          // "." or ".." appeared in enumeration (defensive)
    Directory,             // an existing subdirectory - never entered
    ReparsePoint,          // junction, symlink or cloud placeholder (any tag)
    Hidden,                // FILE_ATTRIBUTE_HIDDEN
    System,                // FILE_ATTRIBUTE_SYSTEM
    NotRegularFile,        // no longer a regular file when opened
    Vanished,              // disappeared between enumeration and opening
    AccessDenied,          // the OS refused to open the entry
    InUse,                 // held by an incompatible existing handle
    OpenFailed,            // open failed for another reason (code in the entry)
    InvalidName,           // name is not a single safe component (defensive)
    EncodingError,         // name is not valid UTF-8 round-trippable
    Unsupported,           // identity capture unsupported on this filesystem
    // Planning-phase reasons (Phase 4):
    UnsafeCategory,        // destination category directory is unsafe for planning
    CategoryUninspectable, // destination category could not be inspected
    CollisionExhausted,    // no free destination name within the suffix limit
};

struct ScannedFile {
    std::string name_utf8;                 // original spelling, preserved
    platform::SourceIdentity identity;     // canonical identity from the opened handle
    unsigned long long size_bytes = 0;
    long long last_write_time = 0;         // Windows FILETIME (100ns units)
    unsigned long attributes = 0;
};

struct SkippedEntry {
    std::string name_utf8;                 // empty when unknown
    SkipReason reason{};
    std::string detail;                    // human-readable explanation
    unsigned long native_code = 0;         // NTSTATUS/Win32 code when relevant
};

struct RootInfo {
    std::string requested_path;            // as supplied by the user
    std::string resolved_path;             // final path through the handle
    std::string volume_guid_path;          // "\\?\Volume{...}\"
    platform::EntryAttributes attributes{};
    platform::SourceIdentity identity{};
};

enum class EnumerationStatus {
    Ok,      // enumeration completed (the directory may still be empty)
    Failed,  // enumeration itself failed - the scan is NOT a success
};

struct ScanResult {
    EnumerationStatus enumeration = EnumerationStatus::Failed;
    std::string error;                     // set when enumeration == Failed
    RootInfo root{};
    std::vector<ScannedFile> files;
    std::vector<SkippedEntry> skipped;
};

struct ScanOptions {
    // Size of the (reusable) enumeration buffer. A single directory entry
    // larger than this fails the scan instead of being silently skipped.
    std::size_t buffer_bytes = 64 * 1024;

    // Test seam: invoked immediately before each candidate source open.
    // Product code never sets it. Lets integration tests make an entry
    // disappear or change type between enumeration and opening.
    std::function<void(const std::string& utf8_name)> before_open;
};

// Scans the directory referenced by the retained, validated root handle.
// Deterministic for an unchanged filesystem: results are sorted with ordinal
// case-insensitive filename comparison and a stable byte-wise tie-break
// (case-equal names are never deduplicated).
ScanResult scan_directory(const platform::RootDirectory& root,
                          const ScanOptions& options = {});

} // namespace folderpilot
