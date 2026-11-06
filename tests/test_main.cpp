// FolderPilot test runner entry point.
// Exactly one translation unit (this one) defines DOCTEST_CONFIG_IMPLEMENT.

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest.h>

int main(int argc, char** argv) {
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
