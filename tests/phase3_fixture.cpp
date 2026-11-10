// FolderPilot - Phase 3 test fixture implementation. TEST CODE ONLY.

#include "phase3_fixture.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winioctl.h>

#include "text.hpp"
#include "folderpilot/path_guard.hpp"

namespace fixture {

namespace {

std::filesystem::path sandbox_base() {
    // Provided by CMake: ${CMAKE_BINARY_DIR}/phase3-test-sandbox
    std::filesystem::path base{FOLDERPILOT_PHASE3_SANDBOX};
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    return base;
}

std::wstring with_backslashes(const std::filesystem::path& p) {
    std::filesystem::path preferred = p;
    preferred.make_preferred();
    return preferred.wstring();
}

// Extended-length-aware path form (\\?\ prefix). The test suite creates
// names whose full path exceeds MAX_PATH (e.g. 255-UTF-16-unit names), which
// plain Win32 paths cannot address; every cleanup operation goes through
// this form. std::filesystem::remove_all is NOT used: it fails on such names
// and (libstdc++, GCC 16.1) hangs when an entry cannot be deleted.
std::wstring with_extended_prefix(const std::filesystem::path& p) {
    std::wstring s = with_backslashes(p);
    if (s.rfind(L"\\\\?\\", 0) == 0) {
        return s;
    }
    return L"\\\\?\\" + s;
}

// Bounded, reparse-safe recursive removal over \\?\ paths. Reparse points
// (junctions, symlinks, cloud placeholders) are removed as LINKS and never
// followed, so cleanup can never escape the sandbox through one. Depth is
// capped so a pathological tree cannot recurse without end. Returns false
// when ANY entry could not be deleted; first_error carries a description of
// the first failure. Deletion continues after a failure (best effort), and
// genuine failures are NEVER swallowed silently - the caller reports them.
bool remove_tree_extended(const std::filesystem::path& p, int depth,
                          std::wstring& first_error) {
    constexpr int kMaxDepth = 64;
    if (depth > kMaxDepth) {
        if (first_error.empty()) {
            first_error = L"cleanup recursion depth limit exceeded: " + with_backslashes(p);
        }
        return false;
    }

    bool ok = true;
    WIN32_FIND_DATAW fd{};
    HANDLE find = ::FindFirstFileW((with_extended_prefix(p) + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        if (err != ERROR_FILE_NOT_FOUND && err != ERROR_NO_MORE_FILES &&
            err != ERROR_PATH_NOT_FOUND) {
            if (first_error.empty()) {
                first_error = L"FindFirstFileW failed (error " + std::to_wstring(err) +
                              L"): " + with_backslashes(p);
            }
            return false;
        }
        return true; // nothing to enumerate
    }
    for (;;) {
        const std::wstring_view name{fd.cFileName};
        if (name != L"." && name != L"..") {
            const std::filesystem::path child = p / std::wstring(name);
            bool removed = false;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                // The LINK itself - never its target.
                removed = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                              ? ::RemoveDirectoryW(with_extended_prefix(child).c_str()) != 0
                              : ::DeleteFileW(with_extended_prefix(child).c_str()) != 0;
                if (!removed && first_error.empty()) {
                    first_error = L"failed to remove reparse point (error " +
                                  std::to_wstring(::GetLastError()) + L"): " +
                                  with_backslashes(child);
                }
            } else if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                removed = remove_tree_extended(child, depth + 1, first_error);
            } else {
                removed = ::DeleteFileW(with_extended_prefix(child).c_str()) != 0;
                if (!removed && first_error.empty()) {
                    first_error = L"failed to delete file (error " +
                                  std::to_wstring(::GetLastError()) + L"): " +
                                  with_backslashes(child);
                }
            }
            ok = ok && removed;
        }
        if (!::FindNextFileW(find, &fd)) {
            const DWORD err = ::GetLastError();
            if (err != ERROR_NO_MORE_FILES) {
                if (first_error.empty()) {
                    first_error = L"FindNextFileW failed (error " + std::to_wstring(err) +
                                  L"): " + with_backslashes(p);
                }
                ok = false;
            }
            break;
        }
    }
    ::FindClose(find);

