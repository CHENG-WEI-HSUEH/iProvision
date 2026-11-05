// iprovision (iProvision): A general process manager for any plugins.
// Each child process is the so_runner executable, which loads (dlopen) the specified .so and calls plugin_main().
// The command handling is in console/console.cpp; this file just assembles and starts it.



// std::string
#include <string>


// app_config.hpp: fixed settings (programs definition file, child process log directory, child process host executable path, and log file path) and ResolveFromExecutable() to convert relative paths to absolute paths)
// app_config::ResolveFromExecutable(): convert relative path to absolute path
// app_config::kProgramsFile, etc.: fixed settings and ResolveFromExecutable()
#include "app_config.hpp"
// Console: users command line interface (read commands, start/pause/resume/stop child processes, show status)
#include "console/console.hpp"
// InstallShutdownSignals aims to handle Ctrl+C, kill, and terminal close signals to cleanly stop child processes
#include "console/shutdown_signal.hpp"
// app_log::Init: load configs/log_messages.yaml and start glog
#include "app_log.hpp"

// Program Entry Point
int main() {
    // Start logging first, so every later step can log: terminal + logs/manager.log (see log_messages.yaml settings)
    app_log::Init(app_config::ResolveFromExecutable(app_config::kLogCatalogFile), app_config::kManagerName,
                  app_log::Output::ConsoleAndFile);

    // Setting Ctrl+C（SIGINT）、kill（SIGTERM）、turn off terminal（SIGHUP）
    InstallShutdownSignals();


    // Loaded process definitions from this file are used to start child processes.
    const std::string programs_path = app_config::ResolveFromExecutable(app_config::kProgramsFile);
    // LOaded child process logs are stored in this directory.
    const std::string log_dir = app_config::ResolveFromExecutable(app_config::kLogDir);
    // so_runner is the host executable for child processes, which loads the specified .so and calls plugin_main().
    const std::string runner = app_config::ResolveFromExecutable(app_config::kRunnerFile);


    // User command line interface (reads processes.yaml and creates ProcessManager)
    Console console(programs_path, log_dir, runner);

    // Once Ctrl+C / quit is received, stop all child processes before exiting
    return console.Run();
}
