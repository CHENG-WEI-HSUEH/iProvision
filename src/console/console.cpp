#include "console.hpp"


// std::filesystem::absolute（Convert relative path to absolute path）
#include <filesystem>
// std::setw、std::left
#include <iomanip>
// std::cout、std::cin
#include <iostream>
// std::move
#include <utility>

// ShutdownRequested：detect Ctrl+C
#include "shutdown_signal.hpp"
// Tokenize、Duration、ClosestWord
#include "text_utils.hpp"
// APP_LOG: write a line from configs/log_messages.yaml (CONSOLE_*)
#include "app_log.hpp"


namespace {
  // Status table column widths (in byte)
  constexpr int kNameWidth = 14;
  // PID width
  constexpr int kPidWidth = 9;
  // STATE width
  constexpr int kStateWidth = 12;
  // UPTIME width
  constexpr int kUptimeWidth = 10;
  // The maximum width of the COMMAND column; if the command is longer, it will be truncated.
  constexpr std::size_t kMaxCommandWidth = 100;
  // The hint for typos in program names: the maximum number of character differences to consider a name "close".
  constexpr std::size_t kMaxTypoDistance = 2;


  // Print the process state as a string.
  std::string StateText(const ProcessInfo& p) {
      // Exited: show the exit code
      if (p.state == ProcessState::Exited) return "exited(" + std::to_string(p.exit_code) + ")";
      // Other status：running / paused / stopping
      return ToString(p.state);
  }

  // Returns true if cmd matches any of the names in the list (start, pause, resume, stop, status).
  bool IsCommand(const std::string& cmd, const std::vector<std::string>& names) {
      for (const std::string& n : names) {
          if (cmd == n) return true;
      }
      return false;
  }

} 

// Constructor: record the configuration file path and so_runner path, and create a process manager (logs written to log_dir)
Console::Console(std::string programs_path, std::string log_dir, std::string runner)
    : programs_path_(std::move(programs_path)), runner_(std::move(runner)), manager_(std::move(log_dir)) {
    // If loading fails (LoadPrograms logs the reason), only warn; the 'start <name> <lib.so>' command can still be used
    if (!LoadPrograms(programs_path_, programs_)) {
        // Log a warning message
        APP_LOG("CONSOLE_PROGRAMS_UNAVAILABLE", {{"path", programs_path_}});
    }
}

// Main loop: read commands and execute them until 
// quit, EOF, or Ctrl+C; stop all child processes before exiting. 
// Returns the program exit code.
int Console::Run() {
    // Welcome message: how many programs were loaded and where the configuration file is
    APP_LOG("CONSOLE_READY", {{"count", std::to_string(programs_.size())}, {"path", programs_path_}});

    // The line of text input by the user
    std::string line;
    // Main loop: until Ctrl+C is received
    while (!ShutdownRequested()) {
        // Print prompt; flush to display immediately
        std::cout << "> " << std::flush;
        // Read a line of input; break if reading fails (EOF or interrupted by Ctrl+C)
        if (!std::getline(std::cin, line)) break;
        // Tokenize: split the input line into multiple words
        const std::vector<std::string> words = Tokenize(line);
        // Empty line: do nothing
        if (words.empty()) continue;
        // Execute command; return false if user entered quit
        if (!Execute(words)) break;
    }

    // Exit the main loop: end the prompt line, then log that all child processes are being stopped
    std::cout << std::endl;
    // Log the shutdown
    APP_LOG("CONSOLE_SHUTDOWN");
    // Stop all child processes (SIGTERM, then SIGKILL after 5 seconds)
    manager_.StopAll();
    // Log the exit
    APP_LOG("CONSOLE_EXIT");
    // Normal exit
    return 0;
}

// Execute a single command
bool Console::Execute(const std::vector<std::string>& words) {
    // The first word is the command, e.g., start, pause
    const std::string& cmd = words[0];
    // The second word is usually the process name (empty string if not specified)
    const std::string name = words.size() > 1 ? words[1] : "";

    // quit / exit / q： exit the program
    if (IsCommand(cmd, {"quit", "exit", "q"})) return false;
    // help / ?： print help information
    if (IsCommand(cmd, {"help", "?"})) { PrintHelp(); return true; }
    // programs：print the list of programs in processes.yaml
    if (cmd == "programs") { PrintPrograms(); return true; }
    // status / ls / list：view status (name can be omitted)
    if (IsCommand(cmd, {"status", "ls", "list"})) { CmdStatus(name); return true; }

    // The following commands require a name (or "all")
    // If no name is provided, print usage information and wait for the next command
    if (name.empty()) {
        // If the command is "start", print the usage for the start command.
        if (cmd == "start") PrintStartUsage();
        // Other command: <cmd> <name>|all
        else APP_LOG("CONSOLE_MISSING_NAME", {{"command", cmd}});
        // Wait for the next command
        return true;
    }

    // Assign the command to the corresponding function
    // start：start a child process
    if (cmd == "start") CmdStart(words);
    // pause：pause a child process (name can be "all")
    else if (cmd == "pause") CmdPause(name);
    // resume / run / continue：resume a paused child process (name can be "all")
    else if (IsCommand(cmd, {"resume", "run", "continue"})) CmdResume(name);
    // stop：stop a child process (name can be "all")
    else if (cmd == "stop") CmdStop(name);
    // remove / rm：remove a completed child process (name can be "all")
    else if (IsCommand(cmd, {"remove", "rm"})) CmdRemove(name);
    // unknown command
    else APP_LOG("CONSOLE_UNKNOWN_COMMAND", {{"command", cmd}});
    // Wait for the next command
    return true;
}

