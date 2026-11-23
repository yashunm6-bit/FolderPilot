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
#include "folderpilot/retry_plan.hpp"
#include "folderpilot/scanner.hpp"
#include "folderpilot/storage_adapter.hpp"
#include "folderpilot/undo_plan.hpp"
#include "folderpilot/undo_execute.hpp"
#include "storage.hpp"
#include "text.hpp"

namespace {

int print_usage() {
    std::cout <<
        "Usage:\n"
        "  folderpilot <directory>           preview the organization plan (dry run)\n"
        "  folderpilot <directory> --apply   execute the planned movements (journaled)\n"
        "  folderpilot <directory> --undo    preview the undo plan (dry run)\n"
        "  folderpilot <directory> --undo --apply\n"
        "                                    execute the undo (restore moved files)\n"
        "  folderpilot <directory> --apply --acknowledge-undo-window\n"
        "                                    forward execute, superseding the live\n"
        "                                    undo window (durably recorded)\n"
        "  folderpilot <directory> --recursive [--apply]\n"
        "                                    organize files in subdirectories too\n"
        "  folderpilot <directory> --retry [--apply]\n"
        "                                    re-attempt locked/denied moves from\n"
        "                                    the newest journaled run\n"
        "  folderpilot --help                show this help\n"
        "  folderpilot --version             show version information\n"
        "\n"
        "The default operation is always a read-only dry run: nothing is moved,\n"
        "created, or deleted. File movement requires the explicit --apply flag;\n"
        "undo restores previously moved files and requires --undo --apply.\n"
        "Every --apply run is journaled; --acknowledge-undo-window is only valid\n"
        "with --apply or --retry --apply (forward execution).\n";
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
    bool recursive = false;
    bool retry = false;
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
        if (arg == "--recursive") {
            recursive = true;
            continue;
        }
        if (arg == "--retry") {
            retry = true;
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
    // Ratified §2 grammar (Phase 8 amendment): the acknowledgement option is
    // restricted to an actual forward EXECUTION - --apply or --retry --apply
    // (a retry apply supersedes a live window only through the same ratified
    // acknowledgement). A read-only preview (including --retry alone), --undo
    // or a dry run is a usage error (exit 1).
    if (acknowledge_undo_window && (undo || !apply)) {
        std::cout << "--acknowledge-undo-window is only valid with --apply or "
                     "--retry --apply (forward execution)\n";
        return print_usage();
    }
    // Ratified Phase 8 grammar: --recursive is forward-only; --retry is
    // forward-only and mutually exclusive with --recursive/--undo (a retry
    // is itself a forward run).
    if (recursive && (undo || retry)) {
        std::cout << "--recursive cannot be combined with --undo or --retry\n";
        return print_usage();
    }
    if (retry && (undo || recursive)) {
        std::cout << "--retry cannot be combined with --undo or --recursive\n";
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

    folderpilot::ScanOptions scan_options;
    scan_options.recursive = recursive;
    if (retry) {
        // Retry mode (§1.4): the preview lists the retryable candidates with
        // their fresh preflight results; --retry --apply executes a fresh
        // fully-journaled run over exactly the preflighted files, under the
        // SAME ratified §9.5 live-undo-window gate as every other forward
        // apply (a live window requires --acknowledge-undo-window).
        const auto retry_plan =
            folderpilot::load_retry_plan(storage_base.path_utf8, guard.root);
        if (!retry_plan.ok) {
            std::cout << "Retry planning failed: " << retry_plan.error << "\n";
            return 3;
        }
        if (retry_plan.busy) {
            std::cout << "Retry unavailable: " << retry_plan.blocked_detail << "\n";
            return 2;
        }
        if (retry_plan.blocked) {
            std::cout << "The newest journal is blocked: " << retry_plan.blocked_detail
                      << "\n";
            return 2;
        }
        if (!apply) {
            std::cout << "RETRY PREVIEW\n";
            if (retry_plan.candidates.empty()) {
                std::cout << "  nothing to retry\n";
                return 0;
            }
            for (const auto& c : retry_plan.candidates) {
                std::cout << "  move " << c.move_id << ": " << c.source_name
                          << " (recorded " << journal_outcome_name(c.recorded_outcome)
                          << ") - "
                          << (c.preflight_ok
                                  ? std::string("retryable")
                                  : std::string("NOT retryable: ") + c.preflight_detail)
                          << "\n";
            }
            if (retry_plan.not_retryable != 0) {
                std::cout << "  not retryable (permanent/identity/completed): "
                          << retry_plan.not_retryable << "\n";
            }
            if (retry_plan.preflight_failed != 0) {
                std::cout << "  failed preflight: " << retry_plan.preflight_failed
                          << "\n";
            }
            return 0;
        }
        // Retry apply: a fresh scan filtered to the preflighted candidates
        // (identity digest AND creation time must match), planned and
        // executed as a fresh fully-journaled run. The scan is RECURSIVE:
        // candidates recorded by a --recursive run carry nested relative
        // paths no flat scan can match.
        folderpilot::ScanOptions retry_scan_options;
        retry_scan_options.recursive = true;
        const auto scan = folderpilot::scan_directory(guard.root, retry_scan_options);
        if (scan.enumeration != folderpilot::EnumerationStatus::Ok) {
            std::cout << "Scan failed: " << scan.error << "\n";
            return 3;
        }
        folderpilot::ScanResult filtered;
        filtered.enumeration = folderpilot::EnumerationStatus::Ok;
        filtered.root = scan.root;
        std::size_t matched = 0;
        for (auto& file : scan.files) {
            for (const auto& c : retry_plan.candidates) {
                if (!c.preflight_ok) {
                    continue;
                }
                std::string live_digest;
                if (file.name_utf8 == c.source_name &&
                    folderpilot::storage_leaf_id(file.identity, live_digest) &&
                    live_digest == c.file_id_hex &&
                    file.creation_time_filetime == c.file_ctime_filetime) {
                    filtered.files.push_back(std::move(file));
                    ++matched;
                    break; // matched: stop comparing candidates for this file
                }
            }
        }
        (void)matched;
        if (filtered.files.empty()) {
            std::cout << "Nothing to retry\n";
            return 0;
        }
        const auto plan = folderpilot::build_plan(guard.root, filtered);
        if (!plan.valid()) {
            std::cout << "Planning failed: " << plan.error() << "\n";
            return 2;
        }
        const auto result = folderpilot::execute_plan(
            plan, directory, storage_base.path_utf8, acknowledge_undo_window);
        std::cout << folderpilot::render_execution_report(plan, result);
        if (result.aborted) {
            return result.abort_exit_code;
        }
        if (result.journal_stopped) {
            return 3;
        }
        return result.all_moved() ? 0 : 3;
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

    // 2. Scan (handle-anchored; recursive per --recursive).
    const auto scan = folderpilot::scan_directory(guard.root, scan_options);
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
