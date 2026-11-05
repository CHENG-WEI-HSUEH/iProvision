#ifndef APP_LOG_HPP
#define APP_LOG_HPP

// Application logging built on Google glog.
//
// Every log line has the same shape:
//
//   2026-11-04 16:29:59.123 [info] [2001] [manager] [Start] started 't1' (pid 1234), log: /.../logs/t1.log
//   <timestamp>             [level] [code] [module] [function] <description>
//
// Everything except the timestamp and the function name comes from configs/log_messages.yaml:
//
//   messages:
//     MGR_STARTED:                       # the id used in the source code
//       code: 2001                       # [code]
//       level: info                      # [level]: info / warning / error
//       module: manager                  # [module]
//       description: "started '{name}' (pid {pid}), log: {log}"   # {name} etc. are filled in by the caller
//       function: ""                     # optional: overrides [function] (default = the C++ function name)
//
// Usage in code:
//
//   APP_LOG("MGR_STARTED", {{"name", name}, {"pid", std::to_string(pid)}, {"log", path}});
//
// This header is shared by the manager, so_runner and the plugins (they all link libapp_log.so),
// so a plugin loaded by so_runner writes through the same catalogue and the same glog instance.

// std::string
#include <string>
// std::pair
#include <utility>
// std::vector
#include <vector>

// Everything belongs to the app_log namespace
namespace app_log {

// One placeholder value: {"name", "op1"} replaces every "{name}" in the description with "op1"
using Param = std::pair<std::string, std::string>;
// All placeholder values for one log line
using Params = std::vector<Param>;

// Where this program writes its log lines
enum class Output {
    // Manager: terminal (if settings.console is true) and settings.manager_log_file
    ConsoleAndFile,
    // Child process (so_runner + plugin): stderr only; the manager already redirects it into logs/<name>.log
    StandardError,
};

// Load the message catalogue from catalog_path and initialise glog. Call once, at the very start of main().
// program_name must stay valid for the whole program (pass a string literal).
// Returns false if the catalogue could not be loaded; logging still works, using built-in defaults.
bool Init(const std::string& catalog_path, const char* program_name, Output output);

// Write one log line: look up id in the catalogue, fill in the placeholders and send it to glog.
// function is the name shown in [function] (APP_LOG passes __func__). An unknown id is logged as an error.
void Write(const char* id, const char* function, const Params& params = {});

}  // namespace app_log

// Shortcut that fills in the current function name automatically: APP_LOG("ID") or APP_LOG("ID", {{"k", "v"}})
// ##__VA_ARGS__ removes the preceding comma when no Params are given (GCC / Clang extension)
#define APP_LOG(id, ...) app_log::Write(id, __func__, ##__VA_ARGS__)

#endif  // APP_LOG_HPP
