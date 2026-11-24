// FolderPilot test runner entry point.
// Exactly one translation unit (this one) defines DOCTEST_CONFIG_IMPLEMENT.

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <thread>

#include "folderpilot/storage_adapter.hpp"
#include "native.hpp"
#include "text.hpp"

// ---------------------------------------------------------------------------
// Cross-process lock-test child mode.
//
// The hosted cross-process suite spawns THIS executable as a child with
//   --fp-lock-child <organized-root> <storage-base> <ready-marker>
//                  <proceed-marker>
// The protocol is parsed BEFORE the doctest context is constructed, so a
// normal test run is completely unaffected. The child:
//   1. bootstraps the same synthetic storage root as the parent,
//   2. acquires the mutation lock,
//   3. writes the ready marker ONLY after the acquisition succeeded; its
//      content is the child's lock_id (the parent later compares it against
//      its own acquisition of the same lock file),
//   4. polls for the proceed marker within a bounded deadline,
//   5. exits 0 on proceed (RAII releases the lock) and distinct nonzero
//      codes on failure/timeout. Parent-initiated termination releases the
//      lock through the kernel (crash-release path).
// All paths are synthetic sandbox paths (ASCII by construction, so the
// narrow argv encoding is exact).
// ---------------------------------------------------------------------------
int run_lock_child(const std::string& organized_root,
                   const std::string& storage_base,
                   const std::string& ready_marker,
                   const std::string& proceed_marker) {
    const auto root = folderpilot::platform::open_root(organized_root);
    if (!root.ok) {
        return 3;
    }
    const auto storage = folderpilot::open_storage_root(storage_base, root.root);
    if (!storage.ok) {
        return 3;
    }
    const auto lock = folderpilot::acquire_mutation_lock(storage);
    if (!lock.ok) {
        return 3;
    }

    {
        std::ofstream ready(ready_marker, std::ios::binary | std::ios::trunc);
        if (!ready.is_open()) {
            return 3;
        }
        ready << lock.lock_id;
        ready.flush();
        if (!ready.good()) {
            return 3;
        }
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(60);
    for (;;) {
        std::error_code ec;
        if (std::filesystem::exists(proceed_marker, ec)) {
            return 0; // RAII releases the lock: the clean-release path
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return 2; // timeout; process exit releases the lock regardless
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}

int main(int argc, char** argv) {
    // Unbuffered diagnostics: if a test case hangs, the last printed line
    // identifies it exactly.
    std::cout.setf(std::ios::unitbuf);

    // Private child-mode protocol, consumed before doctest sees argv.
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--fp-lock-child") {
            if (argc < 6) {
                return 3;
            }
            return run_lock_child(argv[2], argv[3], argv[4], argv[5]);
        }
    }

#ifdef FOLDERPILOT_STORAGE_SANDBOX
    // Test isolation: every spawned CLI instance (and every directly linked
    // test) inherits a PRIVATE storage base under the build directory. The
    // child-mode protocol above runs BEFORE this block because it receives
    // its own explicit storage base. The PID suffix isolates concurrent runs
    // without deleting another process's journal; no test ever creates a
    // root-key directory in the user's real %LOCALAPPDATA% journal storage.
    {
        const std::string sandbox =
            std::string{FOLDERPILOT_STORAGE_SANDBOX} + "-" +
            std::to_string(::GetCurrentProcessId());
        std::error_code ec;
        std::filesystem::create_directories(sandbox, ec);
        if (ec) {
            std::cerr << "STORAGE SANDBOX CREATE FAILURE: " << ec.message() << '\n';
            return 3;
        }
        const auto wide = folderpilot::platform::to_utf16(sandbox);
        // CMake emits binaryDir with forward slashes; the ratified storage
        // base open accepts plain drive-form paths only.
        std::wstring sandbox_path = wide.ok ? wide.value : std::wstring{};
        for (wchar_t& c : sandbox_path) {
            if (c == L'/') {
                c = L'\\';
            }
        }
        if (!wide.ok || sandbox_path.empty() ||
            !::SetEnvironmentVariableW(L"FOLDERPILOT_STORAGE_BASE",
                                       sandbox_path.c_str())) {
            std::cerr << "STORAGE SANDBOX INJECTION FAILURE\n";
            return 3;
        }
    }
#endif

    doctest::Context context(argc, argv);

    // Run all registered test cases. The returned value is suitable as a
    // process exit code: 0 = all tests passed, non-zero = failures.
    const int result = context.run();

    // Propagate the result; querying shouldExit() honors flags such as
    // --exit and is the canonical pattern from the doctest documentation.
    if (context.shouldExit()) {
        return result;
    }
    return result;
}
