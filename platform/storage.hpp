// FolderPilot - platform storage primitives for the Phase 7.1c storage
// adapter (Windows).
//
// This module exposes the raw, policy-free Win32/NT surfaces the storage
// adapter composes into the ratified storage/locking protocol
// (docs/PHASE7-DESIGN.md section 3):
//
//   * the ONE path-based bootstrap open of the storage base directory
//     (NtCreateFile on the RtlDosPathNameToNtPathName_U-converted path,
//     both resolved at runtime from ntdll.dll - the same binding pattern as
//     platform/native.cpp, self-contained here so no verified Phase 3 code
//     is touched),
//   * parent-handle-anchored component (directory) open-or-create with
//     FILE_OPEN_REPARSE_POINT, for the FolderPilot / journals / <root-key>
//     chain,
//   * parent-handle-anchored leaf open implementing the ratified leaf-open
//     protocol primitives (access/share/disposition are supplied per leaf by
//     the adapter policy; the reparse-point and synchronous-I/O options are
//     mandatory here),
//   * known-folder resolution for the storage base
//     (SHGetKnownFolderPath(FOLDERID_LocalAppData)),
//   * ACL validation of an opened directory [G4] - AccessCheck for the
//     current user; validation only, never construction,
//   * FlushFileBuffers through an opened leaf handle.
//
// Policy (which components, which gates, which names, ordering) lives in the
// storage adapter (lib/folderpilot/storage_adapter.*), not here.

#pragma once

#include <string>
#include <string_view>

#include "native.hpp"
#include "unique_handle.hpp"

namespace folderpilot::platform {

// ---------------------------------------------------------------------------
// Known-folder bootstrap (%LOCALAPPDATA%).
// ---------------------------------------------------------------------------

// Resolves %LOCALAPPDATA% through SHGetKnownFolderPath(FOLDERID_LocalAppData)
// [W32-DOC, ratified]. Returns the absolute wide path converted to UTF-8.
struct KnownFolderResult {
    bool ok = false;
    std::string error;
    std::string path_utf8;   // absolute path, no trailing separator
};
KnownFolderResult local_app_data_path();

// ---------------------------------------------------------------------------
// Storage-tree opens. All fail closed: any failure closes every handle (RAII)
// and reports a classified error with the native code preserved.
// ---------------------------------------------------------------------------

// Path-based open of the storage BASE directory (the %LOCALAPPDATA%
// analogue). FILE_OPEN only - the base is never created by FolderPilot
// (it is a known OS folder). The reparse point itself is opened (flag set)
// and reported through `attributes`, so the adapter can refuse.
struct StorageBaseOpen {
    bool ok = false;
    NativeFailure failure = NativeFailure::Other;
    std::string error;
    unsigned long native_code = 0;
    unique_handle handle;              // valid when ok
    EntryAttributes attributes{};      // FileAttributeTagInfo payload
};
StorageBaseOpen open_storage_base(std::string_view utf8_path);

// Parent-anchored open-or-create of a DIRECTORY component of the storage
// chain (FolderPilot, journals, <root-key>). FILE_OPEN_IF semantics; the
// component is opened with FILE_OPEN_REPARSE_POINT (a reparse point is
// opened itself, never followed) and inspected through the handle. The
// hidden/system gate is applied by the adapter policy on the returned
// attributes. Access: READ_CONTROL | FILE_READ_ATTRIBUTES |
// FILE_LIST_DIRECTORY | FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY |
// SYNCHRONIZE (READ_CONTROL enables the ratified [G4] ACL validation
// through this handle; the rest anchors children and enumerates); share
// R|W|D.
struct StorageComponentOpen {
    bool ok = false;
    NativeFailure failure = NativeFailure::Other;
    std::string error;
    unsigned long native_code = 0;
    unique_handle handle;              // valid when ok
    EntryAttributes attributes{};      // FileAttributeTagInfo payload
    bool created = false;              // true when this call created it
};
StorageComponentOpen open_storage_component(HANDLE parent, std::string_view utf8_name);

// Parent-anchored LEAF open implementing the ratified leaf-open protocol
// primitives. `disposition` is one of FILE_OPEN / FILE_CREATE /
// FILE_OPEN_IF; `access` and `share` are supplied per the ratified table
// (e.g. lock: SYNCHRONIZE|FILE_READ_ATTRIBUTES with share 0; journal create:
// GENERIC_WRITE|SYNCHRONIZE with share R; read: FILE_GENERIC_READ with
// share R|W|D). The options always include FILE_OPEN_REPARSE_POINT (an
// existing reparse point is opened itself, never followed) and
// FILE_SYNCHRONOUS_IO_NONALERT. The opened handle is inspected (attributes),
// so the adapter can enforce the post-open validation (regular file,
// non-reparse).
struct StorageLeafOpen {
    bool ok = false;
    NativeFailure failure = NativeFailure::Other;
    std::string error;
    unsigned long native_code = 0;
    unique_handle handle;              // valid when ok
    EntryAttributes attributes{};      // FileAttributeTagInfo payload
    SourceIdentity identity{};         // captured through the handle (lock_id)
    bool created = false;              // true when the call created the leaf
};
StorageLeafOpen open_storage_leaf(HANDLE parent, std::string_view utf8_name,
                                  unsigned long access, unsigned long share,
                                  unsigned long disposition);

// FlushFileBuffers through an opened leaf handle. The leaf must have been
// opened with GENERIC_WRITE [W32-DOC]. Failures are reported with the Win32
// error preserved - a flush failure is a durability event the caller must
// classify, never a silent success.
NativeResult flush_storage_file(HANDLE handle);

// Sequential write at the opened handle's current position (synchronous
// handles advance it). Writes as many bytes as requested; a short write or
// failure is reported with the Win32 error preserved.
NativeResult write_storage_file(HANDLE handle, const unsigned char* bytes,
                                std::size_t byte_count);

// Current file size through the opened handle (GetFileSizeEx).
NativeResult storage_file_size(HANDLE handle, long long& out_bytes);

// ---------------------------------------------------------------------------
// ACL validation [G4] - validation only, never construction.
// ---------------------------------------------------------------------------

// AccessChecks the current user's token against the opened object's DACL:
// granted when the token holds every access bit of `desired_generic_access`
// (generic access mapped through the standard file mapping). A missing DACL
// grants everything and reports granted - the design validates operability,
// not hardening. Never modifies any ACL.
struct AclValidationResult {
    bool ok = false;                   // false: the validation could not run
    NativeFailure failure = NativeFailure::Other;
    std::string error;
    unsigned long native_code = 0;
    bool granted = false;              // meaningful when ok
};
AclValidationResult validate_current_user_access(HANDLE handle,
                                                 unsigned long generic_access);

} // namespace folderpilot::platform
