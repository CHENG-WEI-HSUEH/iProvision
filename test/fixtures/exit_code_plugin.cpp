// libexit_code_plugin.so: a test-only plugin for so_runner.
//
//   so_runner libexit_code_plugin.so <code> [more args...]
//
// It prints every argv entry (so a test can check what so_runner passed in) and returns <code> as the exit code.

// PLUGIN_EXPORT
#include "plugin_api.hpp"

// std::printf
#include <cstdio>
// std::atoi
#include <cstdlib>

// Plugin entry point (looked up by so_runner with dlsym)
PLUGIN_EXPORT int plugin_main(int argc, char* argv[]) {
    // Print the argument count (stdout is redirected into the child's log file)
    std::printf("argc=%d\n", argc);
    // Print every argument
    for (int i = 0; i < argc; ++i) std::printf("argv[%d]=%s\n", i, argv[i]);
    // Make sure it reaches the log before returning
    std::fflush(stdout);
    // argv[1] is the exit code to return; 0 if missing
    return argc > 1 ? std::atoi(argv[1]) : 0;
}