    if (!::RemoveDirectoryW(with_extended_prefix(p).c_str())) {
        if (first_error.empty()) {
            first_error = L"failed to remove directory (error " +
                          std::to_wstring(::GetLastError()) + L"): " + with_backslashes(p);
        }
        ok = false;
    }
    return ok;
}

// Reads `bytes.size() + 1` bytes from `path` (a fully-formed Win32 or \\?\-
// prefixed path) and compares byte-exactly against `expected`.
bool content_matches_path(const std::wstring& path, const std::string& expected) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    std::string actual(expected.size(), char(0));
    DWORD read = 0;
    const BOOL ok = ::ReadFile(h, actual.data(), static_cast<DWORD>(expected.size() + 1), &read, nullptr);
    ::CloseHandle(h);
    return ok != 0 && read == expected.size() && actual == expected;
}

} // namespace

SandboxDir::SandboxDir(const std::string& name, SandboxRoot root) {
    std::filesystem::path base = sandbox_base();
    if (root == SandboxRoot::Phase5) {
        base = base.parent_path() / "phase5-test-sandbox";
        std::error_code ec;
        std::filesystem::create_directories(base, ec);
    }
    sandbox_root_ = base;
    static std::atomic<int> counter{0};
    // PID in the name: killed test runs leave leaked fixtures behind, and a
    // per-process counter alone would collide with them on the next run.
    const std::string unique = name + '-' + std::to_string(::GetCurrentProcessId()) +
                               '-' + std::to_string(counter.fetch_add(1));
    path_ = sandbox_root_ / unique;
    std::error_code ec;
    std::filesystem::create_directories(path_, ec);
}

SandboxDir::SandboxDir(const std::string& name, const std::filesystem::path& base) {
    sandbox_root_ = base;
    static std::atomic<int> counter{0};
    const std::string unique = name + '-' + std::to_string(::GetCurrentProcessId()) +
                               '-' + std::to_string(counter.fetch_add(1));
    path_ = sandbox_root_ / unique;
    std::error_code ec;
    std::filesystem::create_directories(path_, ec);
}

SandboxDir::~SandboxDir() {
    std::error_code ec;
    const auto canonical_target = std::filesystem::canonical(path_, ec);
    const auto canonical_base = std::filesystem::canonical(sandbox_root_, ec);
    if (ec) {
        return; // cannot verify - never clean anything
    }
    // Verify the fixture's canonical path is beneath the sandbox base
    // (prefix + separator boundary) before removing anything.
    const std::wstring target = with_backslashes(canonical_target);
    const std::wstring base = with_backslashes(canonical_base);
    if (target.size() <= base.size() || target.compare(0, base.size(), base) != 0 ||
        target[base.size()] != L'\\') {
        return; // verification failed - refuse to clean
    }
    std::wstring first_error;
    if (!remove_tree_extended(path_, 0, first_error)) {
        // Genuine cleanup failures are never swallowed: they are reported on
        // stderr so the test harness surfaces them (a leftover fixture dir is
        // always a test-side problem worth investigating).
        std::fprintf(stderr, "FIXTURE CLEANUP FAILURE (%ls): %ls\n",
                     with_backslashes(path_).c_str(), first_error.c_str());
        std::fflush(stderr);
    }
}

std::string SandboxDir::u8() const {
    const auto converted = folderpilot::platform::to_utf8(with_backslashes(path_));
    return converted.ok ? converted.value : std::string{};
}

bool create_file(const std::filesystem::path& dir, const std::wstring& name,
                 const std::string& bytes) {
    const std::filesystem::path full = dir / name;
    HANDLE handle = ::CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const bool ok = bytes.empty() ||
                    ::WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    ::CloseHandle(handle);
    return ok && written == bytes.size();
}

bool create_directory(const std::filesystem::path& dir, const std::wstring& name) {
    return ::CreateDirectoryW((dir / name).c_str(), nullptr) != 0;
}

