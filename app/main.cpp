// FolderPilot - application entry point (Phase 4: read-only dry-run CLI).
//
// The default (and currently ONLY) operation is a read-only dry run:
//   folderpilot <directory>
// It validates the target, scans it, classifies the files, plans the
// organization and prints the report - without changing anything.
//
// Exit codes:
//   0  dry-run report printed
//   1  usage error or unsupported feature (--apply)
//   2  target rejected by the safety validation
//   3  scan or planning failed
//
// Known limitation (deferred to a later phase): command-line arguments are
// read through the ANSI entry point, so non-ASCII directory arguments may
// not round-trip on every system. Library-level Unicode is fully supported.

#include <iostream>
#include <string>
#include <vector>

#include "folderpilot/path_guard.hpp"
#include "folderpilot/planner.hpp"
#include "folderpilot/reporter.hpp"
#include "folderpilot/scanner.hpp"

namespace {

int print_usage() {
    std::cout <<
        "Usage:\n"
        "  folderpilot <directory>    preview the organization plan (dry run)\n"
        "  folderpilot --help         show this help\n"
        "  folderpilot --version      show version information\n"
        "\n"
        "The default operation is always a read-only dry run:\n"
        "nothing is moved, created, or deleted.\n";
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv, argv + argc);

    std::string directory;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--help") {
            print_usage();
            return 0;
        }
        if (arg == "--version") {
            std::cout << "FolderPilot " << FOLDERPILOT_VERSION
                      << " (dry-run preview build)\n";
            return 0;
        }
        if (arg == "--apply") {
            std::cout << "File movement is not available in this build. "
                         "No changes were made.\n";
            return 1;
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
        return 3;
    }

    // 4. Print the read-only preview. Nothing was modified.
    std::cout << folderpilot::render_dry_run_report(plan);
    return 0;
}