// start command: determine the loaded .so and arguments
// Three ways to write the start command (the name is words[1]):
//   start <program> [-options...]             the instance name is the program name
//   start <name> <program|lib.so> [args...]   custom instance name (nickname)
//   start <name> <path/to/lib.so> [args]
void Console::CmdStart(const std::vector<std::string>& words) {

    // Child process name (nickname) is words[1]
    const std::string& name = words[1];
    // Named: users used the "nickname + program" format
    const bool named = words.size() > 2 && words[2][0] != '-';
    // Target is specified program name (from processes.yaml) or .so path.
    const std::string target = named ? words[2] : name;
    // The index of the first argument passed to the program (after the name and target)
    const size_t first_arg = named ? 3 : 2;

    // Load the program definition from processes.yaml, or treat it as a .so path if not found
    ProgramDef def;
    // Find the targeted program in the loaded programs; if not found, treat it as a .so path
    const ProgramTable::const_iterator it = programs_.find(target);
    // target is in the loaded programs: copy the .so path, default arguments, and working directory from the configuration file
    if (it != programs_.end()) {
        // Copy the .so path, default arguments, and working directory from the configuration file
        def = it->second;
    // Target is not in the loaded programs: treat it as a .so path (relative path is relative to the current directory)
    } else if (named) {
        // Convert to the absolute path of the .so file (relative path is relative to the current directory)
        def.library = std::filesystem::absolute(target).string();
    // Print a warning message
    } else {
        // Print out the available programs in processes.yaml
        PrintUnknownProgram(name);
        // Return without starting the process
        return;
    }

    // Insert the user-provided arguments after the default arguments
    def.args.insert(def.args.end(), words.begin() + first_arg, words.end());


    // Execute the child process
    ProcessSpec spec;
    // The main sub-process - so_runner
    spec.executable = runner_;
    // so_runner will load the .so file as the first argument
    spec.args.push_back(def.library);
    // The following arguments are for the .so file (passed to plugin_main)
    spec.args.insert(spec.args.end(), def.args.begin(), def.args.end());
    // Working directory (empty string = current directory)
    spec.working_dir = def.working_dir;

    // Start the child process; the manager logs the result (MGR_STARTED, or the reason it failed)
    manager_.Start(name, spec);
}

// pause command: pause a child process (name can be "all")
void Console::CmdPause(const std::string& name) {

    // Pause each target; the manager logs success (MGR_PAUSED) or the reason it failed
    for (const std::string& n : Targets(name)) manager_.Pause(n);
}

// resume / run 指令
void Console::CmdResume(const std::string& name) {
    // Resume each target (all = every process); the manager logs success (MGR_RESUMED) or the reason it failed
    for (const std::string& n : Targets(name)) manager_.Resume(n);
}

// stop 指令
void Console::CmdStop(const std::string& name) {
    // stop all：同時停止所有行程（比一個一個停快）
    if (name == "all") {
        // 停止全部（the manager logs MGR_STOPPING_ALL / MGR_KILLED / MGR_EXITED）：先 SIGTERM，5 秒後還沒結束的送 SIGKILL
        manager_.StopAll();
        // 印出停止後的狀態表
        PrintTable();
        // 結束
        return;
    }
    // 結束碼（0 = 正常結束，143 = 被 SIGTERM 結束，137 = 被 SIGKILL 結束）
    int code = -1;
    // Stop one process; the manager logs MGR_STOPPING / MGR_EXITED, or MGR_NOT_FOUND
    manager_.Stop(name, code);
}

// status command: view status (name can be "all")
void Console::CmdStatus(const std::string& name) {
    // No name specified: print the status table of all processes
    if (name.empty()) {
        // Print the status table
        PrintTable();
        // Return
        return;
    }
    // Process information
    ProcessInfo info;
    // If found, print the detailed information (if not, the manager logs MGR_NOT_FOUND)
    if (manager_.Status(name, info)) PrintDetails(info);
}

// remove command: remove a child process (name can be "all")
void Console::CmdRemove(const std::string& name) {
    // Remove it; the manager logs MGR_REMOVED, or why not (still running / not found)
    manager_.Remove(name);
}

// all → all process names
std::vector<std::string> Console::Targets(const std::string& name) const {
    if (name == "all") return manager_.Names();
    return {name};
}