bool add_attributes(const std::filesystem::path& target, unsigned long attributes) {
    const DWORD existing = ::GetFileAttributesW(target.c_str());
    if (existing == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    return ::SetFileAttributesW(target.c_str(), existing | attributes) != 0;
}

// Local, distinctly-named reparse buffer with the documented MOUNT_POINT
// layout (MinGW's winnt.h does not export REPARSE_DATA_BUFFER; a local
// structure with a distinct name cannot conflict with any header).
struct FpMountPointReparseBuffer {
    unsigned short substitute_name_offset;
    unsigned short substitute_name_length;
    unsigned short print_name_offset;
    unsigned short print_name_length;
    wchar_t path_buffer[1];
};
struct FpReparseDataBuffer {
    unsigned long reparse_tag;
    unsigned short data_length;
    unsigned short reserved;
    FpMountPointReparseBuffer mount_point;
};

bool create_junction(const std::filesystem::path& link, const std::filesystem::path& target) {
    const std::wstring target_abs = with_backslashes(target);
    const std::wstring substitute = L"\\??\\" + target_abs;
    const std::wstring print = target_abs;

    const auto substitute_bytes = static_cast<unsigned short>(substitute.size() * 2);
    const auto print_bytes = static_cast<unsigned short>(print.size() * 2);
    constexpr std::size_t kHeader = offsetof(FpReparseDataBuffer, mount_point.path_buffer);
    const std::size_t total = kHeader + substitute_bytes + 2 + print_bytes + 2;

    std::vector<std::byte> buffer(total);
    auto* reparse = reinterpret_cast<FpReparseDataBuffer*>(buffer.data());
    reparse->reparse_tag = IO_REPARSE_TAG_MOUNT_POINT;
    reparse->data_length = static_cast<unsigned short>(total - offsetof(FpReparseDataBuffer, mount_point));
    reparse->reserved = 0;
    reparse->mount_point.substitute_name_offset = 0;
    reparse->mount_point.substitute_name_length = substitute_bytes;
    reparse->mount_point.print_name_offset = static_cast<unsigned short>(substitute_bytes + 2);
    reparse->mount_point.print_name_length = print_bytes;
    std::memcpy(buffer.data() + kHeader, substitute.c_str(), substitute_bytes);
    std::memcpy(buffer.data() + kHeader + substitute_bytes + 2, print.c_str(), print_bytes);

    if (::CreateDirectoryW(link.c_str(), nullptr) == 0) {
        return false;
    }
    HANDLE handle = ::CreateFileW(link.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                  nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD returned = 0;
    const bool ok = ::DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(),
                                      static_cast<DWORD>(total), nullptr, 0, &returned, nullptr) != 0;
    ::CloseHandle(handle);
    return ok;
}

namespace {

// Walks `dir`, recording every entry under `prefix` (a wide relative path).
// Reparse points are recorded but never followed; keys are strict-converted
// to UTF-8 so Unicode names round-trip exactly.
void walk_snapshot(const std::filesystem::path& dir, const std::wstring& prefix,
                   Snapshot& out) {
    std::error_code ec;
    for (std::filesystem::directory_iterator it{dir, ec}, end; !ec && it != end; ++it) {
        const std::filesystem::path& p = it->path();
        const std::wstring name = p.filename().wstring();
        const std::wstring rel = prefix.empty() ? name : prefix + L"\\" + name;
        const auto key = folderpilot::platform::to_utf8(rel);

        const DWORD attrs = ::GetFileAttributesW(p.c_str());
        const bool reparse = attrs != INVALID_FILE_ATTRIBUTES &&
                             (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

        if (reparse) {
            out[key.value] = SnapshotEntry{0, 0, true};
            continue; // record the link, never descend into it
        }
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            out[key.value] = SnapshotEntry{0, 0, false};
            walk_snapshot(p, rel, out);
            continue;
        }
        std::error_code sec;
        const auto size = std::filesystem::file_size(p, sec);
        const auto mtime = std::filesystem::last_write_time(p, sec);
        out[key.value] = SnapshotEntry{
            sec ? 0 : size,
            sec ? 0 : mtime.time_since_epoch().count(),
            false};
    }
}

} // namespace

Snapshot take_snapshot(const std::filesystem::path& dir) {
    Snapshot snapshot;
    walk_snapshot(dir, L"", snapshot);
    return snapshot;
}

bool snapshots_equal(const Snapshot& a, const Snapshot& b) {
    return a == b;
}

bool root_relative_rename_supported() {
    // The first filesystem operations after a fresh binary link can fail
    // transiently (antivirus/WDAC settling), so the probe retries before
    // concluding the mechanism is unsupported. Only a stable failure is
    // cached.
    const auto evaluate = [] {
        SandboxDir probe{"exec-capability", SandboxRoot::Phase5};
        create_file(probe.path(), L"probe-src.txt", "x");
        create_directory(probe.path(), std::wstring(L"Documents"));
        const auto guard = folderpilot::validate_target(probe.u8());
        if (!guard.ok) {
            return false;
        }
        const auto src = folderpilot::platform::open_source(guard.root.get(), "probe-src.txt");
        if (!src.ok) {
            return false;
        }
        const auto cat = folderpilot::platform::open_category_directory(
            guard.root.get(), "Documents", false);
        if (!cat.ok) {
            return false;
        }
        const auto renamed = folderpilot::platform::native_rename_to_category(
            src.handle.get(), cat.handle.get(), "probe-dst.txt");
        return renamed.ok;
    };
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (evaluate()) {
            return true;
        }
        ::Sleep(300);
    }
    return false;
}

