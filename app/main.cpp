// FolderPilot - application entry point (Phase 6: dry-run + --apply).
//
//   folderpilot <directory>            read-only dry run (default)
//   folderpilot <directory> --apply    execute the planned movements
//   folderpilot --help | --version
//
// The dry run never modifies anything. Actual movement happens only when
// the user explicitly passes --apply; there is no confirmation prompt, no
// background mode, and no --force.
//
// Exit codes:
//   0  dry-run report printed / execution completed with all moves done
//   1  usage error
//   2  safety rejection (target refused, root changed, invalid plan)
//   3  scan or planning failed / execution completed with failures
//
// Arguments arrive through the UTF-16 (wmain) entry point and are converted
// to UTF-8 with the same strict converter the library uses, so non-ASCII
// directory paths round-trip exactly (Phase 6 Unicode CLI support).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <iostream>
#include <string>
#include <vector>

#include "folderpilot/executor.hpp"
#include "folderpilot/path_guard.hpp"
#include "folderpilot/planner.hpp"
#include "folderpilot/reporter.hpp"
#include "folderpilot/scanner.hpp"
#include "storage.hpp"
#include "text.hpp"

namespace {

int print_usage() {
    std::cout <<
        "Usage:\n"
        "  folderpilot <directory>           preview the organization plan (dry run)\n"
        "  folderpilot <directory> --apply   execute the planned movements\n"
        "  folderpilot --help                show this help\n"
        "  folderpilot --version             show version information\n"
        "\n"
        "The default operation is always a read-only dry run:\n"
        "nothing is moved, created, or deleted.\n"
        "File movement requires the explicit --apply flag.\n";
    return 1;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    // Console output code page: the reporter emits UTF-8. Without this, a
    // legacy-codepage console renders non-ASCII paths as mojibake. Display
    // only - pipes, redirection and the test harness's pipe capture are
    // unaffected, and the emitted bytes are identical either way.
    ::SetConsoleOutputCP(CP_UTF8);

    // Strict UTF-16 -> UTF-8 conversion of every command-line argument: the
    // library's UTF-8 interfaces receive the exact command-line text. A
    // conversion failure is not expected for well-formed Windows arguments
    // and fails closed as a usage error.
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc > 0 ? argc : 0));
    for (int i = 0; i < argc; ++i) {
        const auto converted =
            folderpilot::platform::to_utf8(std::wstring_view{argv[i]});
        if (!converted.ok) {
            std::cout << "Argument cannot be represented in UTF-8; rejected.\n";
            return print_usage();
        }
        args.push_back(std::move(converted.value));
    }

    std::string directory;
    bool apply = false;
    bool undo = false;
    bool acknowledge_undo_window = false;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--help") {
            print_usage();
            return 0;
        }
        if (arg == "--version") {
            std::cout << "FolderPilot " << FOLDERPILOT_VERSION
                      << " (dry-run + apply build)\n";
            return 0;
        }
        if (arg == "--apply") {
            apply = true;
            continue;
        }
        if (arg == "--undo") {
            undo = true;
            continue;
        }
        if (arg == "--acknowledge-undo-window") {
            acknowledge_undo_window = true;
            continue;
        }
        if (!arg.empty() && arg[0] == '-') {
            std::cout << "Unknown option: " << arg << "\n";
            print_usage();
            return 1;
        }
        if (!directory.empty()) {
            std::cout << "Unexpected extra argument: " << arg << "\n";
            print_usage();
            return 1;
        }
        directory = arg;
    }

    if (directory.empty()) {
        return print_usage();
    }
    // Ratified §2 grammar: the acknowledgement option is restricted to
    // forward execution; with --undo or without --apply it is a usage error
    // (exit 1).
    if (acknowledge_undo_window && (!apply || undo)) {
        std::cout << "--acknowledge-undo-window is only valid with --apply "
                     "(forward execution)\n";
        return print_usage();
    }

    // 1. Validate the target (single path-based bootstrap open).
    const auto guard = folderpilot::validate_target(directory);
    if (!guard.ok) {
        std::cout << "Target rejected: " << guard.error << "\n";
        return 2;
    }

    const auto storage_base = folderpilot::platform::local_app_data_path();
    if (!storage_base.ok) {
        std::cout << "Storage base unavailable: " << storage_base.error << "\n";
        return 3;
    }

    if (undo) {
        // Undo mode (§2): the preview is the default; mutation requires the
        // explicit --apply. The preview is read-only (no lock, no writes).
        const auto undo_plan =
            folderpilot::load_undo_plan(storage_base.path_utf8, guard.root);
        if (!apply) {
            std::cout << folderpilot::render_undo_preview(undo_plan);
            return undo_plan.ok ? 0 : 3;
        }
        const auto result = folderpilot::execute_undo(
            undo_plan, storage_base.path_utf8, guard.root, true);
        std::cout << folderpilot::render_undo_execution(undo_plan, result);
        if (result.aborted) {
            return result.abort_exit_code;
        }
        if (result.segment_stopped) {
            return 3;
        }
        return (result.restored_count == result.eligible_count &&
                result.conflict_count == 0 && result.failed_count == 0)
                   ? 0
                   : 3;
    }

    // 2. Scan the immediate files (handle-anchored, non-recursive).
    const auto scan = folderpilot::scan_directory(guard.root);
    if (scan.enumeration != folderpilot::EnumerationStatus::Ok) {
        std::cout << "Scan failed: " << scan.error << "\n";
        return 3;
    }

    // 3. Classify, inspect destinations, resolve collisions, publish plan.
    const auto plan = folderpilot::build_plan(guard.root, scan);
    if (!plan.valid()) {
        std::cout << "Planning failed: " << plan.error() << "\n";
        return 2;
    }

    if (!apply) {
        // 4a. Read-only preview. Nothing is modified.
        std::cout << folderpilot::render_dry_run_report(plan);
        return 0;
    }

    // 4b. Explicitly authorized execution against the validated root. Every
    // mutation is journaled through the Phase 7.2 write-ahead session
    // (storage base: the %LOCALAPPDATA% analogue).
    const auto result = folderpilot::execute_plan(
        plan, directory, storage_base.path_utf8, acknowledge_undo_window);
    std::cout << folderpilot::render_execution_report(plan, result);
    if (result.aborted) {
        return result.abort_exit_code; // nothing was moved (2 safety / 3 operational)
    }
    if (result.journal_stopped) {
        return 3; // the run stopped on a durable-record failure (R1)
    }
    return result.all_moved() ? 0 : 3;
}
