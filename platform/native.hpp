// FolderPilot - platform native file handling (Windows).
//
// This module owns every Phase 3 Win32/NT surface:
//   * the ONE path-based root bootstrap open (NtCreateFile + NT path
//     conversion via RtlDosPathNameToNtPathName_U, both resolved at runtime
//     from ntdll.dll - no conflicting prototypes are declared anywhere),
//   * attribute / identity / final-path / volume-GUID inspection,
//   * handle-anchored directory enumeration (GetFileInformationByHandleEx
//     with FileIdBothDirectoryRestartInfo / FileIdBothDirectoryInfo),
//   * root-relative source opening for scanner candidates.
//
// NOTE: including this header transitively includes <Windows.h>. Consumers
// must define NOMINMAX (done project-wide via CMake). Everything declared
// here either mutates nothing or only opens handles with the approved
// sharing/access policy; Phase 3 is read-only with respect to file data.

#pragma once

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "unique_handle.hpp"

namespace folderpilot::platform {

struct RootOpenResult;

// ---------------------------------------------------------------------------
// Failure classification (Engineering Plan v2.2 taxonomy).
// ---------------------------------------------------------------------------
enum class NativeFailure {
    ConfirmedNotFound,   // the final component (or an ancestor) does not exist
    AccessDenied,        // the OS refused the operation
    InvalidPath,         // malformed/unsupported path form or encoding
    SharingConflict,     // incompatible existing handle (locked/in use)
    DeletePending,       // target is pending deletion
    NotADirectory,       // a directory was required, a file was found
    IsADirectory,        // a file was required, a directory was found
    Unsupported,         // required native capability unavailable on this system
    Other,               // anything else - always reported with a native code
};

struct NativeResult {
    bool ok = false;
    NativeFailure failure = NativeFailure::Other;
    std::string error;
    unsigned long native_code = 0; // NTSTATUS or Win32 error code, for reports
};

// ---------------------------------------------------------------------------
// Attributes, identity, volume identity.
// ---------------------------------------------------------------------------
struct EntryAttributes {
    bool valid = false;
    unsigned long attributes = 0;
    unsigned long reparse_tag = 0;

    bool directory()     const { return valid && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0; }
    bool reparse_point() const { return valid && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0; }
    bool hidden()        const { return valid && (attributes & FILE_ATTRIBUTE_HIDDEN) != 0; }
    bool system()        const { return valid && (attributes & FILE_ATTRIBUTE_SYSTEM) != 0; }
};

// Two strictly separate identity representations (Engineering Plan v2.2):
// never compared across variants - identity_matches() fails closed instead.
struct SourceIdentityEx {          // primary: GetFileInformationByHandleEx(FileIdInfo)
    unsigned long long volume_serial;
    FILE_ID_128 file_id;
};
struct SourceIdentityLegacy {      // fallback: GetFileInformationByHandle
    unsigned long volume_serial;
    unsigned long file_index_high;
    unsigned long file_index_low;
};
using SourceIdentity = std::variant<SourceIdentityEx, SourceIdentityLegacy>;

// True only when both identities use the SAME representation and all fields
// are equal. A representation mismatch returns false (fail closed).
bool identity_matches(const SourceIdentity& a, const SourceIdentity& b);

struct VolumeIdentity {
    std::string volume_guid_path;  // UTF-8, "\\?\Volume{GUID}\" form
};

// ---------------------------------------------------------------------------
// Root directory: the single path-based bootstrap open + retained RAII handle.
// ---------------------------------------------------------------------------
class RootDirectory {
public:
    RootDirectory() = default;
    ~RootDirectory() = default;

    RootDirectory(RootDirectory&&) noexcept = default;
    RootDirectory& operator=(RootDirectory&&) noexcept = default;
    RootDirectory(const RootDirectory&) = delete;
    RootDirectory& operator=(const RootDirectory&) = delete;

    bool valid() const noexcept { return handle_.valid(); }
    HANDLE get() const noexcept { return handle_.get(); }

