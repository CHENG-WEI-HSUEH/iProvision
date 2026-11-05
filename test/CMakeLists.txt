# =============================================================================
# GoogleTest suite for iProvision
#
# Build and run everything (tests + pass-rate report + coverage report) with:
#     test/run_tests.sh
#
# Or by hand (the build folder MUST be a direct child of the project root, e.g. build-test/,
# because so_runner finds ../configs/log_messages.yaml relative to its own folder):
#     cmake -S test -B build-test -DCMAKE_BUILD_TYPE=Debug
#     cmake --build build-test -j$(nproc)
#     ctest --test-dir build-test --output-on-failure
#
# This is a stand-alone CMake project: the main CMakeLists.txt is not modified.
# The sources under test are compiled again here with --coverage so gcov can measure them.
# =============================================================================

# Same minimum version as the main project
cmake_minimum_required(VERSION 3.20)
# Separate project name so it never clashes with the main build
project(iProvision_tests LANGUAGES CXX)

# The sources under test use C++17 (std::filesystem, structured bindings, inline constexpr)
set(CMAKE_CXX_STANDARD 17)
# Fail instead of silently falling back to an older standard
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Coverage numbers are only meaningful without optimisation, so default to Debug
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    # Debug = -g, no -O2 (inlining would hide lines from gcov)
    set(CMAKE_BUILD_TYPE Debug)
# End of if
endif()

# Absolute path of the project root (the folder above test/)
get_filename_component(PROJECT_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)
# Where the gtest XML results and the generated reports are written
set(REPORT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/reports)
# Scratch folder for temporary files created by the tests (YAML files, logs, scripts)
set(TEST_TMP_DIR ${CMAKE_BINARY_DIR}/tmp)

# so_runner looks for <its folder>/../configs/log_messages.yaml, so the build folder must sit in the project root
get_filename_component(BUILD_PARENT ${CMAKE_BINARY_DIR}/.. ABSOLUTE)
# Build folder is somewhere else
if(NOT BUILD_PARENT STREQUAL PROJECT_ROOT)
    # Only warn: the unit tests still work, but so_runner integration tests will see the built-in log defaults
    message(WARNING "Build folder should be a direct child of ${PROJECT_ROOT} (e.g. build-test/); "
                    "otherwise so_runner cannot find configs/log_messages.yaml")
# End of if
endif()

# Turn coverage instrumentation on or off (cmake -DENABLE_COVERAGE=OFF ...)
option(ENABLE_COVERAGE "Compile with --coverage so gcov can produce a coverage report" ON)
# Coverage wanted
if(ENABLE_COVERAGE)
    # --coverage = -fprofile-arcs -ftest-coverage; -O0 keeps one-to-one line mapping;
    # -fprofile-update=atomic keeps counters correct in the multi-threaded BlockingQueue tests
    add_compile_options(--coverage -O0 -g -fprofile-update=atomic)
    # The linker must also pull in libgcov
    add_link_options(--coverage)
# End of if
endif()

# Every executable (tests and so_runner) is placed directly in the build folder
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
# Every .so (libapp_log.so, libtest_plugin.so, ...) too
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# ---------------------------------------------------------------------------
# External packages (the same ones the main project uses, plus GoogleTest)
# ---------------------------------------------------------------------------
# Boost.Process v2 + Asio (header only)
find_package(Boost 1.80 REQUIRED)
# pthread
find_package(Threads REQUIRED)
# yaml-cpp: processes.yaml / log_messages.yaml
find_package(yaml-cpp REQUIRED)
# Google glog: back end of app_log
find_package(glog 0.6 REQUIRED)
# GoogleTest: GTest::gtest and GTest::gtest_main (sudo apt install libgtest-dev)
find_package(GTest REQUIRED)

# ---------------------------------------------------------------------------
# Code under test, rebuilt with coverage
# ---------------------------------------------------------------------------
# libapp_log.so: shared, exactly like the main build (so_runner and the plugins must share one copy)
add_library(app_log SHARED ${PROJECT_ROOT}/src/log/app_log.cpp)
# include/app_log.hpp
target_include_directories(app_log PUBLIC ${PROJECT_ROOT}/include)
# glog and yaml-cpp are used only inside app_log
target_link_libraries(app_log PRIVATE glog::glog yaml-cpp::yaml-cpp)
# Symbols stay public: the plugins call app_log::Write
set_target_properties(app_log PROPERTIES CXX_VISIBILITY_PRESET default)

# All manager / console sources in one static library: each source is compiled once,
# so every test executable updates the same .gcda file and coverage merges naturally
add_library(core STATIC
    # ResolveFromExecutable
    ${PROJECT_ROOT}/src/app_config.cpp
    # Tokenize, Duration, EditDistance, ClosestWord
    ${PROJECT_ROOT}/src/console/text_utils.cpp
    # InstallShutdownSignals, ShutdownRequested
    ${PROJECT_ROOT}/src/console/shutdown_signal.cpp
    # LoadPrograms
    ${PROJECT_ROOT}/src/manager/program_config.cpp
    # ManagedProcess
    ${PROJECT_ROOT}/src/manager/managed_process.cpp
    # ProcessManager
    ${PROJECT_ROOT}/src/manager/process_manager.cpp
    # Console
    ${PROJECT_ROOT}/src/console/console.cpp
# End of add_library
)
# src/ for "manager/..." includes, include/ for app_log.hpp, blocking_queue.hpp, plugin_api.hpp
target_include_directories(core PUBLIC ${PROJECT_ROOT}/src ${PROJECT_ROOT}/include)
# Same dependencies as the iprovision executable
target_link_libraries(core PUBLIC Boost::headers Threads::Threads yaml-cpp::yaml-cpp app_log)
# Same warnings as the main build
target_compile_options(core PRIVATE -Wall -Wextra)

