// FolderPilot - platform storage primitives implementation.
//
// Self-contained NT runtime bindings, mirroring platform/native.cpp's
// established pattern (everything resolved at runtime from ntdll.dll; all
// structures from <winternl.h>/<ntstatus.h>; no prototype conflicts). The
// policy decisions (which components, which gates, which names, ordering)
// live in the storage adapter (lib/folderpilot/storage_adapter.*).

#include "storage.hpp"

#ifndef WIN32_NO_STATUS
#define WIN32_NO_STATUS
#endif
#include <Windows.h>
#undef WIN32_NO_STATUS
#include <winternl.h>
#include <ntstatus.h>
#include <aclapi.h>
#include <shlobj.h>

#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

#include "text.hpp"

namespace folderpilot::platform {

namespace {

// ---- runtime-resolved ntdll functions (same pattern as native.cpp) --------

using NtCreateFile_fn = LONG (NTAPI*)(
    PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
    PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
using RtlDosPathNameToNtPathName_U_fn =
    BOOLEAN (NTAPI*)(PCWSTR, PUNICODE_STRING, PWSTR*, PVOID);
using RtlFreeUnicodeString_fn = void (NTAPI*)(PUNICODE_STRING);

struct StorageNtApis {
    NtCreateFile_fn nt_create_file = nullptr;
    RtlDosPathNameToNtPathName_U_fn dos_to_nt = nullptr;
    RtlFreeUnicodeString_fn free_unicode_string = nullptr;
    bool ok() const {
        return nt_create_file != nullptr && dos_to_nt != nullptr &&
               free_unicode_string != nullptr;
    }
};

const StorageNtApis& storage_nt_apis() {
    static const StorageNtApis apis = [] {
        StorageNtApis resolved{};
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

std::string ntstatus_message(const char* what, LONG status) {
    return std::string(what) + " failed with NTSTATUS 0x" +
           ([status] {
               char buf[16]{};
               std::snprintf(buf, sizeof(buf), "%08lX",
                             static_cast<unsigned long>(status));
               return std::string(buf);
           })();
}

std::string win32_message(const char* what, DWORD code) {
    return std::string(what) + " failed with Win32 error " + std::to_string(code);
}

// Handle queries (same payloads as native.cpp's internal helpers).
NativeResult query_attribute_tag_info(HANDLE handle, EntryAttributes& out) {
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (!::GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info, sizeof(info))) {
        return {false, NativeFailure::Other,
                win32_message("GetFileInformationByHandleEx(FileAttributeTagInfo)",
                              ::GetLastError()),
                ::GetLastError()};
    }
    out = EntryAttributes{true, info.FileAttributes, info.ReparseTag};
    return {true, NativeFailure::Other, {}, 0};
}

// A leaf name must be exactly one safe path component.
bool is_safe_leaf_name(std::string_view utf8_name) {
    return !utf8_name.empty() && utf8_name != "." && utf8_name != ".." &&
           utf8_name.find_first_of("/\\:") == std::string_view::npos;
}

// Identity capture through the opened handle (FileIdInfo preferred, legacy
// GetFileInformationByHandle fallback - strictly separate representations,
// mirroring native.cpp).
NativeResult capture_storage_identity(HANDLE handle, SourceIdentity& out) {
    FILE_ID_INFO info{};
    if (::GetFileInformationByHandleEx(handle, FileIdInfo, &info, sizeof(info))) {
        out = SourceIdentityEx{info.VolumeSerialNumber, info.FileId};
        return {true, NativeFailure::Other, {}, 0};
    }
    const DWORD err = ::GetLastError();
    if (err == ERROR_INVALID_PARAMETER || err == ERROR_NOT_SUPPORTED) {
        BY_HANDLE_FILE_INFORMATION legacy{};
        if (::GetFileInformationByHandle(handle, &legacy)) {
            out = SourceIdentityLegacy{legacy.dwVolumeSerialNumber,
                                       legacy.nFileIndexHigh, legacy.nFileIndexLow};
            return {true, NativeFailure::Other, {}, 0};
        }
        return {false, NativeFailure::Unsupported,
                win32_message("GetFileInformationByHandle", ::GetLastError()),
                ::GetLastError()};
    }
    return {false, NativeFailure::Other,
            win32_message("GetFileInformationByHandleEx(FileIdInfo)", err), err};
}

} // namespace

// ---------------------------------------------------------------------------
// Known-folder bootstrap.
// ---------------------------------------------------------------------------

KnownFolderResult local_app_data_path() {
    KnownFolderResult out;
    PWSTR raw = nullptr;
    const HRESULT hr =
        ::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw);
    if (hr != S_OK || raw == nullptr) {
        out.error = "SHGetKnownFolderPath(FOLDERID_LocalAppData) failed with HRESULT 0x" +
                    std::to_string(static_cast<unsigned long>(hr));
        return out;
    }
    const auto wide_free = std::unique_ptr<WCHAR, void (*)(PWSTR)>(
        raw, [](PWSTR p) { ::CoTaskMemFree(p); });
    const auto converted = to_utf8(wide_free.get());
    if (!converted.ok) {
        out.error = "known-folder path is not valid UTF-8: " + converted.error;
        return out;
    }
    // Normalize: strip ALL trailing separators (idempotent).
    std::string path = converted.value;
    while (path.size() > 1 && (path.back() == '\\' || path.back() == '/')) {
        path.pop_back();
    }
    out.ok = true;
    out.path_utf8 = std::move(path);
    return out;
}

// ---------------------------------------------------------------------------
// Storage-tree opens.
// ---------------------------------------------------------------------------

StorageBaseOpen open_storage_base(std::string_view utf8_path) {
    StorageBaseOpen result;

    const auto& apis = storage_nt_apis();
    if (!apis.ok()) {
        result.failure = NativeFailure::Unsupported;
        result.error = "required ntdll functions (NtCreateFile, "
                       "RtlDosPathNameToNtPathName_U) are unavailable";
        return result;
    }

    const auto wide = to_utf16(utf8_path);
    if (!wide.ok) {
        result.failure = NativeFailure::InvalidPath;
        result.error = "storage base path is not valid UTF-8: " + wide.error;
        return result;
    }
    if (const auto bad_form = classify_dos_path(wide.value)) {
        result.failure = *bad_form;
        result.error = "unsupported storage base path form";
        return result;
    }

    UNICODE_STRING nt_name{};
    if (apis.dos_to_nt(wide.value.c_str(), &nt_name, nullptr, nullptr) == 0) {
        result.failure = NativeFailure::Other;
        result.error = "could not convert the storage base path to NT form";
        return result;
    }
    struct NtNameGuard {
        UNICODE_STRING name;
        RtlFreeUnicodeString_fn free_fn;
        ~NtNameGuard() { free_fn(&name); }
    } nt_guard{nt_name, apis.free_unicode_string};

    OBJECT_ATTRIBUTES oa{};
    oa.Length = sizeof(oa);
    oa.RootDirectory = nullptr;
    oa.Attributes = OBJ_CASE_INSENSITIVE;
    oa.ObjectName = &nt_name;
    oa.SecurityDescriptor = nullptr;
    oa.SecurityQualityOfService = nullptr;

    IO_STATUS_BLOCK iosb{};
    HANDLE raw_handle = nullptr;
    // FILE_OPEN only: the base (the %LOCALAPPDATA% analogue) is never created
    // by FolderPilot. The reparse point itself is opened and reported.
    const LONG status = apis.nt_create_file(
        &raw_handle,
        FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | FILE_ADD_FILE |
            FILE_ADD_SUBDIRECTORY | SYNCHRONIZE,
        &oa, &iosb, nullptr, 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        FILE_OPEN,
        FILE_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT,
        nullptr, 0);
    if (status < 0) {
        result.failure = classify_ntstatus(status);
        result.native_code = static_cast<unsigned long>(status);
        result.error = ntstatus_message("NtCreateFile(storage base)", status);
        return result;
    }
    unique_handle handle{raw_handle};

    if (const auto r = query_attribute_tag_info(handle.get(), result.attributes); !r.ok) {
        result.failure = r.failure;
        result.native_code = r.native_code;
        result.error = r.error;
        return result;
    }

    result.ok = true;
    result.handle = unique_handle{handle.release()};
    return result;
}

StorageComponentOpen open_storage_component(HANDLE parent, std::string_view utf8_name) {
    StorageComponentOpen out;

    if (parent == nullptr || !is_safe_leaf_name(utf8_name)) {
        out.failure = NativeFailure::InvalidPath;
        out.error = "storage component name is not a single safe path component";
        return out;
    }
    const auto wide = to_utf16(utf8_name);
    if (!wide.ok) {
        out.failure = NativeFailure::InvalidPath;
        out.error = "storage component name is not valid UTF-8: " + wide.error;
        return out;
    }
    const auto& apis = storage_nt_apis();
    if (!apis.ok()) {
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
    oa.RootDirectory = parent;      // anchored to the verified parent handle
    oa.Attributes = OBJ_CASE_INSENSITIVE;
    oa.ObjectName = &nt_name;
    oa.SecurityDescriptor = nullptr;
    oa.SecurityQualityOfService = nullptr;

    IO_STATUS_BLOCK iosb{};
    HANDLE raw_handle = nullptr;
    // Open-or-create a directory component of the storage chain. The reparse
    // point itself is opened (never followed) and reported through the
    // attribute gate; the hidden/system gate is the adapter policy.
    // READ_CONTROL enables the ratified [G4] ACL validation of the journals
    // directory through this handle.
    const LONG status = apis.nt_create_file(
        &raw_handle,
        FILE_READ_ATTRIBUTES | READ_CONTROL | FILE_LIST_DIRECTORY | FILE_ADD_FILE |
            FILE_ADD_SUBDIRECTORY | SYNCHRONIZE,
        &oa, &iosb, nullptr,
        FILE_ATTRIBUTE_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        FILE_OPEN_IF,
        FILE_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT,
        nullptr, 0);
    if (status < 0) {
        out.native_code = static_cast<unsigned long>(status);
        out.failure = classify_ntstatus(status);
        out.error = ntstatus_message("NtCreateFile(storage component)", status);
        return out;
    }
    unique_handle handle{raw_handle};
    out.created = iosb.Information == FILE_CREATED;

    if (const auto r = query_attribute_tag_info(handle.get(), out.attributes); !r.ok) {
        out.failure = r.failure;
        out.native_code = r.native_code;
        out.error = r.error;
        return out;
    }

    out.ok = true;
    out.handle = unique_handle{handle.release()};
    return out;
}

StorageLeafOpen open_storage_leaf(HANDLE parent, std::string_view utf8_name,
                                  unsigned long access, unsigned long share,
                                  unsigned long disposition) {
    StorageLeafOpen out;

    if (parent == nullptr || !is_safe_leaf_name(utf8_name)) {
        out.failure = NativeFailure::InvalidPath;
        out.error = "storage leaf name is not a single safe path component";
        return out;
    }
    const auto wide = to_utf16(utf8_name);
    if (!wide.ok) {
        out.failure = NativeFailure::InvalidPath;
        out.error = "storage leaf name is not valid UTF-8: " + wide.error;
        return out;
    }
    if (disposition != FILE_OPEN && disposition != FILE_CREATE &&
        disposition != FILE_OPEN_IF) {
        out.failure = NativeFailure::InvalidPath;
        out.error = "leaf disposition is outside the ratified leaf-open protocol";
        return out;
    }
    const auto& apis = storage_nt_apis();
    if (!apis.ok()) {
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
    oa.RootDirectory = parent;      // anchored to the verified parent handle
    oa.Attributes = OBJ_CASE_INSENSITIVE;
    oa.ObjectName = &nt_name;
    oa.SecurityDescriptor = nullptr;
    oa.SecurityQualityOfService = nullptr;

    IO_STATUS_BLOCK iosb{};
    HANDLE raw_handle = nullptr;
    // File leaf. FILE_OPEN_REPARSE_POINT is mandatory: an existing reparse
    // point is opened itself (never followed) and rejected by the post-open
    // attribute gate. SYNCHRONIZE access pairs with synchronous I/O.
    const LONG status = apis.nt_create_file(
        &raw_handle, access, &oa, &iosb, nullptr, 0, share, disposition,
        FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT, nullptr, 0);
    if (status < 0) {
        out.native_code = static_cast<unsigned long>(status);
        out.failure = classify_ntstatus(status);
        out.error = ntstatus_message("NtCreateFile(storage leaf)", status);
        return out;
    }
    unique_handle handle{raw_handle};
    out.created = iosb.Information == FILE_CREATED;

    if (const auto r = query_attribute_tag_info(handle.get(), out.attributes); !r.ok) {
        out.failure = r.failure;
        out.native_code = r.native_code;
        out.error = r.error;
        return out;
    }
    if (const auto r = capture_storage_identity(handle.get(), out.identity); !r.ok) {
        out.failure = r.failure;
        out.native_code = r.native_code;
        out.error = r.error;
        return out;
    }

    out.ok = true;
    out.handle = unique_handle{handle.release()};
    return out;
}

NativeResult flush_storage_file(HANDLE handle) {
    if (handle == nullptr || !::FlushFileBuffers(handle)) {
        return {false, NativeFailure::Other,
                win32_message("FlushFileBuffers", ::GetLastError()), ::GetLastError()};
    }
    return {true, NativeFailure::Other, {}, 0};
}

NativeResult write_storage_file(HANDLE handle, const unsigned char* bytes,
                                std::size_t byte_count) {
    if (handle == nullptr) {
        return {false, NativeFailure::InvalidPath,
                "write requires an opened leaf handle", 0};
    }
    if (byte_count == 0) {
        return {true, NativeFailure::Other, {}, 0};
    }
    if (byte_count > static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())) {
        return {false, NativeFailure::Other,
                "single write exceeds the Win32 DWORD byte bound", 0};
    }
    const DWORD to_write = static_cast<DWORD>(byte_count);
    DWORD written = 0;
    if (!::WriteFile(handle, bytes, to_write, &written, nullptr)) {
        const DWORD code = ::GetLastError();
        return {false, NativeFailure::Other,
                win32_message("WriteFile", code), code};
    }
    if (written != to_write) {
        // A short write without a failure code: local files normally write
        // whole buffers, so this is an abnormal durability event - reported,
        // never retried or silently accepted.
        return {false, NativeFailure::Other,
                "WriteFile wrote " + std::to_string(written) + " of " +
                    std::to_string(to_write) + " bytes without an error code",
                0};
    }
    return {true, NativeFailure::Other, {}, 0};
}

NativeResult storage_file_size(HANDLE handle, long long& out_bytes) {
    LARGE_INTEGER size{};
    if (handle == nullptr || !::GetFileSizeEx(handle, &size)) {
        const DWORD code = handle == nullptr ? 0 : ::GetLastError();
        return {false, NativeFailure::Other,
                win32_message("GetFileSizeEx", code), code};
    }
    out_bytes = size.QuadPart;
    return {true, NativeFailure::Other, {}, 0};
}

// ---------------------------------------------------------------------------
// ACL validation [G4, ratified Decision A].
// ---------------------------------------------------------------------------

DaclInspection inspect_dacl_presence(PSECURITY_DESCRIPTOR descriptor) {
    DaclInspection out;
    if (descriptor == nullptr) {
        out.failure = NativeFailure::Other;
        out.error = "DACL inspection requires a security descriptor";
        return out;
    }
    BOOL present = FALSE;
    PACL dacl = nullptr;
    BOOL defaulted = FALSE;
    if (!::GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted)) {
        out.failure = NativeFailure::Other;
        out.native_code = ::GetLastError();
        out.error = win32_message("GetSecurityDescriptorDacl", out.native_code);
        return out;
    }
    out.ok = true;
    // When NO DACL is present the pointer is never inspected: its value is
    // meaningless for an absent DACL (ratified Decision A classification).
    if (!present) {
        out.presence = DaclPresence::Absent;
        return out;
    }
    if (dacl == nullptr) {
        out.presence = DaclPresence::NullDacl;
        return out;
    }
    out.presence = DaclPresence::Present;
    out.dacl = dacl;
    return out;
}

AclValidationResult validate_current_user_access(HANDLE handle,
                                                 unsigned long generic_access) {
    AclValidationResult out;

    if (handle == nullptr) {
        out.failure = NativeFailure::InvalidPath;
        out.error = "ACL validation requires an opened handle";
        return out;
    }

    // The current process token: AccessCheck evaluates the token's user AND
    // group SIDs against the DACL, so no separate SID extraction is needed.
    // AccessCheck requires an IMPERSONATION-level token (it fails with
    // ERROR_BAD_IMPERSONATION_LEVEL against a primary token), so the
    // process's primary token is duplicated to a Query-only impersonation
    // token - the duplication grants no privileges beyond the source token.
    HANDLE raw_token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(),
                            TOKEN_DUPLICATE | TOKEN_QUERY, &raw_token)) {
        out.failure = NativeFailure::Other;
        out.native_code = ::GetLastError();
        out.error = win32_message("OpenProcessToken", out.native_code);
        return out;
    }
    unique_handle process_token{raw_token};

