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
        # Phase 3 uses Vista/Win8+ surfaces (GetFileInformationByHandleEx with
        # FileIdBothDirectoryInfo, FileIdInfo). Target Windows 10 uniformly so
        # every target sees the same declarations and struct layouts.
        target_compile_definitions(folderpilot_build_options INTERFACE
            _WIN32_WINNT=0x0A00
            WINVER=0x0A00
            NTDDI_VERSION=0x0A00000B
            NOMINMAX
        )
    endif()
endif()

# ---------------------------------------------------------------------------
# Core library: policy and orchestration logic (classification, PathGuard,
# scanner). Filesystem/Win32 calls happen only through the platform library.
# ---------------------------------------------------------------------------
add_library(folderpilot_core STATIC
    lib/folderpilot/category.cpp
    lib/folderpilot/rules.cpp
    lib/folderpilot/classifier.cpp
    lib/folderpilot/path_guard.cpp
    lib/folderpilot/scanner.cpp
    lib/folderpilot/collision_resolver.cpp
    lib/folderpilot/organization_plan.cpp
    lib/folderpilot/planner.cpp
    lib/folderpilot/reporter.cpp
    lib/folderpilot/executor.cpp
)
target_include_directories(folderpilot_core PUBLIC ${CMAKE_SOURCE_DIR}/lib)
target_link_libraries(folderpilot_core PUBLIC
    folderpilot_build_options
    folderpilot_platform
)

# ---------------------------------------------------------------------------
# Platform library: Windows-specific helpers (native file handling, RAII
# handles, UTF conversion, enumeration, native error mapping).
# ---------------------------------------------------------------------------
add_library(folderpilot_platform STATIC
    platform/text.cpp
    platform/native.cpp
)
target_include_directories(folderpilot_platform PUBLIC ${CMAKE_SOURCE_DIR}/platform)
target_link_libraries(folderpilot_platform PUBLIC folderpilot_build_options)

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
target_link_libraries(folderpilot PRIVATE
    folderpilot_build_options
    folderpilot_core
)

# ---------------------------------------------------------------------------
# Testing: CTest wiring; the test targets live in tests/.
# ---------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)
