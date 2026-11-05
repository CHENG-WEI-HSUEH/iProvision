// libtest_plugin.so: the simplest example plugin, used to test the manager (no GPU required).
//
//   start t1 test               logs one line per second
//   start t2 test 0.2           logs one line every 0.2 seconds
//
// It handles SIGTERM: on receipt it logs a message and exits normally (exit code 0), demonstrating a "graceful stop".
// All output goes through APP_LOG (glog); the texts are TEST_* in configs/log_messages.yaml.

// APP_LOG (so_runner has already initialised logging before calling plugin_main)
#include "app_log.hpp"
// PLUGIN_EXPORT
#include "plugin_api.hpp"

// std::signal, sig_atomic_t
#include <csignal>
// std::snprintf
#include <cstdio>
// std::atof
#include <cstdlib>
// std::to_string
#include <string>
// std::this_thread::sleep_for
#include <thread>
// std::chrono
#include <chrono>
// getpid
#include <unistd.h>

// Set to 1 when SIGTERM is received
static volatile std::sig_atomic_t g_stop = 0;

// SIGTERM handler: only sets the flag, letting the main loop finish on its own
static void OnTerm(int) { g_stop = 1; }

// Plugin entry point
PLUGIN_EXPORT int plugin_main(int argc, char* argv[]) {
    // The first argument is the interval in seconds; defaults to 1 second if not given
    const double interval = argc > 1 ? std::atof(argv[1]) : 1.0;
    // Register the SIGTERM handler (the manager's stop command sends SIGTERM)
    std::signal(SIGTERM, OnTerm);
    // The interval as text with two decimals (std::to_string would give "1.000000")
    char text[32];
    // e.g. "0.50"
    std::snprintf(text, sizeof text, "%.2f", interval);
    // Log the start-up message
    APP_LOG("TEST_STARTED", {{"pid", std::to_string(getpid())}, {"interval", text}});
    // Counter
    long tick = 0;
    // Loop until SIGTERM is received
    while (!g_stop) {
        // Log the current count
        APP_LOG("TEST_TICK", {{"tick", std::to_string(++tick)}});
        // Wait for one interval
        std::this_thread::sleep_for(std::chrono::duration<double>(interval));
    }
    // A stop was requested, so exit normally
    APP_LOG("TEST_STOPPED", {{"tick", std::to_string(tick)}});
    // Exit code 0
    return 0;
}
