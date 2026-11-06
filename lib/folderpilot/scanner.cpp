// FolderPilot - non-recursive, handle-anchored directory scanner implementation.

#include "folderpilot/scanner.hpp"

#include <algorithm>
#include <utility>

#include "native.hpp"
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
