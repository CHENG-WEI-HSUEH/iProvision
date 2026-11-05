#ifndef CONSOLE_HPP
#define CONSOLE_HPP

// std::string
#include <string>
// std::vector
#include <vector>

// ProcessManager aims to start/stop/pause/resume child processes, and query their status
#include "manager/process_manager.hpp"
// ProgramConfig aims to load processes.yaml and provide the program definitions to ProcessManager
#include "manager/program_config.hpp"


// User command line interface:
//  start <program> [options...]          start a program from processes.yaml, e.g. start isurgical --show
//  start <name> <program> [args...]      another instance with a nickname, e.g. start op1 isurgical
//  start <name> <path/to/lib.so> [args]  load any .
//  programs                              list the programs in processes.yaml
//  pause <name>|all                      pause (SIGSTOP)
//  resume <name>|all  | run              continue a paused process (SIGCONT)
//  stop <name>|all                       SIGTERM, then SIGKILL after 5 s
//  status [name]      | ls               all processes, or one in detail
//  remove <name>                         remove a finished process from the list
//  help, quit
class Console {
public:

    // programs_path：processes.yaml；log_dir：child process log directory；runner：so_runner path
    Console(std::string programs_path, std::string log_dir, std::string runner);

    // Main loop: read commands and execute them until quit, EOF, or Ctrl+C; stop all child processes before exiting. Returns the program exit code.
    int Run();

private:
    // Execute a command line (tokens); return false if the user wants to exit
    bool Execute(const std::vector<std::string>& words);


    // Start command: determine the loaded .so and arguments 
    void CmdStart(const std::vector<std::string>& words);
    // Pause command (name can be "all")
    void CmdPause(const std::string& name);
    // Resume / run command (name can be "all")
    void CmdResume(const std::string& name);
    // Stop command (name can be "all")
    void CmdStop(const std::string& name);
    // Status command (name can be "all")
    void CmdStatus(const std::string& name);
    // Remove command: remove a finished process from process manager (name cannot be "all")
    void CmdRemove(const std::string& name);

    // Name is "all" returns all process names; otherwise returns just the specified name
    std::vector<std::string> Targets(const std::string& name) const;

    // Print a table of all processes (sorted by name)
    void PrintTable() const;
    // Print the details of the process
    void PrintDetails(const ProcessInfo& info) const;
    // Print the list of programs in processes.yaml
    void PrintPrograms() const;
    // Hint for the user when the program name is not found in processes.yaml
    void PrintUnknownProgram(const std::string& name) const;
    // Print help information for all commands
    static void PrintHelp();
    // Print usage information for the start command
    static void PrintStartUsage();
    // If the command is too long, truncate it and add "..."
    std::string ShortCommand(const std::string& command) const;

    // processes.yaml path
    std::string programs_path_;
    // absolute so_runner path
    std::string runner_;
    // Program definitions loaded from processes.yaml
    ProgramTable programs_;
    // Process manager. mutable: Status() will also reap finished child processes (change internal state),
    // but for Console this is just "query", so let const Print functions also call it
    mutable ProcessManager manager_;
};

#endif  // CONSOLE_HPP