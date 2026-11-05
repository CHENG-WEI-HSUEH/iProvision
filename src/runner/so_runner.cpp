// so_runner: the "host" executable of every child process.
//
//   so_runner <library.so> [args...]
//
// On Linux a new process must start from an executable (exec); a .so cannot run by itself.
// So every child the manager (Boost.Process) starts is one run of so_runner,
// which loads the given .so with dlopen and calls its plugin_main(argc, argv).
// The value plugin_main returns becomes the child's exit code.
//
// Logging: so_runner initialises app_log (glog) with Output::StandardError. The manager has redirected
// stderr into logs/<name>.log, so every APP_LOG line of so_runner and of the plugin ends up there.

// dlopen, dlsym, dlerror: load a .so at run time and look up its functions
#include <dlfcn.h>
// setvbuf, stdout
#include <cstdio>
// std::string, std::to_string
#include <string>
// getpid
#include <unistd.h>

// ResolveFromExecutable, kLogCatalogFile, kRunnerName
#include "app_config.hpp"
// APP_LOG, app_log::Init
#include "app_log.hpp"
// PluginMainFn (function pointer type), PLUGIN_ENTRY_NAME ("plugin_main")
#include "plugin_api.hpp"

// Exit code: invalid arguments (the usual convention for command-line tools)
constexpr int kExitUsage = 2;
// Exit code: loading failed (matches the 127 a shell uses when a program cannot be found)
constexpr int kExitLoadFailed = 127;

// Program entry point: argv[1] is the path of the .so, argv[2..] are passed to the plugin unchanged
int main(int argc, char* argv[]) {
    // Start logging; the catalogue is found relative to this executable, like the manager does
    app_log::Init(app_config::ResolveFromExecutable(app_config::kLogCatalogFile), app_config::kRunnerName,
                  app_log::Output::StandardError);

    // There must at least be the path of the .so (argv[0] is so_runner itself)
    if (argc < 2) {
        // Log the usage
        APP_LOG("RUNNER_USAGE");
        // Invalid arguments
        return kExitUsage;
    }

    // Output that does not go through glog (e.g. printf inside libisurgical) is redirected to the log file.
    // For files, C stdout is fully buffered by default; switch it to line buffering (_IOLBF)
    // so each line appears in the log straight away.
    setvbuf(stdout, nullptr, _IOLBF, 0);

    // The plugin arguments as one string, for the log line
    std::string args;
    // Each argument after the .so path
    for (int i = 2; i < argc; ++i) args += std::string(i > 2 ? " " : "") + argv[i];
    // This replaces the old "===== start =====" header: the first line of every run in logs/<name>.log
    APP_LOG("RUNNER_LOADING", {{"library", argv[1]}, {"pid", std::to_string(getpid())}, {"args", args}});

    // Load the .so. RTLD_NOW: resolve every symbol now, so a missing dependency fails here rather than mid-run;
    // RTLD_LOCAL: this .so's symbols are not visible to .so files loaded later, avoiding name clashes.
    void* handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    // Loading failed (file missing, not a .so, a dependency missing, ...)
    if (!handle) {
        // dlerror() returns the reason as text
        APP_LOG("RUNNER_LOAD_FAILED", {{"library", argv[1]}, {"reason", dlerror()}});
        // Loading failed
        return kExitLoadFailed;
    }

    // Look up plugin_main in the .so; dlsym returns void*, which must be cast to a function pointer to call it
    PluginMainFn entry = reinterpret_cast<PluginMainFn>(dlsym(handle, PLUGIN_ENTRY_NAME));
    // Not found: this .so does not implement the plugin interface (or PLUGIN_EXPORT was forgotten)
    if (!entry) {
        // Say which function is missing
        APP_LOG("RUNNER_NO_ENTRY", {{"library", argv[1]}, {"entry", PLUGIN_ENTRY_NAME}, {"reason", dlerror()}});
        // Also treated as a loading failure
        return kExitLoadFailed;
    }

    // Call the plugin with argv shifted by one: the plugin sees argv[0] = path of the .so, argv[1..] = the user's arguments.
    // No dlclose: the plugin may still have threads or global objects using its code, and unloading early would crash;
    // the operating system reclaims everything when the process ends.
    const int code = entry(argc - 1, argv + 1);
    // Log what the plugin returned (not reached if the process is killed by a signal)
    APP_LOG("RUNNER_FINISHED", {{"library", argv[1]}, {"code", std::to_string(code)}});
    // The plugin's return value is the exit code
    return code;
}
