#ifndef APP_CONFIG_HPP
#define APP_CONFIG_HPP

// std::string
#include <string>


// Program manager host executable: so_runner is the main program for child processes, which loads the specified .so and calls plugin_main().
// The path is relative to the executable, e.g. "so_runner" → "/.../build/so_runner"
// Processes.yaml defines child processes to be started by the manager.
namespace app_config {


  // kRunnerFile stores so_runner main prgrame name.
  inline constexpr const char* kRunnerFile = "so_runner";

  // Processes.yaml defines child processes to be started by the manager.
  inline constexpr const char* kProgramsFile = "../configs/processes.yaml";

  // Log file paths for storing child process stdout/stderr outputs.
  inline constexpr const char* kLogDir = "../logs";

  // Log message catalogue (code / level / module / description of every log line), shared by the manager and so_runner
  inline constexpr const char* kLogCatalogFile = "../configs/log_messages.yaml";

  // Program names handed to glog (used internally by glog only)
  inline constexpr const char* kManagerName = "iprovision";
  // so_runner's name for glog
  inline constexpr const char* kRunnerName = "so_runner";


  // Convert a path relative to the executable into an absolute path, e.g. "so_runner" → "/.../build/so_runner"
  std::string ResolveFromExecutable(const char* relative_path);


}  // namespace app_config

#endif  // APP_CONFIG_HPP