# so_runner: the real child-process host, used by the Console and integration tests
add_executable(so_runner ${PROJECT_ROOT}/src/runner/so_runner.cpp)
# core supplies app_config.o (only the objects actually needed are linked); libdl for dlopen
target_link_libraries(so_runner PRIVATE core ${CMAKE_DL_LIBS})

# libtest_plugin.so: the real test plugin (one log line per tick, graceful SIGTERM)
add_library(test_plugin SHARED ${PROJECT_ROOT}/plugins/test_plugin.cpp)
# app_log for APP_LOG
target_link_libraries(test_plugin PRIVATE app_log)
# Like the main build: everything hidden except plugin_main
set_target_properties(test_plugin PROPERTIES CXX_VISIBILITY_PRESET hidden)

# libexit_code_plugin.so: test-only plugin that prints argv and returns argv[1] as its exit code
add_library(exit_code_plugin SHARED fixtures/exit_code_plugin.cpp)
# include/plugin_api.hpp
target_include_directories(exit_code_plugin PRIVATE ${PROJECT_ROOT}/include)
# Everything hidden except plugin_main
set_target_properties(exit_code_plugin PROPERTIES CXX_VISIBILITY_PRESET hidden)

# ---------------------------------------------------------------------------
# Test executables
# ---------------------------------------------------------------------------
# Enable ctest (add_test below)
enable_testing()

# add_gtest(<name> <sources...>): one GoogleTest executable + one ctest entry
function(add_gtest name)
    # The executable, built from the sources given after the name
    add_executable(${name} ${ARGN})
    # Code under test + GoogleTest (gtest_main provides main())
    target_link_libraries(${name} PRIVATE core GTest::gtest GTest::gtest_main)
    # test/common/test_helpers.hpp
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/common)
    # Paths the tests need, passed in as string macros (only the test build does this)
    target_compile_definitions(${name} PRIVATE
        # Project root: configs/processes.yaml, configs/log_messages.yaml
        PROJECT_ROOT_DIR="${PROJECT_ROOT}"
        # Scratch folder
        TEST_TMP_DIR="${TEST_TMP_DIR}"
        # The instrumented so_runner
        SO_RUNNER_PATH="$<TARGET_FILE:so_runner>"
        # The instrumented libtest_plugin.so
        TEST_PLUGIN_PATH="$<TARGET_FILE:test_plugin>"
        # The fixture plugin
        EXIT_CODE_PLUGIN_PATH="$<TARGET_FILE:exit_code_plugin>"
        # A real .so that has no plugin_main (used to test RUNNER_NO_ENTRY)
        APP_LOG_LIB_PATH="$<TARGET_FILE:app_log>"
    # End of target_compile_definitions
    )
    # Warnings on the test code too
    target_compile_options(${name} PRIVATE -Wall -Wextra)
    # The runner and the plugins must exist before any test runs
    add_dependencies(${name} so_runner test_plugin exit_code_plugin)
    # Register with ctest; the XML result goes to test/reports/xml/<name>.xml
    add_test(NAME ${name} COMMAND ${name} --gtest_output=xml:${REPORT_DIR}/xml/${name}.xml)
    # Death tests and spawned processes need a writable temp folder for gtest's stderr capture
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "TEST_TMPDIR=${TEST_TMP_DIR}")
# End of function
endfunction()

# Tokenize / Duration / EditDistance / ClosestWord
add_gtest(test_text_utils        unit/test_text_utils.cpp)
# app_config constants and ResolveFromExecutable
add_gtest(test_app_config        unit/test_app_config.cpp)
# SIGINT / SIGTERM / SIGHUP handling
add_gtest(test_shutdown_signal   unit/test_shutdown_signal.cpp)
# app_log: catalogue loading, placeholders, levels, file sink
add_gtest(test_app_log           unit/test_app_log.cpp)
# LoadPrograms (processes.yaml)
add_gtest(test_program_config    unit/test_program_config.cpp)
# include/blocking_queue.hpp
add_gtest(test_blocking_queue    unit/test_blocking_queue.cpp)
# ManagedProcess: start / pause / resume / stop real child processes
add_gtest(test_managed_process   unit/test_managed_process.cpp)
# ProcessManager: names, many children, StopAll
add_gtest(test_process_manager   unit/test_process_manager.cpp)
# Console: scripted command sessions through std::cin / std::cout
add_gtest(test_console           integration/test_console.cpp)
# so_runner + libtest_plugin.so + fixture plugin, end to end
add_gtest(test_so_runner         integration/test_so_runner.cpp)
