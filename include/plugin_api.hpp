#ifndef PLUGIN_API_HPP
#define PLUGIN_API_HPP

// Plugin (.so) interface: after so_runner loads a .so with dlopen, it uses dlsym to look up the function named below and calls it.
//
//   int plugin_main(int argc, char* argv[]);
//
//   argc / argv: argv[0] is the path of the .so; argv[1..] are the arguments given to the start command (just like an ordinary main)
//   Return value: the child process's exit code (0 = success)
//
// Any .so that defines plugin_main with PLUGIN_EXPORT can be launched by the manager.
// See plugins/test_plugin.cpp for an example.

// extern "C": keeps the function name as "plugin_main" so the C++ compiler does not rename it (name mangling); otherwise dlsym could not find it.
// visibility("default"): even when the .so is compiled with -fvisibility=hidden (all symbols hidden by default), this function remains publicly visible.
#define PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))

// The function name that so_runner looks up with dlsym (must match the name of plugin_main above)
#define PLUGIN_ENTRY_NAME "plugin_main"

// Function pointer type: points to int plugin_main(int argc, char* argv[]); so_runner casts the result of dlsym to this type before calling it
using PluginMainFn = int (*)(int argc, char* argv[]);

#endif  // PLUGIN_API_HPP
