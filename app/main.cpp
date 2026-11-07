// FolderPilot - application entry point (Phase 5: dry-run + --apply).
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
// Known limitation (deferred to a later phase): command-line arguments are
// read through the ANSI entry point, so non-ASCII directory arguments may
// not round-trip on every system. Library-level Unicode is fully supported.

#include <iostream>
#include <string>
#include <vector>

#include "folderpilot/executor.hpp"
#include "folderpilot/path_guard.hpp"
#include "folderpilot/planner.hpp"
#include "folderpilot/reporter.hpp"
#include "folderpilot/scanner.hpp"

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

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv, argv + argc);

    std::string directory;
    bool apply = false;
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

    // 1. Validate the target (single path-based bootstrap open).
    const auto guard = folderpilot::validate_target(directory);
    if (!guard.ok) {
        std::cout << "Target rejected: " << guard.error << "\n";
        return 2;
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

    // 4b. Explicitly authorized execution against the validated root.
    const auto result = folderpilot::execute_plan(plan, directory);
    std::cout << folderpilot::render_execution_report(plan, result);
    if (result.aborted) {
        return 2; // nothing was moved
    }
    return result.all_moved() ? 0 : 3;
}