    HANDLE raw_duplicate = nullptr;
    if (!::DuplicateTokenEx(process_token.get(), TOKEN_QUERY, nullptr,
                            SecurityImpersonation, TokenImpersonation,
                            &raw_duplicate)) {
        out.failure = NativeFailure::Other;
        out.native_code = ::GetLastError();
        out.error = win32_message("DuplicateTokenEx", out.native_code);
        return out;
    }
    unique_handle token{raw_duplicate};

    // The opened object's security descriptor (owner AND group information
    // included: AccessCheck evaluates owner-rights ACEs and rejects a
    // group-less descriptor with ERROR_INVALID_SECURITY_DESCR - runtime
    // evidence 2026-11-18: after a SetNamedSecurityInfoW DACL write, the
    // descriptor returned without GROUP_SECURITY_INFORMATION lacks the group
    // and every AccessCheck on it fails). Validation only - the descriptor
    // is freed, never written back.
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (::GetSecurityInfo(handle, SE_FILE_OBJECT,
                          DACL_SECURITY_INFORMATION | OWNER_SECURITY_INFORMATION |
                              GROUP_SECURITY_INFORMATION,
                          nullptr, nullptr, nullptr, nullptr,
                          &descriptor) != ERROR_SUCCESS ||
        descriptor == nullptr) {
        out.failure = NativeFailure::Other;
        out.native_code = ::GetLastError();
        out.error = win32_message("GetSecurityInfo(DACL)", out.native_code);
        return out;
    }
    const auto descriptor_free = std::unique_ptr<void, void (*)(void*)>(
        descriptor, [](void* p) { ::LocalFree(p); });