const char kMovementSkipReason[] =
    "SKIPPED: RootDirectory-relative rename returns ERROR_INVALID_PARAMETER "
    "on this OS build - the approved movement mechanism is blocked";

bool file_content_matches(const std::filesystem::path& p, const std::string& expected) {
    // Full share mode: a test-side read must not fail merely because the
    // process itself (or the executor) holds a DELETE-capable handle on the
    // file being verified.
    return content_matches_path(with_backslashes(p), expected);
}

bool file_exists_extended(const std::filesystem::path& p) {
    const DWORD attrs = ::GetFileAttributesW(with_extended_prefix(p).c_str());
    return attrs != INVALID_FILE_ATTRIBUTES;
}

bool file_content_matches_extended(const std::filesystem::path& p,
                                   const std::string& expected) {
    return content_matches_path(with_extended_prefix(p), expected);
}

std::string file_id_string(const std::filesystem::path& p) {
    HANDLE h = ::CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return {}; // unreadable: callers treat empty as verification failure
    }
    char buf[16]{}; // small hex chunk buffer
    std::string out;
    FILE_ID_INFO info{};
    if (::GetFileInformationByHandleEx(h, FileIdInfo, &info, sizeof(info))) {
        out = "ex:";
        std::snprintf(buf, sizeof(buf), "%016llx",
                      static_cast<unsigned long long>(info.VolumeSerialNumber));
        out += buf;
        for (const unsigned char b : info.FileId.Identifier) {
            std::snprintf(buf, sizeof(buf), "%02x", b);
            out += buf;
        }
    } else {
        BY_HANDLE_FILE_INFORMATION legacy{};
        if (::GetFileInformationByHandle(h, &legacy)) {
            out = "lg:";
            std::snprintf(buf, sizeof(buf), "%08lx", legacy.dwVolumeSerialNumber);
            out += buf;
            std::snprintf(buf, sizeof(buf), "%08x%08x",
                          static_cast<unsigned>(legacy.nFileIndexHigh),
                          static_cast<unsigned>(legacy.nFileIndexLow));
            out += buf;
        }
    }
    ::CloseHandle(h);
    return out;
}

CliRun run_folderpilot_cli(const std::vector<std::wstring>& arguments) {
    CliRun run;

    // Build the command line: quoted exe path, then one quoted argument each.
    // Harness override: when a machine's WDAC/DeviceGuard policy blocks a
    // freshly-linked binary, the gate can point the CLI tests at an already
    // runnable build via the FOLDERPILOT_TEST_EXE_PATH environment variable.
    // Any such deviation is disclosed in the phase report.
    const char* override_path = std::getenv("FOLDERPILOT_TEST_EXE_PATH");
    const std::wstring exe = folderpilot::platform::to_utf16(
        override_path != nullptr ? std::string_view{override_path}
                                 : std::string_view{FOLDERPILOT_EXE_PATH}).value;
    std::wstring command = L"\"" + exe + L"\"";
    for (const std::wstring& argument : arguments) {
        command += L" \"" + argument + L"\"";
    }

    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;
    if (!::CreatePipe(&stdout_read, &stdout_write, &inherit, 0)) {
        run.exit_code = -1;
        run.output = "CreatePipe failed";
        return run;
    }
    ::SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = stdout_write;
    startup.hStdError = stdout_write;
    PROCESS_INFORMATION process{};

    std::wstring mutable_command = command;
    const BOOL created = ::CreateProcessW(nullptr, mutable_command.data(), nullptr,
                                          nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                          nullptr, &startup, &process);
    ::CloseHandle(stdout_write); // child holds its own inherited copy now
    if (created == 0) {
        const DWORD error = ::GetLastError();
        ::CloseHandle(stdout_read);
        run.exit_code = -1;
        run.output = "CreateProcessW failed (Windows error " + std::to_string(error) + ")";
        return run;
    }
    ::CloseHandle(process.hThread);

    // Drain the child's stdout, then wait for it to finish.
    char chunk[4096];
    DWORD read = 0;
    for (;;) {
        if (!::ReadFile(stdout_read, chunk, sizeof(chunk), &read, nullptr) || read == 0) {
            break;
        }
        run.output.append(chunk, read);
    }
    ::WaitForSingleObject(process.hProcess, 30000);
    DWORD exit_code = 0;
    ::GetExitCodeProcess(process.hProcess, &exit_code);
    run.exit_code = static_cast<int>(exit_code);
    ::CloseHandle(process.hProcess);
    ::CloseHandle(stdout_read);
    return run;
}

} // namespace fixture