// Status table
void Console::PrintTable() const {
    // Get the status of all processes
    const std::vector<ProcessInfo> all = manager_.Status();
    // If there are no processes, prompt the user on how to start them
    if (all.empty()) {
        // Print a message indicating no processes are running
        std::cout << "no processes; use: start <program> | start <name> <program|lib.so> [args...]\n";
        // No items to print
        return;
    }
    // Print the table header; setw sets the width of each column, left aligns the text
    std::cout << std::left << std::setw(kNameWidth) << "NAME" << std::setw(kPidWidth) << "PID"
              << std::setw(kStateWidth) << "STATE" << std::setw(kUptimeWidth) << "UPTIME" << "COMMAND\n";
    // Print a row for each process
    for (const ProcessInfo& p : all) {
        // Name, PID, state, uptime, loaded .so file and arguments
        std::cout << std::left << std::setw(kNameWidth) << p.name << std::setw(kPidWidth) << p.pid
                  << std::setw(kStateWidth) << StateText(p) << std::setw(kUptimeWidth) << Duration(p.uptime_s)
                  << ShortCommand(p.command) << '\n';
    }
}


// Abbreviate the command line
std::string Console::ShortCommand(const std::string& command) const {

    // The command line is too long, so it will be truncated to avoid wrapping in the table.
    std::string text = command;
    // Remove the so_runner path from the command line, leaving only the .so file name and its arguments.
    if (text.rfind(runner_ + " ", 0) == 0) text = text.substr(runner_.size() + 1);
    // The first space in the command line (after the .so file name)
    const size_t space = text.find(' ');
    // The last slash before the first space (to extract the .so file name)
    const size_t slash = text.rfind('/', space);
    // If there is a slash, remove the path and keep only the .so file name and its arguments.
    if (slash != std::string::npos) text = text.substr(slash + 1);
    // If the command line is still too long, truncate it and add "..." at the end.
    if (text.size() > kMaxCommandWidth) text = text.substr(0, kMaxCommandWidth - 3) + "...";
    // Return the abbreviated command line
    return text;
}

// Print the details of a single process 
// name, PID, state, uptime, full command, working directory, log file path
void Console::PrintDetails(const ProcessInfo& p) const {
    std::cout << "name      " << p.name << '\n'
              << "pid       " << p.pid << '\n'
              << "state     " << StateText(p) << '\n'
              << "uptime    " << Duration(p.uptime_s) << '\n'
              << "command   " << p.command << '\n'
              << "cwd       " << p.working_dir << '\n'
              << "log       " << (p.log_path.empty() ? "-" : p.log_path) << '\n';
}

// Print the programes in processes.yaml
void Console::PrintPrograms() const {
    // Print a message if no programs are defined in processes.yaml
    if (programs_.empty()) {
        std::cout << "no programs defined (" << programs_path_ << ")\n";
        // Finish printing
        return;
    }

    // Print the list of programs in processes.yaml
    for (const auto& [name, def] : programs_) {
        // Print the program name and the .so path (library)
        std::cout << "  " << std::left << std::setw(kNameWidth) << name << def.library;
        // Print the default arguments if specified
        for (const std::string& a : def.args) std::cout << ' ' << a;
        // Print the working directory if specified
        if (!def.working_dir.empty()) std::cout << "   (cwd " << def.working_dir << ")";
        // New line
        std::cout << '\n';
    }
}

// Print the hint when the program name is not found in processes.yaml
void Console::PrintUnknownProgram(const std::string& name) const {
    
    // Log the warning; the hints below are interface output, not log lines
    APP_LOG("CONSOLE_UNKNOWN_PROGRAM", {{"name", name}, {"path", programs_path_}});
    std::vector<std::string> names;
    // Add all program names from the loaded programs to the list
    for (const auto& [program, def] : programs_) names.push_back(program);
    // Find the closest match to the name (if any) in the list of program names
    const std::string guess = ClosestWord(names, name, kMaxTypoDistance);
    // Hint the user if a close match is found
    if (!guess.empty()) std::cout << "did you mean '" << guess << "'?\n";

    // Print out the program names in a single line, separated by spaces
    std::cout << "programs:";
    for (const std::string& n : names) std::cout << ' ' << n;
    // New line
    std::cout << '\n';
    // Print usage information for the start command
    PrintStartUsage();
}

// Print usage information for the start command
void Console::PrintStartUsage() {
    std::cout << "usage: start <program> [options...] | start <name> <program|lib.so> [args...]\n";
}

// Help message for all commands
void Console::PrintHelp() {
    // Print the usage of each command
    std::cout <<
        "  start <program> [options...]         start a program from processes.yaml, e.g. start isurgical --show\n"
        "  start <name> <program> [args...]     another instance with a nickname, e.g. start op1 isurgical\n"
        "  start <name> <lib.so> [args...]      load any .so that has plugin_main, e.g. start x ./build/libtest_plugin.so\n"
        "  programs                             list the programs in processes.yaml\n"
        "  pause <name>|all                     pause (SIGSTOP)\n"
        "  resume <name>|all  | run             continue a paused process (SIGCONT)\n"
        "  stop <name>|all                      SIGTERM, then SIGKILL after 5 s\n"
        "  status [name]      | ls              all processes, or one in detail\n"
        "  remove <name>                        forget an exited process\n"
        "  help | quit\n"
        "Each process is so_runner loading the .so. Output goes to logs/<name>.log.\n";
}
