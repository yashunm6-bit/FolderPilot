// FolderPilot - platform native file handling implementation.
//
// Every semi-documented NT surface (NtCreateFile, RtlDosPathNameToNtPathName_U,
// RtlFreeUnicodeString) is resolved AT RUNTIME from ntdll.dll. No NT
// prototype is re-declared anywhere in this project, so nothing can conflict
// with the system headers. All structures (UNICODE_STRING, OBJECT_ATTRIBUTES,
// IO_STATUS_BLOCK, STATUS_* constants, FILE_ID_BOTH_DIR_INFO, ...) come from
// the system headers <winternl.h>, <ntstatus.h> and <winbase.h> - verified by
// a compile probe before Phase 3 implementation was written.

#include "native.hpp"

#ifndef WIN32_NO_STATUS
#define WIN32_NO_STATUS
#endif
#include <Windows.h>
#undef WIN32_NO_STATUS
#include <winternl.h>
#include <ntstatus.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include "text.hpp"

namespace folderpilot::platform {

namespace {

// Documented NTSTATUS value (0xC0000054). MinGW's ntstatus.h does not export
// this particular constant, so it is declared here under a project-local
// name - a constant, not a prototype, so nothing can conflict.
constexpr LONG kStatusLockViolation = static_cast<LONG>(0xC0000054L);

// ---- runtime-resolved ntdll functions --------------------------------------

using NtCreateFile_fn = LONG (NTAPI*)(
    PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
    PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
using RtlDosPathNameToNtPathName_U_fn =
    BOOLEAN (NTAPI*)(PCWSTR, PUNICODE_STRING, PWSTR*, PVOID);
using RtlFreeUnicodeString_fn = void (NTAPI*)(PUNICODE_STRING);

struct NativeApis {
    NtCreateFile_fn nt_create_file = nullptr;
    RtlDosPathNameToNtPathName_U_fn dos_to_nt = nullptr;
    RtlFreeUnicodeString_fn free_unicode_string = nullptr;
    bool ok() const { return nt_create_file != nullptr && dos_to_nt != nullptr && free_unicode_string != nullptr; }
};

const NativeApis& native_apis() {
    static const NativeApis apis = [] {
        NativeApis resolved{};
        const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        if (ntdll != nullptr) {
            resolved.nt_create_file = reinterpret_cast<NtCreateFile_fn>(
                reinterpret_cast<void*>(::GetProcAddress(ntdll, "NtCreateFile")));
            resolved.dos_to_nt = reinterpret_cast<RtlDosPathNameToNtPathName_U_fn>(
                reinterpret_cast<void*>(::GetProcAddress(ntdll, "RtlDosPathNameToNtPathName_U")));
            resolved.free_unicode_string = reinterpret_cast<RtlFreeUnicodeString_fn>(
                reinterpret_cast<void*>(::GetProcAddress(ntdll, "RtlFreeUnicodeString")));
        }
        return resolved;
    }();
    return apis;
}

// ---- small helpers ----------------------------------------------------------

std::string ntstatus_message(const char* what, LONG status) {
    static constexpr const char* digits = "0123456789ABCDEF";
    std::string msg = std::string(what) + " failed (NTSTATUS 0x";
    for (int shift = 28; shift >= 0; shift -= 4) {
        msg += digits[(static_cast<unsigned long>(status) >> shift) & 0xF];
    }
    msg += ")";
    return msg;
}

std::string win32_message(const char* what, DWORD code) {
    return std::string(what) + " failed (Windows error " + std::to_string(code) + ")";
}

bool is_ascii_alpha(wchar_t c) {
    return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
}

} // namespace

NativeFailure classify_ntstatus(LONG status) {
    if (status == STATUS_OBJECT_NAME_NOT_FOUND || status == STATUS_OBJECT_PATH_NOT_FOUND) {
        return NativeFailure::ConfirmedNotFound;
    }
    if (status == STATUS_ACCESS_DENIED) {
        return NativeFailure::AccessDenied;
    }
    if (status == STATUS_OBJECT_NAME_INVALID || status == STATUS_INVALID_PARAMETER) {
        return NativeFailure::InvalidPath;
    }
    if (status == STATUS_SHARING_VIOLATION || status == kStatusLockViolation) {
        return NativeFailure::SharingConflict;
    }
    if (status == STATUS_DELETE_PENDING) {
        return NativeFailure::DeletePending;
    }
    if (status == STATUS_NOT_A_DIRECTORY) {
        return NativeFailure::NotADirectory;
    }
    if (status == STATUS_FILE_IS_A_DIRECTORY) {
        return NativeFailure::IsADirectory;
    }
    return NativeFailure::Other;
}

namespace {

// Handle queries used by the bootstrap and by source inspection.
NativeResult query_attribute_tag_info(HANDLE handle, EntryAttributes& out) {
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (!::GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info, sizeof(info))) {
        return {false, NativeFailure::Other,
                win32_message("GetFileInformationByHandleEx(FileAttributeTagInfo)", ::GetLastError()),
                ::GetLastError()};
    }
    out = EntryAttributes{true, info.FileAttributes, info.ReparseTag};
    return {true, NativeFailure::Other, {}, 0};
}

NativeResult capture_identity(HANDLE handle, SourceIdentity& out) {
    FILE_ID_INFO info{};
    if (::GetFileInformationByHandleEx(handle, FileIdInfo, &info, sizeof(info))) {
        out = SourceIdentityEx{info.VolumeSerialNumber, info.FileId};
        return {true, NativeFailure::Other, {}, 0};
    }
    const DWORD err = ::GetLastError();
    if (err == ERROR_INVALID_PARAMETER || err == ERROR_NOT_SUPPORTED) {
        // Legacy fallback - kept as a STRICTLY SEPARATE representation.
        BY_HANDLE_FILE_INFORMATION legacy{};
        if (::GetFileInformationByHandle(handle, &legacy)) {
            out = SourceIdentityLegacy{legacy.dwVolumeSerialNumber,
                                       legacy.nFileIndexHigh, legacy.nFileIndexLow};
            return {true, NativeFailure::Other, {}, 0};
        }
        return {false, NativeFailure::Unsupported,
                win32_message("GetFileInformationByHandle", ::GetLastError()), ::GetLastError()};
    }
    return {false, NativeFailure::Other, win32_message("GetFileInformationByHandleEx(FileIdInfo)", err), err};
}

// Resolved final path through a handle, normalized to a plain drive form
// ("C:\..."). Extended-length and UNC prefixes are recognized explicitly,
// never stripped by blind string surgery.
NativeResult resolved_final_path(HANDLE handle, std::string& out_utf8) {
    std::wstring wide;
    std::size_t size = 1024;
    while (wide.size() < 65536) {
        wide.resize(size);
        const DWORD written = ::GetFinalPathNameByHandleW(
            handle, wide.data(), static_cast<DWORD>(wide.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (written == 0) {
            return {false, NativeFailure::Other,
                    win32_message("GetFinalPathNameByHandleW", ::GetLastError()), ::GetLastError()};
        }
        if (written < wide.size()) {
            wide.resize(written); // written excludes the terminator on success
            break;
        }
        size = written + 1; // buffer was too small; retry with the required size
    }
    if (wide.size() >= 65536) {
        return {false, NativeFailure::Other, "resolved path exceeds the supported length", 0};
    }

    std::wstring_view view = wide;
    if (view.starts_with(L"\\\\?\\UNC\\")) {
        return {false, NativeFailure::InvalidPath, "target resolves to a UNC/network location", 0};
    }
    if (view.starts_with(L"\\\\?\\")) {
        view.remove_prefix(4); // the one documented extended-length prefix
    }
    if (view.size() < 3 || !is_ascii_alpha(view[0]) || view[1] != L':') {
        return {false, NativeFailure::Other, "resolved path has an unrecognized form", 0};
    }

    const auto narrow = to_utf8(view);
    if (!narrow.ok) {
        return {false, NativeFailure::Other, "resolved path is not valid UTF-8 round-trippable", 0};
    }
    out_utf8 = narrow.value;
    return {true, NativeFailure::Other, {}, 0};
}

} // namespace

std::wstring normalize_volume_mount_point(std::wstring_view mount_point) {
    std::wstring normalized{mount_point};
    // Strip ALL trailing backslashes, then append EXACTLY one.
    while (!normalized.empty() && normalized.back() == L'\\') {
        normalized.pop_back();
    }
    normalized.push_back(L'\\');
    return normalized;
}

NativeResult volume_identity_for_path(std::wstring_view dos_path, VolumeIdentity& out) {
    // Null-terminated working copy for the Win32 APIs.
    const std::wstring path{dos_path};

    std::wstring mount_point(1024, L'\0');
    const DWORD written = ::GetVolumePathNameW(
        path.c_str(), mount_point.data(), static_cast<DWORD>(mount_point.size()));
    if (written == 0) {
        return {false, NativeFailure::Other,
                win32_message("GetVolumePathNameW", ::GetLastError()), ::GetLastError()};
    }
    // GetVolumePathNameW's return length INCLUDES the terminating null -
    // drop it so the normalization below sees the real mount point.
    mount_point.resize(written - 1);
    mount_point = normalize_volume_mount_point(mount_point);

    wchar_t guid[100]{}; // documented maximum size for volume GUID paths
    if (!::GetVolumeNameForVolumeMountPointW(mount_point.c_str(), guid, 100)) {
        return {false, NativeFailure::Unsupported,
                win32_message("GetVolumeNameForVolumeMountPointW", ::GetLastError()), ::GetLastError()};
    }

    // Only local volume GUIDs are supported; anything else fails closed.
    const std::wstring_view guid_view = guid;
    if (!guid_view.starts_with(L"\\\\?\\Volume{") || !guid_view.ends_with(L"}\\")) {
        return {false, NativeFailure::Unsupported, "volume is not a local volume (network or special volume)", 0};
    }

    const auto narrow = to_utf8(guid_view);
    if (!narrow.ok) {
        return {false, NativeFailure::Other, "volume GUID path is not valid UTF-8 round-trippable", 0};
    }
    out.volume_guid_path = narrow.value;
    return {true, NativeFailure::Other, {}, 0};
}

// ---------------------------------------------------------------------------
// Pure helpers.
// ---------------------------------------------------------------------------

std::optional<NativeFailure> classify_dos_path(std::wstring_view path) {
    if (path.empty()) {
        return NativeFailure::InvalidPath;
    }
    for (const wchar_t c : path) {
        if (c == L'\0') {
            return NativeFailure::InvalidPath; // embedded null: truncated path
        }
    }
    if (path.find(L'/') != std::wstring_view::npos) {
        return NativeFailure::InvalidPath; // strict backslash-only form
    }
    if (path.starts_with(L"\\\\")) {
        return NativeFailure::InvalidPath; // UNC or device namespace (\\server, \\.\, \\?\)
    }
    if (path.size() < 3 || !is_ascii_alpha(path[0]) || path[1] != L':' || path[2] != L'\\') {
        // Covers: relative paths, root-relative (\foo), drive-relative (C:foo, C:).
        return NativeFailure::InvalidPath;
    }
    if (path.find(L':', 2) != std::wstring_view::npos) {
        return NativeFailure::InvalidPath; // alternate data stream syntax
    }
    return std::nullopt;
}

int compare_ordinal_ci(std::string_view a, std::string_view b) {
    const auto wa = to_utf16(a);
    const auto wb = to_utf16(b);
    if (!wa.ok || !wb.ok) {
        // Deterministic fallback for input that cannot round-trip (cannot
        // originate from a real NT path); byte comparison keeps ordering stable.
        return a < b ? -1 : (b < a ? 1 : 0);
    }
    const int r = ::CompareStringOrdinal(wa.value.c_str(), static_cast<int>(wa.value.size()),
                                         wb.value.c_str(), static_cast<int>(wb.value.size()), TRUE);
    if (r == CSTR_EQUAL) {
        return 0;
    }
    return r == CSTR_LESS_THAN ? -1 : 1;
}

std::optional<std::string> current_user_home() {
    std::wstring home(MAX_PATH, L'\0');
    for (;;) {
        const DWORD needed = ::GetEnvironmentVariableW(L"USERPROFILE", home.data(),
                                                       static_cast<DWORD>(home.size()));
        if (needed == 0) {
            return std::nullopt; // not set - cannot determine
        }
        if (needed <= home.size()) {
            home.resize(needed);
            break;
        }
        home.resize(needed); // includes the terminator
    }
    while (home.size() > 2 && home.back() == L'\\') {
        home.pop_back();
    }
    const auto narrow = to_utf8(home);
    if (!narrow.ok) {
        return std::nullopt;
    }
    return narrow.value;
}

// ---------------------------------------------------------------------------
// Enumeration buffer parsing (pure).
// ---------------------------------------------------------------------------

DirParseResult parse_id_both_dir_entries(const unsigned char* data, std::size_t bytes) {
    DirParseResult result;
    if (bytes == 0) {
        return result; // empty batch: valid, zero entries
    }

    // All fixed header fields up to (and including) the file id must be
    // present before anything is read. The file id ends exactly where the
    // file name begins, so the required header is offsetof(..., FileName).
    constexpr std::size_t kHeaderBytes = offsetof(FILE_ID_BOTH_DIR_INFO, FileName);

    std::size_t offset = 0;
    for (;;) {
        if (offset + kHeaderBytes > bytes) {
            result.status = DirParseStatus::TruncatedHeader;
            result.error = "entry header extends past the buffer";
            return result;
        }

        unsigned long next_entry = 0;
        unsigned long attributes = 0;
        unsigned long name_length = 0;
        long long end_of_file = 0;
        long long last_write_time = 0;
        unsigned long long file_id = 0;
        std::memcpy(&next_entry, data + offset + offsetof(FILE_ID_BOTH_DIR_INFO, NextEntryOffset), sizeof(next_entry));
        std::memcpy(&attributes, data + offset + offsetof(FILE_ID_BOTH_DIR_INFO, FileAttributes), sizeof(attributes));
        std::memcpy(&name_length, data + offset + offsetof(FILE_ID_BOTH_DIR_INFO, FileNameLength), sizeof(name_length));
        std::memcpy(&end_of_file, data + offset + offsetof(FILE_ID_BOTH_DIR_INFO, EndOfFile), sizeof(end_of_file));
        std::memcpy(&last_write_time, data + offset + offsetof(FILE_ID_BOTH_DIR_INFO, LastWriteTime), sizeof(last_write_time));
        std::memcpy(&file_id, data + offset + offsetof(FILE_ID_BOTH_DIR_INFO, FileId), sizeof(file_id));

        if (next_entry != 0 && next_entry % 8 != 0) {
            result.status = DirParseStatus::MisalignedEntry;
            result.error = "NextEntryOffset is not 8-byte aligned";
            return result;
        }

        if (name_length % 2 != 0) {
            result.status = DirParseStatus::BadNameLength;
            result.error = "FileNameLength is not a whole number of UTF-16 code units";
            return result;
        }

        const std::size_t name_offset = offset + kHeaderBytes;
        if (name_offset + name_length > bytes) {
            result.status = DirParseStatus::NameOutOfBounds;
            result.error = "filename bytes extend past the buffer";
            return result;
        }

        result.entries.push_back(RawDirEntry{
            std::wstring_view{reinterpret_cast<const wchar_t*>(data + name_offset), name_length / 2},
            attributes, end_of_file, last_write_time, file_id});

        if (next_entry == 0) {
            break; // last entry in this batch
        }
        if (offset + next_entry > bytes) {
            result.status = DirParseStatus::EntryOutOfBounds;
            result.error = "NextEntryOffset points past the buffer";
            return result;
        }
        offset += next_entry;
    }
    return result;
}

// ---------------------------------------------------------------------------
// Enumeration loop.
// ---------------------------------------------------------------------------

NativeResult enumerate_immediate_children(
    HANDLE root, std::size_t buffer_bytes,
    const std::function<void(const RawDirEntry&)>& visitor) {
    if (root == nullptr) {
        return {false, NativeFailure::Other, "root handle is not valid", 0};
    }
    // Large enough for any single legal entry (32k-char name + fixed fields);
    // 64 KiB is the customary size. 8-byte alignment via the allocator.
    const std::size_t buffer_size = std::max<std::size_t>(buffer_bytes, 64 * 1024);
    std::vector<std::byte> buffer(buffer_size);

    FILE_INFO_BY_HANDLE_CLASS info_class = FileIdBothDirectoryRestartInfo;
    for (;;) {
        // GetFileInformationByHandleEx returns only success/failure - it does
        // NOT report how many bytes were written. The valid region therefore
        // ends at the entry whose NextEntryOffset is 0, which the parser
        // uses; the whole buffer is handed to it as the bounds.
        if (!::GetFileInformationByHandleEx(root, info_class, buffer.data(),
                                            static_cast<DWORD>(buffer.size()))) {
            const DWORD err = ::GetLastError();
            if (err == ERROR_NO_MORE_FILES) {
                return {true, NativeFailure::Other, {}, 0}; // clean end of enumeration
            }
            if (err == ERROR_INSUFFICIENT_BUFFER) {
                return {false, NativeFailure::Other,
                        "a single directory entry exceeds the enumeration buffer", err};
            }
            return {false, NativeFailure::Other,
                    win32_message("GetFileInformationByHandleEx(directory)", err), err};
        }

        const auto parsed = parse_id_both_dir_entries(
            reinterpret_cast<const unsigned char*>(buffer.data()), buffer.size());
        if (parsed.status != DirParseStatus::Ok) {
            return {false, NativeFailure::Other, "malformed enumeration buffer: " + parsed.error, 0};
        }
        for (const RawDirEntry& entry : parsed.entries) {
            visitor(entry);
        }
        info_class = FileIdBothDirectoryInfo; // subsequent batches
    }
}

// ---------------------------------------------------------------------------
// Root bootstrap open.
// ---------------------------------------------------------------------------

RootOpenResult open_root(std::string_view utf8_path) {
    RootOpenResult result;

    if (!native_apis().ok()) {
        result.failure = NativeFailure::Unsupported;
        result.error = "required ntdll functions (NtCreateFile, RtlDosPathNameToNtPathName_U) are unavailable";
        return result;
    }

    // 1. Strict UTF-8 -> UTF-16 conversion of the user-supplied path.
    const auto wide = to_utf16(utf8_path);
    if (!wide.ok) {
        result.failure = NativeFailure::InvalidPath;
        result.error = "target path is not valid UTF-8: " + wide.error;
        return result;
    }

    // 2. Pure DOS-form classification (relative / UNC / device / ADS / ...).
    if (const auto bad_form = classify_dos_path(wide.value)) {
        result.failure = *bad_form;
        result.error = "unsupported target path form";
        return result;
    }

    // 3. Correct native conversion to the NT namespace (\??\...).
    UNICODE_STRING nt_name{};
    if (native_apis().dos_to_nt(wide.value.c_str(), &nt_name, nullptr, nullptr) == 0) {
        result.failure = NativeFailure::Other;
        result.error = "could not convert the target path to NT form";
        return result;
    }
    struct NtNameGuard {
        UNICODE_STRING name;
        RtlFreeUnicodeString_fn free_fn;
        ~NtNameGuard() { free_fn(&name); }
    } nt_guard{nt_name, native_apis().free_unicode_string};

    // Verify the actual NT representation (never trust blind prefix edits).
    const std::wstring_view nt_view{nt_name.Buffer, nt_name.Length / sizeof(WCHAR)};
    if (!nt_view.starts_with(L"\\??\\")) {
        result.failure = NativeFailure::Other;
        result.error = "NT path conversion produced an unrecognized form";
        return result;
    }
    const std::wstring_view nt_body = nt_view.substr(4);
    if (nt_body.starts_with(L"UNC")) {
        result.failure = NativeFailure::InvalidPath;
        result.error = "UNC/network targets are not supported";
        return result;
    }
    if (nt_body.size() < 2 || !is_ascii_alpha(nt_body[0]) || nt_body[1] != L':') {
        result.failure = NativeFailure::Other;
        result.error = "NT path conversion produced a non-drive form";
        return result;
    }

    // 4. THE bootstrap open. Access supports listing, inspection, traversal
    //    and (future-phase) directory creation; the leaf is opened without
    //    following a reparse point; no FILE_SHARE_DELETE.
    OBJECT_ATTRIBUTES oa{};
    oa.Length = sizeof(oa);
    oa.RootDirectory = nullptr;
    oa.Attributes = OBJ_CASE_INSENSITIVE;
    oa.ObjectName = &nt_name;
    oa.SecurityDescriptor = nullptr;
    oa.SecurityQualityOfService = nullptr;

    IO_STATUS_BLOCK iosb{};
    HANDLE raw_handle = nullptr;
    const LONG status = native_apis().nt_create_file(
        &raw_handle,
        FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | FILE_TRAVERSE | FILE_ADD_SUBDIRECTORY | SYNCHRONIZE,
        &oa, &iosb, nullptr, 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        FILE_OPEN,
        FILE_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT,
        nullptr, 0);
    if (status < 0) {
        result.failure = classify_ntstatus(status);
        result.native_code = static_cast<unsigned long>(status);
        result.error = ntstatus_message("NtCreateFile(root bootstrap)", status);
        return result;
    }
    unique_handle handle{raw_handle};

    // 5. Everything below is queried through the retained handle; any failure
    //    closes the handle (RAII) and fails closed.
    RootDirectory root;
    EntryAttributes attributes{};
    if (const auto r = query_attribute_tag_info(handle.get(), attributes); !r.ok) {
        result.failure = r.failure;
        result.native_code = r.native_code;
        result.error = r.error;
        return result;
    }

    std::string resolved;
    if (const auto r = resolved_final_path(handle.get(), resolved); !r.ok) {
        result.failure = r.failure;
        result.native_code = r.native_code;
        result.error = r.error;
        return result;
    }

    SourceIdentity identity{};
    if (const auto r = capture_identity(handle.get(), identity); !r.ok) {
        result.failure = r.failure;
        result.native_code = r.native_code;
        result.error = r.error;
        return result;
    }

    VolumeIdentity volume{};
    if (const auto r = volume_identity_for_path(std::wstring{wide.value}, volume); !r.ok) {
        result.failure = r.failure;
        result.native_code = r.native_code;
        result.error = r.error;
        return result;
    }

    // 6. Success: adopt the handle and publish the verified payload.
    root.handle_ = unique_handle{handle.release()};
    root.requested_path_utf8.assign(utf8_path);
    root.resolved_path_utf8 = std::move(resolved);
    root.attributes = attributes;
    root.identity = std::move(identity);
    root.volume = std::move(volume);

    result.ok = true;
    result.root = std::move(root);
    return result;
}

// ---------------------------------------------------------------------------
// Root-relative source opening + inspection.
// ---------------------------------------------------------------------------

SourceInspection inspect_source(HANDLE root_handle, std::string_view utf8_name) {
    SourceInspection out;

    if (root_handle == nullptr) {
        out.failure = NativeFailure::Other;
        out.error = "root handle is not valid";
        return out;
    }
    if (utf8_name.empty() || utf8_name == "." || utf8_name == ".." ||
        utf8_name.find_first_of("/\\:") != std::string_view::npos) {
        out.failure = NativeFailure::InvalidPath;
        out.error = "source name is not a single safe path component";
        return out;
    }
    const auto wide = to_utf16(utf8_name);
    if (!wide.ok) {
        out.failure = NativeFailure::InvalidPath;
        out.error = "source name is not valid UTF-8: " + wide.error;
        return out;
    }

    if (!native_apis().ok()) {
        out.failure = NativeFailure::Unsupported;
        out.error = "required ntdll functions are unavailable";
        return out;
    }

    std::wstring name = std::move(wide.value);
    UNICODE_STRING nt_name{};
    nt_name.Length = static_cast<USHORT>(name.size() * sizeof(WCHAR));
    nt_name.MaximumLength = nt_name.Length + sizeof(WCHAR);
    nt_name.Buffer = name.data();

    OBJECT_ATTRIBUTES oa{};
    oa.Length = sizeof(oa);
    oa.RootDirectory = root_handle; // anchored to the retained root object
    oa.Attributes = OBJ_CASE_INSENSITIVE;
    oa.ObjectName = &nt_name;
    oa.SecurityDescriptor = nullptr;
    oa.SecurityQualityOfService = nullptr;

    IO_STATUS_BLOCK iosb{};
    HANDLE raw_handle = nullptr;
    const LONG status = native_apis().nt_create_file(
        &raw_handle,
        DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        &oa, &iosb, nullptr, 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE, // no FILE_SHARE_DELETE: approved policy
        FILE_OPEN,
        FILE_NON_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT,
        nullptr, 0);
    if (status < 0) {
        out.failure = classify_ntstatus(status);
        out.native_code = static_cast<unsigned long>(status);
        out.error = ntstatus_message("NtCreateFile(source)", status);
        return out;
    }
    unique_handle handle{raw_handle}; // closed before return (Phase 3 scope)

    if (const auto r = query_attribute_tag_info(handle.get(), out.attributes); !r.ok) {
        out.failure = r.failure;
        out.native_code = r.native_code;
        out.error = r.error;
        return out;
    }

    FILE_BASIC_INFO basic{};
    if (!::GetFileInformationByHandleEx(handle.get(), FileBasicInfo, &basic, sizeof(basic))) {
        out.failure = NativeFailure::Other;
        out.native_code = ::GetLastError();
        out.error = win32_message("GetFileInformationByHandleEx(FileBasicInfo)", out.native_code);
        return out;
    }
    out.last_write_time = basic.LastWriteTime.QuadPart;

    FILE_STANDARD_INFO standard{};
    if (!::GetFileInformationByHandleEx(handle.get(), FileStandardInfo, &standard, sizeof(standard))) {
        out.failure = NativeFailure::Other;
        out.native_code = ::GetLastError();
        out.error = win32_message("GetFileInformationByHandleEx(FileStandardInfo)", out.native_code);
        return out;
    }
    out.size_bytes = standard.EndOfFile.QuadPart;

    if (const auto r = capture_identity(handle.get(), out.identity); !r.ok) {
        out.failure = r.failure;
        out.native_code = r.native_code;
        out.error = r.error;
        return out;
    }

    out.ok = true;
    return out;
}

bool identity_matches(const SourceIdentity& a, const SourceIdentity& b) {
    // A representation mismatch fails closed (never compare across variants).
    if (a.index() != b.index()) {
        return false;
    }
    if (std::holds_alternative<SourceIdentityEx>(a)) {
        const auto& ea = std::get<SourceIdentityEx>(a);
        const auto& eb = std::get<SourceIdentityEx>(b);
        return ea.volume_serial == eb.volume_serial &&
               std::memcmp(ea.file_id.Identifier, eb.file_id.Identifier,
                           sizeof(FILE_ID_128::Identifier)) == 0;
    }
    const auto& la = std::get<SourceIdentityLegacy>(a);
    const auto& lb = std::get<SourceIdentityLegacy>(b);
    return la.volume_serial == lb.volume_serial &&
           la.file_index_high == lb.file_index_high &&
           la.file_index_low == lb.file_index_low;
}

CategoryDirInspection inspect_category_directory(HANDLE root_handle,
                                                 std::string_view utf8_name) {
    CategoryDirInspection out;

    if (root_handle == nullptr || utf8_name.empty() ||
        utf8_name.find_first_of("/\\:") != std::string_view::npos ||
        utf8_name == "." || utf8_name == "..") {
        out.status = CategoryDirStatus::Uninspectable;
        out.detail = "category name is not a single safe path component";
        return out;
    }
    const auto wide = to_utf16(utf8_name);
    if (!wide.ok) {
        out.status = CategoryDirStatus::Uninspectable;
        out.detail = "category name is not valid UTF-8: " + wide.error;
        return out;
    }
    if (!native_apis().ok()) {
        out.status = CategoryDirStatus::Uninspectable;
        out.detail = "required ntdll functions are unavailable";
        return out;
    }

    std::wstring name = std::move(wide.value);
    UNICODE_STRING nt_name{};
    nt_name.Length = static_cast<USHORT>(name.size() * sizeof(WCHAR));
    nt_name.MaximumLength = nt_name.Length + sizeof(WCHAR);
    nt_name.Buffer = name.data();

    OBJECT_ATTRIBUTES oa{};
    oa.Length = sizeof(oa);
    oa.RootDirectory = root_handle; // anchored to the retained root object
    oa.Attributes = OBJ_CASE_INSENSITIVE;
    oa.ObjectName = &nt_name;
    oa.SecurityDescriptor = nullptr;
    oa.SecurityQualityOfService = nullptr;

    IO_STATUS_BLOCK iosb{};
    HANDLE raw_handle = nullptr;
    const LONG status = native_apis().nt_create_file(
        &raw_handle,
        FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE, // inspection only
        &oa, &iosb, nullptr, 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE, // no FILE_SHARE_DELETE: approved policy
        FILE_OPEN,                          // never creates anything
        FILE_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT,
        nullptr, 0);
    if (status < 0) {
        out.native_code = static_cast<unsigned long>(status);
        if (status == STATUS_OBJECT_NAME_NOT_FOUND || status == STATUS_OBJECT_PATH_NOT_FOUND) {
            out.status = CategoryDirStatus::Missing;
            out.detail = "category directory does not exist";
            return out;
        }
        if (status == STATUS_NOT_A_DIRECTORY) {
            out.status = CategoryDirStatus::Unsafe;
            out.detail = "a regular file with the category name already exists";
            return out;
        }
        out.status = CategoryDirStatus::Uninspectable;
        out.detail = ntstatus_message("NtCreateFile(category)", status);
        return out;
    }
    unique_handle handle{raw_handle}; // closed before return

    EntryAttributes attributes{};
    if (const auto r = query_attribute_tag_info(handle.get(), attributes); !r.ok) {
        out.status = CategoryDirStatus::Uninspectable;
        out.native_code = r.native_code;
        out.detail = r.error;
        return out;
    }
    if (attributes.reparse_point()) {
        out.status = CategoryDirStatus::Unsafe;
        out.detail = "category directory is a reparse point";
        return out;
    }
    if (attributes.hidden()) {
        out.status = CategoryDirStatus::Unsafe;
        out.detail = "category directory is hidden";
        return out;
    }
    if (attributes.system()) {
        out.status = CategoryDirStatus::Unsafe;
        out.detail = "category directory is a system directory";
        return out;
    }
    if (!attributes.directory()) {
        out.status = CategoryDirStatus::Unsafe;
        out.detail = "category name is not a directory";
        return out;
    }

    // Snapshot every immediate name: hidden/system/reparse/directories all
    // occupy filenames and therefore all count as collisions.
    NativeResult enumeration{true, NativeFailure::Other, {}, 0};
    enumeration = enumerate_immediate_children(handle.get(), 64 * 1024,
        [&out](const RawDirEntry& entry) {
            // Defensive: "." and ".." can never collide with a real filename.
            if (entry.name == L"." || entry.name == L"..") {
                return;
            }
            const auto narrow = to_utf8(entry.name);
            if (narrow.ok) {
                out.existing_names.push_back(narrow.value);
            }
        });
    if (!enumeration.ok) {
        out.status = CategoryDirStatus::Uninspectable;
        out.detail = "category enumeration failed: " + enumeration.error;
        return out;
    }

    out.status = CategoryDirStatus::Safe;
    return out;
}

} // namespace folderpilot::platform
