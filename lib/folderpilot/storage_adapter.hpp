// FolderPilot - Phase 7.1c storage adapter (docs/PHASE7-DESIGN.md section 3).
//
// The ratified storage/locking policy, composed over the platform storage
// primitives (platform/storage.hpp):
//
//   * handle-anchored bootstrap: one path-based open of the storage base
//     (the %LOCALAPPDATA% analogue; the known-folder resolution is supplied
//     by platform::local_app_data_path), then FolderPilot -> journals ->
//     <root-key> opened/created relative to the verified parent handles,
//     each component reparse-inspected through its opened handle; hidden and
//     system attributes on the FolderPilot-created components refuse
//     (fail-closed); the legitimately hidden base ancestor is accepted,
//   * root-key derivation: SHA-256 over the volume-GUID path and the root
//     directory's file identity - the filesystem identity governs, the path
//     is informational; the key is re-derived every session and names the
//     per-root storage directory,
//   * ACL validation of the journals directory [G4] - AccessCheck for the
//     current user; absent and NULL DACLs refuse outright and an empty DACL
//     is denied by AccessCheck (ratified Decision A); operability validation
//     only (Decision D), never construction,
//   * the leaf-open protocol: exclusive mutation lock (first acquisition
//     FILE_OPEN_IF, later FILE_OPEN, share 0,
//     SYNCHRONIZE|FILE_READ_ATTRIBUTES|FILE_READ_DATA - the data bit is
//     required by the sharing arithmetic, which exempts attributes-only
//     access - probe triple held / never-created / free),
//     journal/segment/anchor creation with FILE_CREATE
//     (GENERIC_WRITE|FILE_READ_ATTRIBUTES|SYNCHRONIZE share R - the
//     attributes bit serves the mandated post-open validation),
//     refuse-on-exists, reads with FILE_GENERIC_READ share R|W|D; every
//     leaf opened with FILE_OPEN_REPARSE_POINT and post-open validated as a
//     regular non-reparse file,
//   * lock_id: the lock file's identity digest, recorded in RUN-START and
//     re-verified cross-session; the lock is never deleted and never opened
//     with DELETE access,
//   * append-only persistence: writes at the sequential position followed by
//     FlushFileBuffers; journals, segments, anchors and the lock are never
//     deleted or rewritten by FolderPilot; no truncate-to-repair, no
//     automatic recovery,
//   * capacity admission: the exact per-root aggregate byte recompute against
//     the 128 MiB quota (to be called under the mutation lock), and the
//     ratified 16 MiB per-file limit enforced on appends.
//
// No function here deletes, truncates or rewrites an existing journal,
// segment, anchor or lock file. Every ambiguity resolves to a structured
// refusal with the native code preserved.

#pragma once

#include <string>
#include <string_view>

#include "folderpilot/journal.hpp"
#include "native.hpp"
#include "storage.hpp"
#include "unique_handle.hpp"

namespace folderpilot {

// ---------------------------------------------------------------------------
// Failure taxonomy (storage adapter).
// ---------------------------------------------------------------------------

enum class StorageFailure {
    Ok,                    // the operation succeeded
    ReparseLeaf,           // an opened entry carried a reparse point
    NotRegularFile,        // a leaf was required, a directory/special was found
    HiddenOrSystem,        // a FolderPilot-created component carried hidden/system
    IdentityMismatch,      // a recorded identity does not match the verified one
    NameNotFound,          // a required leaf does not exist
    NameCollision,         // FILE_CREATE refused: the leaf already exists
    SharingViolation,      // an exclusive hold rejected the operation
    AccessDenied,          // the OS refused the operation
    AclValidationFailed,   // [G4]: the current user lacks the required access
    QuotaExceeded,         // the 128 MiB aggregate or 16 MiB per-file limit
    InvalidName,           // a name was not a single safe path component
    Unsupported,           // the required native capability is unavailable
    IoError,               // anything else - always reported with a native code
};

struct StorageResult {
    bool ok = false;
    StorageFailure failure = StorageFailure::IoError;
    std::string error;
    unsigned long native_code = 0;    // NTSTATUS or Win32 code, preserved
};

// ---------------------------------------------------------------------------
// Root key derivation (pure; unit-testable without any filesystem access).
// ---------------------------------------------------------------------------

// Canonical identity material for a source identity: a representation-tagged
// byte string (Ex: 'E' + 8-byte volume serial LE + 16-byte file id; legacy:
// 'L' + 4-byte serial + 4-byte index-high + 4-byte index-low LE). Returns
// false for a mixed/unknown representation (fail closed).
bool storage_identity_material(const platform::SourceIdentity& identity,
                               std::string& out_bytes);

// The lock_id: lowercase hex SHA-256 of the canonical identity material
// (64 characters, within the ratified 73-byte lock_id bound); the derivation
// convention is ratified as Decision E (2026-11-17).
bool storage_lock_id(const platform::SourceIdentity& identity, std::string& out_hex);

// The leaf identity digest (the journal_id / anchor_id source per §20.4):
// the same G15 convention v1 applied to any storage leaf's captured identity.
bool storage_leaf_id(const platform::SourceIdentity& identity, std::string& out_hex);

// The root key: lowercase hex SHA-256 over UTF-8(volume GUID path) || 0x00 ||
// the canonical identity material. 64 characters. `volume_guid_path` must be
// non-empty (the volume GUID is ratified as part of the root identity).
bool derive_root_key(const std::string& volume_guid_path,
                     const platform::SourceIdentity& identity, std::string& out_hex);

// ---------------------------------------------------------------------------
// Bootstrap: the handle-anchored storage chain.
// ---------------------------------------------------------------------------

// Opens (or creates) the per-root storage chain
//   <base>/FolderPilot/journals/<root-key>
// anchored to verified parent handles at every component. `base_path_utf8`
// is the storage base (production: platform::local_app_data_path(); tests:
// a sandbox directory). `organized_root` must be a validated platform root
// (platform::open_root) - its volume GUID and file identity derive the
// root key. Gates applied per component: reparse ⇒ refuse; hidden/system on
// the FolderPilot-created components ⇒ refuse; the base ancestor is exempt.
// The journals directory is ACL-validated [G4] for the current user's
// read/write access.
struct StorageRoot {
    bool ok = false;
    StorageFailure failure = StorageFailure::IoError;
    std::string error;
    unsigned long native_code = 0;