    // Captured at open time and verified through the retained handle:
    std::string requested_path_utf8;   // exactly what the user supplied
    std::string resolved_path_utf8;    // GetFinalPathNameByHandleW result
    EntryAttributes attributes{};      // FileAttributeTagInfo payload
    SourceIdentity identity{};         // FileIdInfo (or legacy fallback)
    VolumeIdentity volume{};           // volume-GUID identity

private:
    friend RootOpenResult open_root(std::string_view utf8_path);
    unique_handle handle_;
};

// Result of the bootstrap open: either a validated, retained root handle with
// its verified payload, or a classified failure (RAII closes any handle).
struct RootOpenResult {
    bool ok = false;
    NativeFailure failure = NativeFailure::Other;
    std::string error;
    unsigned long native_code = 0;
    RootDirectory root;
};

// Opens the user-selected target directory (the ONE bootstrap open).
// Policy-free on purpose: it reports attributes/resolved path/identity/
// volume and lets PathGuard decide. Never creates anything (FILE_OPEN only).
// Failure closes everything (RAII) and fails closed.
RootOpenResult open_root(std::string_view utf8_path);

// ---------------------------------------------------------------------------
// Pure helpers (unit-testable without any filesystem access).
// ---------------------------------------------------------------------------

// Classifies a wide DOS-form path BEFORE any native conversion.
// Returns nullopt when the path form is acceptable (absolute local drive
// path, backslash separators); otherwise the failure class for:
// relative paths, drive-relative paths, root-relative paths, UNC/device
// namespaces, alternate-data-stream syntax, forward slashes, empty input,
// embedded nulls.
std::optional<NativeFailure> classify_dos_path(std::wstring_view dos_path);

// Ordinal, case-insensitive comparison of two UTF-8 strings, backed by
// CompareStringOrdinal (correct Windows semantics, not locale collation).
// Returns <0, 0, >0. Non-convertible input falls back to byte comparison
// (deterministic; such input cannot originate from a real NT path).
int compare_ordinal_ci(std::string_view utf8_a, std::string_view utf8_b);

// Current user's home directory (USERPROFILE) as UTF-8 without a trailing
// separator, or nullopt when it cannot be determined.
std::optional<std::string> current_user_home();

// ---------------------------------------------------------------------------
// Directory enumeration buffer parsing (pure - tested with synthetic buffers).
// ---------------------------------------------------------------------------
struct RawDirEntry {
    std::wstring_view name;          // view into the caller's buffer
    unsigned long attributes = 0;
    long long end_of_file = 0;       // file size in bytes
    long long last_write_time = 0;   // Windows FILETIME (100ns units)
    unsigned long long file_id = 0;  // SUPPLEMENTARY ONLY - never an identity
};

enum class DirParseStatus {
    Ok,
    TruncatedHeader,    // entry header does not fit in the buffer
    MisalignedEntry,    // NextEntryOffset is not an 8-byte multiple
    BadNameLength,      // FileNameLength is not an even number of bytes
    NameOutOfBounds,    // filename bytes extend past the valid buffer region
    EntryOutOfBounds,   // NextEntryOffset points past the valid region
};

struct DirParseResult {
    DirParseStatus status = DirParseStatus::Ok;
    std::string error;
    std::vector<RawDirEntry> entries;
};

// Parses one buffer as returned by GetFileInformationByHandleEx with
// FileIdBothDirectoryInfo / FileIdBothDirectoryRestartInfo. Validates every
// boundary before access; entries are never assumed null-terminated.
DirParseResult parse_id_both_dir_entries(const unsigned char* data, std::size_t bytes);

// Full handle-anchored enumeration of the immediate children of `root`:
// FileIdBothDirectoryRestartInfo first, then FileIdBothDirectoryInfo
// batches, parsed strictly, each entry passed to `visitor` in order.
// A single entry larger than the buffer fails the enumeration (fail closed)
// rather than being silently skipped.
NativeResult enumerate_immediate_children(
    HANDLE root,
    std::size_t buffer_bytes,
    const std::function<void(const RawDirEntry&)>& visitor);

// ---------------------------------------------------------------------------
// Root-relative source opening + inspection (used by the scanner).
// Opens `utf8_name` as a SINGLE component relative to `root_handle` with
// DELETE|FILE_READ_ATTRIBUTES|SYNCHRONIZE and FILE_SHARE_READ|FILE_SHARE_WRITE
// (no FILE_SHARE_DELETE - approved source protection policy), inspects the
// actual opened handle and CLOSES it before returning. The handle queries are
// the source of truth, not the enumeration snapshot.
// ---------------------------------------------------------------------------
struct SourceInspection {
    bool ok = false;
    NativeFailure failure = NativeFailure::Other;
    std::string error;
    unsigned long native_code = 0;
    EntryAttributes attributes{};
    SourceIdentity identity{};
    long long size_bytes = 0;
    long long last_write_time = 0;
};

SourceInspection inspect_source(HANDLE root_handle, std::string_view utf8_name);

} // namespace folderpilot::platform
