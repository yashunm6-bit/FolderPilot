# FolderPilot test suite.
# Framework: doctest, vendored in third_party/doctest (pinned release).

add_executable(folderpilot_tests
    test_main.cpp
    unit/test_smoke.cpp
)

# doctest is third-party code: mark the include directory SYSTEM so that
# warnings are not reported for the vendored header. Our own test and
# production sources continue to build with full warnings.
target_include_directories(folderpilot_tests
    SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/third_party/doctest
)

target_link_libraries(folderpilot_tests PRIVATE folderpilot_build_options)

# One CTest entry point for now: the doctest binary runs all registered
# cases. Per-case discovery can be added when the suite grows (Phase 2+).
add_test(NAME smoke COMMAND folderpilot_tests)
