// FolderPilot - non-recursive, handle-anchored directory scanner implementation.

#include "folderpilot/scanner.hpp"

#include <algorithm>
#include <cstdio>
#include <unordered_set>
#include <utility>

#include "folderpilot/storage_adapter.hpp"
#include "folderpilot/journal.hpp"
#include "native.hpp"
#include "sha256.hpp"
#include "text.hpp"

namespace folderpilot {

namespace {

SkippedEntry skipped(std::string name, SkipReason reason, std::string detail,
                     unsigned long native_code = 0) {
    return SkippedEntry{std::move(name), reason, std::move(detail), native_code};
}

// Enumeration attributes -> skip decision. Cheap filters run BEFORE the
// source open so protected entries are never opened at all.
bool skip_from_enumeration_attributes(unsigned long attributes, SkipReason& reason) {
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) { reason = SkipReason::ReparsePoint; return true; }
    if (attributes & FILE_ATTRIBUTE_HIDDEN)        { reason = SkipReason::Hidden;        return true; }
    if (attributes & FILE_ATTRIBUTE_SYSTEM)        { reason = SkipReason::System;        return true; }
    if (attributes & FILE_ATTRIBUTE_DIRECTORY)     { reason = SkipReason::Directory;     return true; }
    return false;
}

SkipReason skip_reason_from(platform::NativeFailure failure) {
    switch (failure) {
        case platform::NativeFailure::ConfirmedNotFound: return SkipReason::Vanished;
        case platform::NativeFailure::DeletePending:     return SkipReason::Vanished;
        case platform::NativeFailure::AccessDenied:      return SkipReason::AccessDenied;
        case platform::NativeFailure::SharingConflict:   return SkipReason::InUse;
        case platform::NativeFailure::IsADirectory:      return SkipReason::NotRegularFile;
        case platform::NativeFailure::NotADirectory:     return SkipReason::NotRegularFile;
        case platform::NativeFailure::InvalidPath:       return SkipReason::InvalidName;
        case platform::NativeFailure::Unsupported:       return SkipReason::Unsupported;
        case platform::NativeFailure::Conflict:          return SkipReason::OpenFailed;
        case platform::NativeFailure::CrossVolume:       return SkipReason::OpenFailed;
        case platform::NativeFailure::Other:             return SkipReason::OpenFailed;
    }
    return SkipReason::OpenFailed;
}

void process_entry(ScanResult& result, const platform::RootDirectory& root,
                   const platform::RawDirEntry& entry, const ScanOptions& options) {
    // 1. Convert the name to UTF-8 with the strict platform converter.
    const auto name = platform::to_utf8(entry.name);
    if (!name.ok) {
        result.skipped.push_back(skipped({}, SkipReason::EncodingError,
                                         "filename is not valid UTF-8 round-trippable: " + name.error));
        return;
    }

    // 2. Defensive: "." and ".." are never expected from NT enumeration.
    if (name.value == "." || name.value == "..") {
        result.skipped.push_back(skipped(name.value, SkipReason::ReservedName,
                                         "reserved directory alias"));
        return;
    }

    // 3. Cheap protection filters from enumeration attributes.
    SkipReason reason{};
    if (skip_from_enumeration_attributes(entry.attributes, reason)) {
        result.skipped.push_back(skipped(name.value, reason, "filtered by enumeration attributes"));
        return;
    }

    // 4. Test seam (see ScanOptions): the fixture may remove or replace the
    //    entry here to exercise the enumeration/open race deterministically.
    if (options.before_open) {
        options.before_open(name.value);
    }

    // 5. Open the candidate relative to the retained root handle and inspect
    //    the actual opened handle (the source of truth). The handle is closed
    //    by inspect_source before it returns.
    const auto inspection = platform::inspect_source(root.get(), name.value);
    if (!inspection.ok) {
        result.skipped.push_back(
            skipped(name.value, skip_reason_from(inspection.failure), inspection.error,
                    inspection.native_code));
        return;
    }

    // 6. The handle may reveal a different state than the enumeration did.
    if (inspection.attributes.reparse_point()) {
        result.skipped.push_back(skipped(name.value, SkipReason::ReparsePoint,
                                         "entry became a reparse point before opening"));
        return;
    }
    if (inspection.attributes.hidden()) {
        result.skipped.push_back(skipped(name.value, SkipReason::Hidden,
                                         "entry became hidden before opening"));
        return;
    }
    if (inspection.attributes.system()) {
        result.skipped.push_back(skipped(name.value, SkipReason::System,
                                         "entry became a system file before opening"));
        return;
    }

    ScannedFile file;
    file.name_utf8 = std::move(name.value);
    file.identity = std::move(inspection.identity);
    file.size_bytes = static_cast<unsigned long long>(inspection.size_bytes);
    file.last_write_time = inspection.last_write_time;
    file.creation_time_filetime = inspection.creation_time_filetime;
    file.attributes = inspection.attributes.attributes;
    result.files.push_back(std::move(file));
}

bool name_before(const std::string& a, const std::string& b) {
    // Ordinal case-insensitive first; EXACT byte order as the tie-break, so
    // names differing only by case keep both entries and a stable order.
    const int ci = platform::compare_ordinal_ci(a, b);
    if (ci != 0) {
        return ci < 0;
    }
    return a < b;
}

// The 64-bit file index of a captured identity (the same value the
// enumeration records in RawDirEntry::file_id): the low 64 identifier bytes
// for the FileIdInfo form, the high|low index for the legacy form. Returns 0
// when the index cannot be derived (the caller skips the comparison).
unsigned long long file_index64(const platform::SourceIdentity& identity) {
    if (const auto* ex = std::get_if<platform::SourceIdentityEx>(&identity)) {
        const unsigned char* b = ex->file_id.Identifier;
        unsigned long long low = 0;
        for (int i = 7; i >= 0; --i) {
            low = (low << 8) | static_cast<unsigned long long>(b[i]);
        }
        return low;
    }
    if (const auto* lg = std::get_if<platform::SourceIdentityLegacy>(&identity)) {
        return (static_cast<unsigned long long>(lg->file_index_high) << 32) |
               lg->file_index_low;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Phase 8: recursive traversal (docs/PHASE8-DESIGN.md §1). Deterministic
// depth-first walk through parent handles; every step gates and revalidates;
// enumeration failures and the depth/entry limits fail the WHOLE scan.
// ---------------------------------------------------------------------------

// Takes the OWNED UTF-8 name produced inside the enumeration callback:
// RawDirEntry::name is only a view into the enumerator's transient buffer and
// is invalid once enumerate_immediate_children returns (the recursive walk
// stores entries for sorting, so it must never re-read entry.name afterwards).
bool is_root_category_name(std::string_view name) {
    // The seven ratified category destination folders are never traversed as
    // sources at the root level (organizing out of destinations is not a
    // ratified behavior).
    static const char* kCategories[] = {
        "Documents", "Images", "Videos", "Audio", "Archives", "Code", "Other",
    };
    for (const char* cat : kCategories) {
        if (platform::compare_ordinal_ci(name, cat) == 0) {
            return true;
        }
    }
    return false;
}

struct RecursiveWalk {
    const platform::RootDirectory& root;
    const ScanOptions& options;
    ScanResult& result;
    unsigned long long entries = 0;
    bool failed = false;
    std::string error;
    std::unordered_set<std::string> visited; // traversed directory identities (v1 bytes)
    std::size_t directories = 0;        // # of directories actually traversed
};

std::string identity_key(const platform::SourceIdentity& identity) {
    // The G15 convention v1 digest doubles as the visited-set key: same
    // identity => same digest; unrepresentable identities never match.
    std::string material;
    if (!storage_identity_material(identity, material)) {
        return {};
    }
    const auto digest = platform::sha256(
        {reinterpret_cast<const unsigned char*>(material.data()), material.size()});
    if (!digest.ok) {
        return {};
    }
    return journal_hex(digest.digest);
}

void walk_recursive(HANDLE dir_handle, const std::string& rel_prefix, int depth,
                    RecursiveWalk& st) {
    if (st.failed) {
        return;
    }
    if (depth > static_cast<int>(st.options.max_recursive_depth)) {
        st.failed = true;
        st.error = "recursive traversal exceeded the " +
                   std::to_string(st.options.max_recursive_depth) +
                   "-level depth limit at: " + rel_prefix;
        return;
    }

    // This directory's entries are collected FIRST: the enumeration
    // completes before the walk descends anywhere (§1.2), and the entries
    // are ordered per directory by the ratified ordinal-CI + byte-tiebreak
    // comparator - traversal order IS the planning order (§1.1).
    std::vector<std::pair<std::string, platform::RawDirEntry>> entries;
    const auto enumerated = platform::enumerate_immediate_children(
        dir_handle, st.options.buffer_bytes,
        [&](const platform::RawDirEntry& entry) {
            if (entry.name == L"." || entry.name == L"..") {
                return;
            }
            const auto child_name = platform::to_utf8(entry.name);
            if (!child_name.ok) {
                st.result.skipped.push_back(skipped(
                    {}, SkipReason::EncodingError,
                    "filename is not valid UTF-8 round-trippable: " + child_name.error));
                // Encoding failures are skipped entries (§1.2 cap counts them).
                ++st.entries;
                if (st.entries > st.options.max_recursive_entries) {
                    st.failed = true;
                    st.error = "recursive traversal exceeded the " +
                               std::to_string(st.options.max_recursive_entries) +
                               "-entry limit";
                }
                return;
            }
            entries.emplace_back(child_name.value, entry);
        });
    if (!enumerated.ok) {
        st.failed = true;
        st.error = "directory enumeration failed (" + rel_prefix +
                   "): " + enumerated.error;
        return; // fail closed - never an incomplete plan
    }
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) {
                  return name_before(a.first, b.first);
              });

    // Terminal entries (planned files + skipped entries) count toward the
    // ratified 50000-entry limit; traversed directories do not (§1.2).
    const auto count_entry = [&](const std::string& child_rel) -> bool {
        ++st.entries;
        if (st.entries > st.options.max_recursive_entries) {
            st.result.skipped.push_back(skipped(
                child_rel, SkipReason::EntryLimitExceeded,
                "the " + std::to_string(st.options.max_recursive_entries) +
                    "-entry limit was exceeded"));
            st.failed = true;
            st.error = "recursive traversal exceeded the " +
                       std::to_string(st.options.max_recursive_entries) +
                       "-entry limit at: " + child_rel;
            return false;
        }
        return true;
    };

    for (const auto& item : entries) {
        if (st.failed) {
            return;
        }
        const std::string& child_name = item.first;
        const platform::RawDirEntry& entry = item.second;
        const std::string child_rel =
            rel_prefix.empty() ? child_name : rel_prefix + "\\" + child_name;

        // Cheap protection filters from enumeration attributes (both files
        // and directories; a reparse/hidden/system DIRECTORY is skipped
        // without ever being opened). Each skipped entry consumes the budget;
        // a directory that is actually TRAVERSED does not (§1.2).
        if ((entry.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            if (!count_entry(child_rel)) {
                return;
            }
            st.result.skipped.push_back(skipped(child_rel, SkipReason::ReparsePoint,
                                                "filtered by enumeration attributes"));
            continue;
        }
        if ((entry.attributes & FILE_ATTRIBUTE_HIDDEN) != 0) {
            if (!count_entry(child_rel)) {
                return;
            }
            st.result.skipped.push_back(skipped(child_rel, SkipReason::Hidden,
                                                "filtered by enumeration attributes"));
            continue;
        }
        if ((entry.attributes & FILE_ATTRIBUTE_SYSTEM) != 0) {
            if (!count_entry(child_rel)) {
                return;
            }
            st.result.skipped.push_back(skipped(child_rel, SkipReason::System,
                                                "filtered by enumeration attributes"));
            continue;
        }

        if ((entry.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            // Directories: never sources at the root level for the seven
            // category destinations; identity-visited tracking; descend.
            if (depth == 0 && is_root_category_name(child_name)) {
                if (!count_entry(child_rel)) {
                    return;
                }
                st.result.skipped.push_back(
                    skipped(child_rel, SkipReason::CategoryDestination,
                            "a root category destination is never traversed as a source"));
                continue;
            }
            if (depth + 1 > static_cast<int>(st.options.max_recursive_depth)) {
                if (!count_entry(child_rel)) {
                    return;
                }
                st.result.skipped.push_back(
                    skipped(child_rel, SkipReason::DepthLimitExceeded,
                            "descending would exceed the depth limit"));
                st.failed = true;
                st.error = "recursive traversal exceeded the " +
                           std::to_string(st.options.max_recursive_depth) +
                           "-level depth limit at: " + child_rel;
                return;
            }
            const auto child = platform::open_traversal_directory(dir_handle,
                                                                  child_name);
            if (!child.ok) {
                // §1.2 ratified: a subdirectory that cannot be opened or
                // enumerated - FOR ANY REASON - fails the whole scan; an
                // incomplete plan is never presented as complete. The
                // failure names the offending directory.
                st.failed = true;
                st.error = "subdirectory could not be traversed safely (" +
                           child_rel + "): " + child.error;
                return;
            }
            // §1.2 containment: the opened directory's identity must match
            // the identity recorded when the parent was enumerated (a
            // swapped intermediate is a containment compromise - fail
            // closed). The enumeration's 64-bit file index is supplementary;
            // when it is available it must agree with the opened handle.
            if (entry.file_id != 0) {
                const unsigned long long opened = file_index64(child.identity);
                if (opened != 0 && opened != entry.file_id) {
                    st.failed = true;
                    st.error = "directory identity changed between enumeration "
                               "and traversal (" + child_rel + ")";
                    return;
                }
            }
            const std::string key = identity_key(child.identity);
            if (key.empty()) {
                st.failed = true;
                st.error = "a traversed directory identity could not be captured (" +
                           child_rel + ")";
                return;
            }
            if (st.visited.find(key) != st.visited.end()) {
                if (!count_entry(child_rel)) {
                    return;
                }
                st.result.skipped.push_back(
                    skipped(child_rel, SkipReason::DirectoryIdentityDuplicate,
                            "a directory with this identity was already traversed"));
                continue;
            }
            // A traversed directory is NOT a terminal entry: the budget is
            // consumed only by planned files and skipped entries (§1.2).
            // Directories are instead bounded by their own fail-closed cap.
            st.visited.insert(key);
            if (++st.directories > st.options.max_recursive_directories) {
                st.failed = true;
                st.error = "recursive traversal exceeded the " +
                           std::to_string(st.options.max_recursive_directories) +
                           "-directory limit at: " + child_rel;
                return; // fail closed - never an incomplete plan
            }
            walk_recursive(child.handle.get(), child_rel, depth + 1, st);
            continue;
        }

        // A file: this is a terminal entry regardless of whether the open
        // below succeeds (a vanished/inaccessible file is a skipped entry).
        if (!count_entry(child_rel)) {
            return;
        }
        if (st.options.before_open) {
            st.options.before_open(child_rel);
        }
        const auto inspection = platform::inspect_source(dir_handle, child_name);
        if (!inspection.ok) {
            st.result.skipped.push_back(
                skipped(child_rel, skip_reason_from(inspection.failure),
                        inspection.error, inspection.native_code));
            continue;
        }
        if (inspection.attributes.reparse_point()) {
            st.result.skipped.push_back(skipped(child_rel, SkipReason::ReparsePoint,
                                                "entry became a reparse point before opening"));
            continue;
        }
        if (inspection.attributes.hidden()) {
            st.result.skipped.push_back(skipped(child_rel, SkipReason::Hidden,
                                                "entry became hidden before opening"));
            continue;
        }
        if (inspection.attributes.system()) {
            st.result.skipped.push_back(skipped(child_rel, SkipReason::System,
                                                "entry became a system file before opening"));
            continue;
        }
        ScannedFile file;
        file.name_utf8 = child_rel;
        file.identity = std::move(inspection.identity);
        file.size_bytes = static_cast<unsigned long long>(inspection.size_bytes);
        file.last_write_time = inspection.last_write_time;
        file.creation_time_filetime = inspection.creation_time_filetime;
        file.attributes = inspection.attributes.attributes;
        st.result.files.push_back(std::move(file));
    }
}

} // namespace

ScanResult scan_directory(const platform::RootDirectory& root, const ScanOptions& options) {
    ScanResult result;
    result.root.requested_path = root.requested_path_utf8;
    result.root.resolved_path = root.resolved_path_utf8;
    result.root.volume_guid_path = root.volume.volume_guid_path;
    result.root.attributes = root.attributes;
    result.root.identity = root.identity;

    if (!root.valid()) {
        result.enumeration = EnumerationStatus::Failed;
        result.error = "root directory handle is not valid";
        return result; // fail closed - never an apparent empty success
    }

    if (options.recursive) {
        RecursiveWalk walk{root, options, result, 0, false, {}, {}, {}};
        // The root's own identity seeds the visited set (§1.2: EVERY
        // traversed directory's identity is recorded).
        const std::string root_identity = identity_key(root.identity);
        if (root_identity.empty()) {
            result.enumeration = EnumerationStatus::Failed;
            result.error = "the root directory identity could not be captured";
            return result; // fail closed
        }
        walk.visited.insert(root_identity);
        walk_recursive(root.get(), "", 0, walk);
        if (walk.failed) {
            result.enumeration = EnumerationStatus::Failed;
            result.error = walk.error;
            return result; // fail closed - an incomplete plan is never presented
        }
        // No post-walk reordering: the per-directory sorted DFS order IS the
        // ratified planning order (§1.1/§1.2).
        result.enumeration = EnumerationStatus::Ok;
        return result;
    }

    const auto enumeration = platform::enumerate_immediate_children(
        root.get(), options.buffer_bytes,
        [&result, &root, &options](const platform::RawDirEntry& entry) {
            process_entry(result, root, entry, options);
        });
    if (!enumeration.ok) {
        result.enumeration = EnumerationStatus::Failed;
        result.error = enumeration.error;
        return result;
    }

    std::stable_sort(result.files.begin(), result.files.end(),
                     [](const ScannedFile& a, const ScannedFile& b) {
                         return name_before(a.name_utf8, b.name_utf8);
                     });
    std::stable_sort(result.skipped.begin(), result.skipped.end(),
                     [](const SkippedEntry& a, const SkippedEntry& b) {
                         return name_before(a.name_utf8, b.name_utf8);
                     });

    result.enumeration = EnumerationStatus::Ok;
    return result;
}

} // namespace folderpilot