    // Ratified Decision A: classify DACL presence EXACTLY before any access
    // evaluation. An inspection failure fails closed; when no DACL is
    // present the pointer is never inspected. Absent and NULL DACLs refuse
    // outright (either would otherwise grant everyone); an EMPTY DACL
    // (present, zero ACEs) falls through to AccessCheck, which denies it.
    const auto inspection = inspect_dacl_presence(descriptor);
    if (!inspection.ok) {
        out.failure = inspection.failure;
        out.native_code = inspection.native_code;
        out.error = "security descriptor DACL inspection failed: " + inspection.error;
        return out;
    }
    if (inspection.presence == DaclPresence::Absent) {
        out.ok = true;
        out.granted = false;
        out.error = "the security descriptor has NO DACL (absent DACL), which "
                    "cannot be validated; refused per ratified Decision A";
        return out;
    }
    if (inspection.presence == DaclPresence::NullDacl) {
        out.ok = true;
        out.granted = false;
        out.error = "the security descriptor has a NULL DACL, which grants "
                    "everyone; refused per ratified Decision A";
        return out;
    }

    // AccessCheck rejects a descriptor whose control word carries
    // SE_DACL_AUTO_INHERITED with ERROR_INVALID_SECURITY_DESCR even when the
    // descriptor is structurally valid (runtime evidence 2026-11-18: DACLs
    // written through SetNamedSecurityInfoW carry the hint, and
    // SetSecurityDescriptorControl cannot clear that specific bit - it
    // manages SE_DACL_PROTECTED and SE_DACL_AUTO_INHERIT_REQ only). The
    // validator therefore evaluates the SAME owner/group/DACL content
    // rebuilt into a fresh ABSOLUTE working descriptor: the hint never
    // enters the copy, and the evaluated access inputs are identical. This
    // is a private copy - the object itself is never touched.
    SECURITY_DESCRIPTOR working{};
    if (!::InitializeSecurityDescriptor(&working, SECURITY_DESCRIPTOR_REVISION)) {
        out.failure = NativeFailure::Other;
        out.native_code = ::GetLastError();
        out.error = win32_message("InitializeSecurityDescriptor", out.native_code);
        return out;
    }
    BOOL owner_defaulted = FALSE;
    PSID owner = nullptr;
    if (!::GetSecurityDescriptorOwner(descriptor, &owner, &owner_defaulted) ||
        !::SetSecurityDescriptorOwner(&working, owner, owner_defaulted)) {
        out.failure = NativeFailure::Other;
        out.native_code = ::GetLastError();
        out.error = win32_message("security descriptor owner transfer", out.native_code);
        return out;
    }
    BOOL group_defaulted = FALSE;
    PSID group = nullptr;
    if (!::GetSecurityDescriptorGroup(descriptor, &group, &group_defaulted) ||
        !::SetSecurityDescriptorGroup(&working, group, group_defaulted)) {
        out.failure = NativeFailure::Other;
        out.native_code = ::GetLastError();
        out.error = win32_message("security descriptor group transfer", out.native_code);
        return out;
    }
    BOOL dacl_present = FALSE;
    BOOL dacl_defaulted = FALSE;
    PACL dacl = nullptr;
    if (!::GetSecurityDescriptorDacl(descriptor, &dacl_present, &dacl,
                                     &dacl_defaulted) ||
        !::SetSecurityDescriptorDacl(&working, TRUE, dacl, dacl_defaulted)) {
        out.failure = NativeFailure::Other;
        out.native_code = ::GetLastError();
        out.error = win32_message("security descriptor DACL transfer", out.native_code);
        return out;
    }

    // Generic -> specific mapping through the standard file mapping.
    GENERIC_MAPPING mapping{FILE_GENERIC_READ, FILE_GENERIC_WRITE,
                            FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
    unsigned long desired = generic_access;
    ::MapGenericMask(&desired, &mapping);

    ACCESS_MASK granted = 0;
    BOOL access_status = FALSE;
    PRIVILEGE_SET privileges{};
    DWORD privileges_size = sizeof(privileges);
    const BOOL check = ::AccessCheck(&working, token.get(), desired, &mapping,
                                     &privileges, &privileges_size, &granted,
                                     &access_status);
    if (!check) {
        out.failure = NativeFailure::Other;
        out.native_code = ::GetLastError();
        out.error = win32_message("AccessCheck", out.native_code);
        return out;
    }

    out.ok = true;
    out.granted = access_status == TRUE && (granted & desired) == desired;
    if (!out.granted) {
        out.error = "the current user does not hold the required access on the "
                    "journals directory";
    }
    return out;
}

} // namespace folderpilot::platform