    std::string root_key;             // derived key == the <root-key> name
    platform::unique_handle journals_dir;       // journals directory handle (ACL-validated)
    platform::unique_handle root_key_dir;       // <root-key> directory handle (validated)
    std::string base_path_utf8;       // the storage base path (informational)
};
StorageRoot open_storage_root(std::string_view base_path_utf8,
                              const platform::RootDirectory& organized_root);

// ---------------------------------------------------------------------------
// Mutation lock.
// ---------------------------------------------------------------------------

enum class LockState { NeverCreated, Held, Free };

struct LockProbeResult {
    bool ok = false;
    StorageFailure failure = StorageFailure::IoError;
    std::string error;
    unsigned long native_code = 0;
    LockState state = LockState::NeverCreated;
};

// The ratified probe: an attributes-inspection open with full sharing
// (FILE_READ_DATA in the mask is required by the sharing arithmetic - see
// the acquisition note below). SHARING_VIOLATION ⇒ held; NAME_NOT_FOUND ⇒
// never created; success ⇒ free (the probe handle is closed immediately).
LockProbeResult probe_mutation_lock(const StorageRoot& root);

// Acquisition: FILE_OPEN_IF with share 0 (exclusive). Creates the lock file
// on a fresh root, opens it thereafter; SHARING_VIOLATION while another
// operation holds it. `lock_id` is the lock file's identity digest
// (recorded in RUN-START by the caller). Release is RAII: closing the handle
// releases the hold; the lock FILE is never deleted. Never opened with
// DELETE access.
struct MutationLock {
    bool ok = false;
    StorageFailure failure = StorageFailure::IoError;
    std::string error;
    unsigned long native_code = 0;
    platform::unique_handle handle;             // held until destruction
    std::string lock_id;              // the lock file's identity digest
};
MutationLock acquire_mutation_lock(const StorageRoot& root);

// Cross-session continuity: `recorded_lock_id` (the RUN-START value) must
// match the lock file's current identity digest. Mismatch ⇒ refuse.
StorageResult verify_mutation_lock_id(const MutationLock& lock,
                                      std::string_view recorded_lock_id);

// ---------------------------------------------------------------------------
// Journal / segment / anchor leaves.
// ---------------------------------------------------------------------------

// FILE_CREATE (refuse-on-exists) of a fresh journal/segment/anchor leaf
// with GENERIC_WRITE|SYNCHRONIZE share R. `file_name` must be a single safe
// path component (the naming scheme is the caller's).
struct StorageLeaf {
    bool ok = false;
    StorageFailure failure = StorageFailure::IoError;
    std::string error;
    unsigned long native_code = 0;
    platform::unique_handle handle;             // valid when ok
    bool created = false;
    std::string id_hex;                         // identity digest of the leaf (G15 convention v1)
};
StorageLeaf create_journal_leaf(const StorageRoot& root, std::string_view file_name);

// Read open of an existing leaf: FILE_OPEN, FILE_GENERIC_READ, share R|W|D.
// Refuses reparse points and non-regular files (post-open validation).
StorageLeaf open_journal_leaf_read(const StorageRoot& root,
                                   std::string_view file_name);

// Appends `bytes` at the sequential position of an opened leaf and enforces
// the ratified 16 MiB per-file limit (an append that would cross it is
// refused before any byte is written). Ordering across calls is the
// sequential file position; call flush_journal_leaf after each ratified
// write-ahead step.
StorageResult append_journal_leaf_bytes(const platform::unique_handle& leaf,
                                        std::string_view bytes);

// FlushFileBuffers through the leaf handle (durability point for the
// ratified write-ahead ordering). A failure is reported with the native
// code preserved - never silently swallowed.
StorageResult flush_journal_leaf(const platform::unique_handle& leaf);

// ---------------------------------------------------------------------------
// Capacity admission.
// ---------------------------------------------------------------------------

// The exact per-root aggregate byte recompute (to be called while the
// mutation lock is held): sums the current sizes of the files under
// <root-key> (checked arithmetic) and admits `proposed_reserve_bytes` only
// when the aggregate stays within the ratified 128 MiB quota.
struct QuotaCheckResult {
    bool ok = false;                  // false: refused or the check failed
    StorageFailure failure = StorageFailure::IoError;
    std::string error;
    unsigned long native_code = 0;
    unsigned long long existing_bytes = 0;
};
QuotaCheckResult check_aggregate_quota(const StorageRoot& root,
                                       unsigned long long proposed_reserve_bytes);

} // namespace folderpilot
