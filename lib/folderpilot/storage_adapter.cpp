// FolderPilot - Phase 7.1c storage adapter implementation.
//
// The ratified storage/locking policy over the platform storage primitives.
// Every ambiguity resolves to a structured refusal with the native code
// preserved. No function here deletes, truncates or rewrites an existing
// journal, segment, anchor or lock file.

#include "folderpilot/storage_adapter.hpp"

#include <limits>
#include <utility>
#include <variant>

#include "sha256.hpp"
#include "storage.hpp"

namespace folderpilot {

namespace {

// Map a classified platform failure onto the storage taxonomy.
StorageFailure map_failure(platform::NativeFailure failure) {
    switch (failure) {
        case platform::NativeFailure::ConfirmedNotFound:
            return StorageFailure::NameNotFound;
        case platform::NativeFailure::AccessDenied:
            return StorageFailure::AccessDenied;
        case platform::NativeFailure::SharingConflict:
            return StorageFailure::SharingViolation;
        case platform::NativeFailure::InvalidPath:
            return StorageFailure::InvalidName;
        case platform::NativeFailure::Unsupported:
            return StorageFailure::Unsupported;
        case platform::NativeFailure::NotADirectory:
        case platform::NativeFailure::IsADirectory:
            return StorageFailure::NotRegularFile;
        case platform::NativeFailure::Conflict:
            return StorageFailure::NameCollision;
        default:
            return StorageFailure::IoError;
    }
}

StorageResult propagate(const platform::NativeResult& r) {
    StorageResult out;
    out.failure = map_failure(r.failure);
    out.error = r.error;
    out.native_code = r.native_code;
    return out;
}

// The ratified component gate: a storage-chain component must be a genuine,
// non-reparse directory without hidden/system attributes (the FolderPilot-
// created components are gated; the base ancestor is exempt by contract).
StorageResult gate_component(const platform::EntryAttributes& attributes,
                             const char* component) {
    StorageResult out;
    if (attributes.reparse_point()) {
        out.failure = StorageFailure::ReparseLeaf;
        out.error = std::string(component) +
                    " is a reparse point; the storage chain refuses to follow it";
        return out;
    }
    if (attributes.hidden() || attributes.system()) {
        out.failure = StorageFailure::HiddenOrSystem;
        out.error = std::string(component) +
                    " carries hidden/system attributes, which is not an expected "
                    "FolderPilot-created state";
        return out;
    }
    if (!attributes.directory()) {
        out.failure = StorageFailure::NotRegularFile;
        out.error = std::string(component) + " is not a directory";
        return out;
    }
    out.ok = true;
    return out;
}

// The ratified leaf gate: a journal/segment/anchor/lock leaf must be a
// regular, non-reparse file.
StorageResult gate_leaf(const platform::EntryAttributes& attributes) {
    StorageResult out;
    if (attributes.reparse_point()) {
        out.failure = StorageFailure::ReparseLeaf;
        out.error = "the storage leaf is a reparse point; its target is never "
                    "followed or touched";
        return out;
    }
    if (attributes.directory()) {
        out.failure = StorageFailure::NotRegularFile;
        out.error = "the storage leaf is a directory, not a regular file";
        return out;
    }
    out.ok = true;
    return out;
}

StorageResult checked_add(unsigned long long& sum, unsigned long long addend) {
    StorageResult out;
    if (addend > std::numeric_limits<unsigned long long>::max() - sum) {
        out.failure = StorageFailure::QuotaExceeded;
        out.error = "the per-root aggregate size overflows the quota arithmetic";
        return out;
    }
    sum += addend;
    out.ok = true;
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Root key derivation (pure).
// ---------------------------------------------------------------------------

bool storage_identity_material(const platform::SourceIdentity& identity,
                               std::string& out_bytes) {
    if (const auto* ex = std::get_if<platform::SourceIdentityEx>(&identity)) {
        out_bytes.clear();
        out_bytes.push_back('E');
        const auto serial = ex->volume_serial;
        for (int shift = 0; shift < 64; shift += 8) {
            out_bytes.push_back(static_cast<char>((serial >> shift) & 0xFF));
        }
        out_bytes.append(std::begin(ex->file_id.Identifier),
                         std::end(ex->file_id.Identifier));
        return true;
    }
    if (const auto* legacy = std::get_if<platform::SourceIdentityLegacy>(&identity)) {
        out_bytes.clear();
        out_bytes.push_back('L');
        const auto serial = legacy->volume_serial;
        for (int shift = 0; shift < 32; shift += 8) {
            out_bytes.push_back(static_cast<char>((serial >> shift) & 0xFF));
        }
        const auto high = legacy->file_index_high;
        for (int shift = 0; shift < 32; shift += 8) {
            out_bytes.push_back(static_cast<char>((high >> shift) & 0xFF));
        }
        const auto low = legacy->file_index_low;
        for (int shift = 0; shift < 32; shift += 8) {
            out_bytes.push_back(static_cast<char>((low >> shift) & 0xFF));
        }
        return true;
    }
    return false;  // unknown representation: fail closed
}

bool storage_lock_id(const platform::SourceIdentity& identity, std::string& out_hex) {
    std::string material;
    if (!storage_identity_material(identity, material)) {
        return false;
    }
    const auto digest = platform::sha256(
        {reinterpret_cast<const unsigned char*>(material.data()), material.size()});
    if (!digest.ok) {
        return false;
    }
    out_hex = journal_hex(digest.digest);
    return true;
}

bool derive_root_key(const std::string& volume_guid_path,
                     const platform::SourceIdentity& identity, std::string& out_hex) {
    if (volume_guid_path.empty()) {
        return false;
    }
    std::string material;
    if (!storage_identity_material(identity, material)) {
        return false;
    }
    std::string seed = volume_guid_path;
    seed.push_back('\0');
    seed.append(material);
    const auto digest = platform::sha256(
        {reinterpret_cast<const unsigned char*>(seed.data()), seed.size()});
    if (!digest.ok) {
        return false;
    }
    out_hex = journal_hex(digest.digest);
    return true;
}

// ---------------------------------------------------------------------------
// Bootstrap.
// ---------------------------------------------------------------------------

StorageRoot open_storage_root(std::string_view base_path_utf8,
                              const platform::RootDirectory& organized_root) {
    StorageRoot out;
    out.base_path_utf8 = std::string(base_path_utf8);

    if (!organized_root.valid() || organized_root.volume.volume_guid_path.empty()) {
        out.failure = StorageFailure::InvalidName;
        out.error = "storage bootstrap requires a validated organized root with "
                    "volume-GUID identity";
        return out;
    }
    std::string key;
    if (!derive_root_key(organized_root.volume.volume_guid_path,
                         organized_root.identity, key)) {
        out.failure = StorageFailure::IdentityMismatch;
        out.error = "the root key could not be derived from the verified volume "
                    "and file identity";
        return out;
    }
    out.root_key = key;

    // 1. The storage base (the %LOCALAPPDATA% analogue). FILE_OPEN only; the
    //    base is exempt from the hidden/system gate by contract, but a
    //    reparse-pointed base refuses.
    const auto base = platform::open_storage_base(base_path_utf8);
    if (!base.ok) {
        out.failure = map_failure(base.failure);
        out.error = "storage base: " + base.error;
        out.native_code = base.native_code;
        return out;
    }
    if (base.attributes.reparse_point()) {
        out.failure = StorageFailure::ReparseLeaf;
        out.error = "the storage base is a reparse point; the storage chain "
                    "refuses to follow it";
        return out;
    }
    if (!base.attributes.directory()) {
        out.failure = StorageFailure::NotRegularFile;
        out.error = "the storage base is not a directory";
        return out;
    }

    // 2..4. FolderPilot -> journals -> <root-key>, each anchored to the
    // verified parent handle, each gated (reparse, hidden/system, directory).
    auto open_component = [&](HANDLE parent, const char* name)
        -> std::pair<platform::StorageComponentOpen, StorageResult> {
        auto opened = platform::open_storage_component(parent, name);
        if (!opened.ok) {
            return {std::move(opened), propagate({false, opened.failure, opened.error,
                                                  opened.native_code})};
        }
        StorageResult gate = gate_component(opened.attributes, name);
        if (!gate.ok) {
            return {std::move(opened), gate};
        }
        gate.ok = true;
        return {std::move(opened), gate};
    };

    auto pilot = open_component(base.handle.get(), "FolderPilot");
    if (!pilot.second.ok) {
        out.failure = pilot.second.failure;
        out.error = pilot.second.error;
        out.native_code = pilot.first.native_code;
        return out;
    }
    auto journals = open_component(pilot.first.handle.get(), "journals");
    if (!journals.second.ok) {
        out.failure = journals.second.failure;
        out.error = journals.second.error;
        out.native_code = journals.first.native_code;
        return out;
    }

    // 5. ACL validation of the journals directory [G4]: the current user must
    //    hold read/write access. Validation only - never construction.
    const auto acl = platform::validate_current_user_access(
        journals.first.handle.get(), FILE_GENERIC_READ | FILE_GENERIC_WRITE);
    if (!acl.ok) {
        out.failure = StorageFailure::AclValidationFailed;
        out.error = "journals directory ACL validation failed: " + acl.error;
        out.native_code = acl.native_code;
        return out;
    }
    if (!acl.granted) {
        out.failure = StorageFailure::AclValidationFailed;
        out.error = "the journals directory ACL does not grant the current user "
                    "read/write access: " + acl.error;
        return out;
    }

    auto key_dir = open_component(journals.first.handle.get(), out.root_key.c_str());
    if (!key_dir.second.ok) {
        out.failure = key_dir.second.failure;
        out.error = key_dir.second.error;
        out.native_code = key_dir.first.native_code;
        return out;
    }

    out.ok = true;
    out.journals_dir = platform::unique_handle{journals.first.handle.release()};
    out.root_key_dir = platform::unique_handle{key_dir.first.handle.release()};
    return out;
}

// ---------------------------------------------------------------------------
// Mutation lock.
// ---------------------------------------------------------------------------

namespace {
constexpr const char* kMutationLockName = "mutation.lock";

// The ratified post-open leaf gate for the lock file.
StorageResult gate_lock_leaf(const platform::StorageLeafOpen& opened) {
    StorageResult out;
    if (opened.attributes.reparse_point()) {
        out.failure = StorageFailure::ReparseLeaf;
        out.error = "the mutation lock is a reparse point; its target is never "
                    "followed or touched";
        return out;
    }
    if (opened.attributes.directory()) {
        out.failure = StorageFailure::NotRegularFile;
        out.error = "the mutation lock is not a regular file";
        return out;
    }
    out.ok = true;
    return out;
}
} // namespace

LockProbeResult probe_mutation_lock(const StorageRoot& root) {
    LockProbeResult out;
    if (!root.ok) {
        out.failure = StorageFailure::InvalidName;
        out.error = "the mutation lock probe requires a validated storage root";
        return out;
    }
    // Attributes-inspection probe with full sharing: SHARING_VIOLATION ⇒
    // held; NAME_NOT_FOUND ⇒ never created; success ⇒ free (closed
    // immediately). SYNCHRONIZE pairs with the synchronous open.
    // FILE_READ_DATA is REQUIRED by the sharing arithmetic: attributes-only
    // access is exempt from sharing checks, so without a data bit the probe
    // could never observe the holder's share-0 and Held would be unreachable
    // (runtime evidence 2026-11-18). The probe still inspects attributes
    // only and closes immediately.
    auto opened = platform::open_storage_leaf(
        root.root_key_dir.get(), kMutationLockName,
        SYNCHRONIZE | FILE_READ_ATTRIBUTES | FILE_READ_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        FILE_OPEN);
    if (!opened.ok) {
        if (opened.failure == platform::NativeFailure::ConfirmedNotFound) {
            out.ok = true;
            out.state = LockState::NeverCreated;
            return out;
        }
        if (opened.failure == platform::NativeFailure::SharingConflict) {
            out.ok = true;
            out.state = LockState::Held;
            return out;
        }
        out.failure = map_failure(opened.failure);
        out.error = "mutation lock probe: " + opened.error;
        out.native_code = opened.native_code;
        return out;
    }
    const auto gate = gate_lock_leaf(opened);
    if (!gate.ok) {
        out.failure = gate.failure;
        out.error = gate.error;
        return out;
    }
    out.ok = true;
    out.state = LockState::Free;  // the probe handle closes with `opened`
    return out;
}

MutationLock acquire_mutation_lock(const StorageRoot& root) {
    MutationLock out;
    if (!root.ok) {
        out.failure = StorageFailure::InvalidName;
        out.error = "the mutation lock requires a validated storage root";
        return out;
    }
    // The ratified two-row table: the first acquisition (the lock file was
    // never created) uses FILE_OPEN_IF; later acquisitions use FILE_OPEN —
    // an externally deleted lock refuses instead of being recreated. The
    // ratified probe supplies the row. Share 0 rejects every concurrent open
    // [W32-DOC sharing arithmetic]: FILE_READ_DATA in the access mask makes
    // that arithmetic effective - attributes-only access is exempt from
    // sharing checks, so a second acquirer requesting only
    // SYNCHRONIZE|FILE_READ_ATTRIBUTES would otherwise never conflict with
    // the holder's share 0 and exclusivity would be void (runtime evidence
    // 2026-11-18).
    auto probe = probe_mutation_lock(root);
    if (!probe.ok) {
        out.failure = probe.failure;
        out.error = "mutation lock probe before acquisition: " + probe.error;
        out.native_code = probe.native_code;
        return out;
    }
    if (probe.state == LockState::Held) {
        out.failure = StorageFailure::SharingViolation;
        out.error = "another FolderPilot operation holds the mutation lock";
        return out;
    }
    const unsigned long disposition = probe.state == LockState::NeverCreated
                                          ? FILE_OPEN_IF
                                          : FILE_OPEN;
    auto opened = platform::open_storage_leaf(
        root.root_key_dir.get(), kMutationLockName,
        SYNCHRONIZE | FILE_READ_ATTRIBUTES | FILE_READ_DATA, 0, disposition);
    if (!opened.ok) {
        if (opened.failure == platform::NativeFailure::SharingConflict) {
            out.failure = StorageFailure::SharingViolation;
            out.error = "another FolderPilot operation holds the mutation lock";
            out.native_code = opened.native_code;
            return out;
        }
        out.failure = map_failure(opened.failure);
        out.error = "mutation lock acquisition: " + opened.error;
        out.native_code = opened.native_code;
        return out;
    }
    const auto gate = gate_lock_leaf(opened);
    if (!gate.ok) {
        out.failure = gate.failure;
        out.error = gate.error;
        return out;
    }
    std::string lock_id;
    if (!storage_lock_id(opened.identity, lock_id)) {
        out.failure = StorageFailure::IdentityMismatch;
        out.error = "the mutation lock identity could not be captured";
        return out;
    }

    out.ok = true;
    out.handle = platform::unique_handle{opened.handle.release()};
    out.lock_id = std::move(lock_id);
    return out;
}

StorageResult verify_mutation_lock_id(const MutationLock& lock,
                                      std::string_view recorded_lock_id) {
    StorageResult out;
    if (!lock.ok || !lock.handle.valid()) {
        out.failure = StorageFailure::InvalidName;
        out.error = "lock-id verification requires a held mutation lock";
        return out;
    }
    if (recorded_lock_id.empty()) {
        out.failure = StorageFailure::IdentityMismatch;
        out.error = "the recorded lock_id is absent; cross-session continuity "
                    "cannot be verified";
        return out;
    }
    if (recorded_lock_id != lock.lock_id) {
        out.failure = StorageFailure::IdentityMismatch;
        out.error = "the recorded lock_id does not match the mutation lock's "
                    "verified identity";
        return out;
    }
    out.ok = true;
    return out;
}

// ---------------------------------------------------------------------------
// Journal / segment / anchor leaves.
// ---------------------------------------------------------------------------

namespace {
StorageFailure validate_leaf_name(std::string_view file_name) {
    if (file_name.empty() || file_name == "." || file_name == ".." ||
        file_name.find_first_of("/\\:") != std::string_view::npos) {
        return StorageFailure::InvalidName;
    }
    return StorageFailure::Ok;
}
} // namespace

StorageLeaf create_journal_leaf(const StorageRoot& root, std::string_view file_name) {
    StorageLeaf out;
    if (!root.ok) {
        out.failure = StorageFailure::InvalidName;
        out.error = "leaf creation requires a validated storage root";
        return out;
    }
    if (const auto bad = validate_leaf_name(file_name); bad != StorageFailure::Ok) {
        out.failure = bad;
        out.error = "the leaf name is not a single safe path component";
        return out;
    }
    // FILE_CREATE (refuse-on-exists) with GENERIC_WRITE|SYNCHRONIZE share R
    // (GENERIC_WRITE is required by FlushFileBuffers [W32-DOC]; share R keeps
    // preview readers compatible). FILE_READ_ATTRIBUTES is required by the
    // ratified post-open validation: FileAttributeTagInfo and the identity
    // capture fail with ERROR_ACCESS_DENIED against a write-only handle
    // (runtime evidence 2026-11-18).
    auto opened = platform::open_storage_leaf(
        root.root_key_dir.get(), file_name,
        GENERIC_WRITE | FILE_READ_ATTRIBUTES | SYNCHRONIZE, FILE_SHARE_READ,
        FILE_CREATE);
    if (!opened.ok) {
        if (opened.failure == platform::NativeFailure::Conflict) {
            out.failure = StorageFailure::NameCollision;
            out.error = "the leaf already exists; journals, segments and anchors "
                        "are never overwritten";
            out.native_code = opened.native_code;
            return out;
        }
        out.failure = map_failure(opened.failure);
        out.error = "leaf creation: " + opened.error;
        out.native_code = opened.native_code;
        return out;
    }
    const auto gate = gate_leaf(opened.attributes);
    if (!gate.ok) {
        out.failure = gate.failure;
        out.error = gate.error;
        return out;
    }

    out.ok = true;
    out.handle = platform::unique_handle{opened.handle.release()};
    out.created = opened.created;
    return out;
}

StorageLeaf open_journal_leaf_read(const StorageRoot& root,
                                   std::string_view file_name) {
    StorageLeaf out;
    if (!root.ok) {
        out.failure = StorageFailure::InvalidName;
        out.error = "leaf read requires a validated storage root";
        return out;
    }
    if (const auto bad = validate_leaf_name(file_name); bad != StorageFailure::Ok) {
        out.failure = bad;
        out.error = "the leaf name is not a single safe path component";
        return out;
    }
    // FILE_OPEN, FILE_GENERIC_READ, share R|W|D: reader-compatible with the
    // writer's GENERIC_WRITE share R [W32-DOC sharing arithmetic].
    auto opened = platform::open_storage_leaf(
        root.root_key_dir.get(), file_name,
        FILE_GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        FILE_OPEN);
    if (!opened.ok) {
        if (opened.failure == platform::NativeFailure::ConfirmedNotFound) {
            out.failure = StorageFailure::NameNotFound;
            out.error = "the leaf does not exist";
            out.native_code = opened.native_code;
            return out;
        }
        out.failure = map_failure(opened.failure);
        out.error = "leaf read open: " + opened.error;
        out.native_code = opened.native_code;
        return out;
    }
    const auto gate = gate_leaf(opened.attributes);
    if (!gate.ok) {
        out.failure = gate.failure;
        out.error = gate.error;
        return out;
    }

    out.ok = true;
    out.handle = platform::unique_handle{opened.handle.release()};
    out.created = opened.created;
    return out;
}

StorageResult append_journal_leaf_bytes(const platform::unique_handle& leaf,
                                        std::string_view bytes) {
    if (!leaf.valid()) {
        return {false, StorageFailure::InvalidName,
                "append requires an opened leaf handle", 0};
    }
    // The ratified 16 MiB per-file limit: an append that would cross it is
    // refused before any byte is written (fail-closed, no partial write).
    long long current = 0;
    if (const auto size = platform::storage_file_size(leaf.get(), current); !size.ok) {
        return propagate(size);
    }
    const unsigned long long limit = journal_max_file_bytes;
    const auto bytes_to_write = static_cast<unsigned long long>(bytes.size());
    if (bytes_to_write > limit ||
        static_cast<unsigned long long>(current) > limit - bytes_to_write) {
        return {false, StorageFailure::QuotaExceeded,
                "the append would exceed the ratified 16 MiB per-file limit", 0};
    }
    const auto written = platform::write_storage_file(
        leaf.get(), reinterpret_cast<const unsigned char*>(bytes.data()),
        bytes.size());
    if (!written.ok) {
        return propagate(written);
    }
    return {true, StorageFailure::Ok, {}, 0};
}

StorageResult flush_journal_leaf(const platform::unique_handle& leaf) {
    if (!leaf.valid()) {
        return {false, StorageFailure::InvalidName,
                "flush requires an opened leaf handle", 0};
    }
    const auto flushed = platform::flush_storage_file(leaf.get());
    if (!flushed.ok) {
        return propagate(flushed);
    }
    return {true, StorageFailure::Ok, {}, 0};
}

// ---------------------------------------------------------------------------
// Capacity admission.
// ---------------------------------------------------------------------------

QuotaCheckResult check_aggregate_quota(const StorageRoot& root,
                                       unsigned long long proposed_reserve_bytes) {
    QuotaCheckResult out;
    if (!root.ok || !root.root_key_dir.valid()) {
        out.failure = StorageFailure::InvalidName;
        out.error = "the aggregate quota check requires a validated storage root";
        return out;
    }
    // The exact per-root byte recompute: the current sizes of every file
    // under <root-key>, summed with checked arithmetic. No directory is a
    // legitimate storage entry; a planted one is an unexpected state that
    // undercounts the true aggregate - fail closed on it (the adapter never
    // deletes, so it never cleans one up either).
    unsigned long long existing = 0;
    bool saw_overflow = false;
    bool saw_directory = false;
    std::string enumerate_error;
    const auto enumerated = platform::enumerate_immediate_children(
        root.root_key_dir.get(), 64 * 1024,
        [&](const platform::RawDirEntry& entry) {
            // "." and ".." are prefix records some filesystem surfaces return
            // in first/restart queries (docs/ARCHITECTURE.md section 9, the
            // scanner's ReservedName skip). They are not storage entries and
            // must not trigger the planted-directory refusal. Every OTHER
            // directory under <root-key> remains an unexpected state that
            // invalidates the exact recompute; enumeration errors and size
            // overflow still fail closed below.
            if (entry.name == L"." || entry.name == L"..") {
                return;
            }
            if (entry.attributes & FILE_ATTRIBUTE_DIRECTORY) {
                saw_directory = true;
                return;
            }
            const auto added = checked_add(existing,
                                           static_cast<unsigned long long>(
                                               entry.end_of_file));
            if (!added.ok) {
                saw_overflow = true;
                enumerate_error = added.error;
            }
        });
    if (!enumerated.ok || saw_overflow || saw_directory) {
        out.failure = saw_directory ? StorageFailure::NotRegularFile
                                    : StorageFailure::IoError;
        out.error = saw_directory
                        ? "an unexpected directory entry exists under the "
                          "<root-key> storage directory; the exact aggregate "
                          "recompute cannot be trusted"
                        : (saw_overflow ? enumerate_error
                                        : "per-root size enumeration failed: " +
                                              enumerated.error);
        out.native_code = enumerated.native_code;
        return out;
    }
    out.existing_bytes = existing;

    const unsigned long long quota = journal_max_aggregate_bytes;
    if (existing > quota || proposed_reserve_bytes > quota - existing) {
        out.failure = StorageFailure::QuotaExceeded;
        out.error = "the per-root aggregate quota of 128 MiB is exhausted: " +
                    std::to_string(existing) + " bytes present + " +
                    std::to_string(proposed_reserve_bytes) + " bytes requested > " +
                    std::to_string(quota) + " bytes";
        return out;
    }
    out.ok = true;
    return out;
}

} // namespace folderpilot
