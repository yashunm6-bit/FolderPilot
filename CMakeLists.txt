cmake_minimum_required(VERSION 3.21)

project(FolderPilot
    VERSION 0.1.0
    DESCRIPTION "FolderPilot - a safe, Windows-first file organization utility"
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Language settings: C++20 is mandatory, compiler-specific extensions off.
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Shared build options: umbrella target that all FolderPilot targets link.
# GCC (MSYS2 UCRT64) is the approved toolchain for this project.
# ---------------------------------------------------------------------------
add_library(folderpilot_build_options INTERFACE)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(folderpilot_build_options INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        # Note: -Werror is intentionally NOT enabled at this stage.
    )
    if(WIN32)
        # Self-contained executables: link the GCC runtime (libgcc, libstdc++,
        # winpthread) statically. Without this, the Windows DLL search can find
        # other libstdc++/libgcc copies on PATH (e.g. Git-for-Windows' mingw64
        # DLLs) before the UCRT64 ones, which crashes the process with
        # STATUS_ENTRYPOINT_NOT_FOUND. Static linking removes any dependency
        # on PATH order.
        target_link_options(folderpilot_build_options INTERFACE -static)
    endif()
endif()

# ---------------------------------------------------------------------------
# Application: the FolderPilot CLI.
# Phase 1 scope: scaffold entry point only. Scanning, classification,
# planning and movement arrive in later phases.
# ---------------------------------------------------------------------------
add_executable(folderpilot
    app/main.cpp
)
target_compile_definitions(folderpilot PRIVATE
    FOLDERPILOT_VERSION="${PROJECT_VERSION}"
)
target_link_libraries(folderpilot PRIVATE folderpilot_build_options)

# ---------------------------------------------------------------------------
# Testing: CTest wiring; the test targets live in tests/.
# ---------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)
