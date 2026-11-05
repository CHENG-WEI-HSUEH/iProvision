// libisurgical_plugin.so: wraps libisurgical.so as a plugin.
//
// libisurgical.so only provides C++ classes (iSurgical, iSurgicalConfig) and has no plugin_main,
// so so_runner cannot run it directly. This adapter layer provides plugin_main, which does the same as the original main.cpp:
// read the arguments → create iSurgical → Run().
//
// The lines this file logs are ISURGICAL_* in configs/log_messages.yaml. What libisurgical.so prints itself
// (inference progress etc.) is written directly to the log file and cannot be reformatted from here.

// APP_LOG (so_runner has already initialised logging before calling plugin_main)
#include "app_log.hpp"
// iSurgicalConfig, iSurgical (from libisurgical.so)
#include "isurgical.hpp"
// PLUGIN_EXPORT
#include "plugin_api.hpp"

// std::string, std::to_string
#include <string>
// std::move
#include <utility>

// Join argv[1..] into one string for the log
static std::string JoinArgs(int argc, char* argv[]) {
    // The result
    std::string out;
    // Each option after the .so path
    for (int i = 1; i < argc; ++i) out += std::string(i > 1 ? " " : "") + argv[i];
    // Return it
    return out;
}

// Plugin entry point: argv[0] is the path of the .so; argv[1..] are the isurgical options (--input, --show, --no-rsd ...)
PLUGIN_EXPORT int plugin_main(int argc, char* argv[]) {
    // Default settings (model, config file paths, etc., relative to the child process's working directory)
    iSurgicalConfig config;
    // Override the defaults with the command-line options; on an unrecognised option, log it, print the usage and exit
    if (!iSurgicalConfig::FromArgs(argc, argv, config)) {
        // Log which options were rejected
        APP_LOG("ISURGICAL_BAD_ARGS", {{"args", JoinArgs(argc, argv)}});
        // Print every option isurgical supports (library output, not a log line)
        iSurgicalConfig::PrintUsage(argv[0]);
        // Invalid arguments
        return 2;
    }
    // Log the start with its options
    APP_LOG("ISURGICAL_STARTING", {{"args", JoinArgs(argc, argv)}});
    // Create iSurgical (the config is moved into the object)
    iSurgical app(std::move(config));
    // Run until the video ends or it is stopped; returns 0 on success, -1 on failure
    const int code = app.Run();
    // Log the result (not reached if the process is stopped with SIGTERM, since libisurgical does not handle it)
    APP_LOG("ISURGICAL_FINISHED", {{"code", std::to_string(code)}});
    // Exit code
    return code;
}
