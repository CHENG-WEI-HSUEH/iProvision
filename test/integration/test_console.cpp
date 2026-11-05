// Integration tests for src/console/console.cpp: whole command sessions.
//
// Console::Run() reads std::cin and writes std::cout, so each scene feeds a script of command lines through a
// custom streambuf and captures std::cout (interface output) and stderr (APP_LOG lines).
// Child processes are the REAL so_runner loading the REAL libtest_plugin.so (both built with coverage).
//
// Script lines starting with "@sleep <ms>" are not sent to the console: the input pauses for that long,
// which lets child processes make progress between two commands.

// Code under test
#include "console/console.hpp"

// GoogleTest
#include <gtest/gtest.h>

// std::chrono
#include <chrono>
// std::filesystem
#include <filesystem>
// std::cin, std::cout
#include <iostream>
// std::ostringstream
#include <sstream>
// std::streambuf
#include <streambuf>
// std::string
#include <string>
// std::this_thread::sleep_for
#include <thread>
// std::vector
#include <vector>

// TempDir, WriteFile, ReadFile, WaitUntil, EnsureLogInit
#include "test_helpers.hpp"

// Anonymous namespace
namespace {

// Helpers
using namespace test_helpers;

// Input stream buffer that serves a script line by line, honouring "@sleep <ms>" directives
class ScriptedInput : public std::streambuf {
public:
    // Take the script
    explicit ScriptedInput(std::vector<std::string> lines) : lines_(std::move(lines)) {}

protected:
    // Called by std::getline when the current line has been consumed
    int_type underflow() override {
        // Until the script runs out
        while (next_ < lines_.size()) {
            // Next script line
            const std::string& line = lines_[next_++];
            // Directive: pause the input
            if (line.rfind("@sleep ", 0) == 0) {
                // Sleep for the given milliseconds
                std::this_thread::sleep_for(std::chrono::milliseconds(std::stoi(line.substr(7))));
                // Do not deliver this line
                continue;
            }
            // Deliver the line with its newline
            current_ = line + "\n";
            // Expose it as the get area
            setg(current_.data(), current_.data(), current_.data() + current_.size());
            // First character
            return traits_type::to_int_type(current_[0]);
        }
        // Script finished: EOF (Console::Run then stops all children and returns)
        return traits_type::eof();
    }

private:
    // The script
    std::vector<std::string> lines_;
    // Index of the next line
    size_t next_ = 0;
    // The line being read
    std::string current_;
};

// What a session produced
struct Session {
    // Console::Run's return value
    int code = -1;
    // Everything written to std::cout
    std::string out;
    // Everything written to stderr (log lines)
    std::string err;
};

// Fixture: a processes.yaml in the scratch folder pointing at the real libtest_plugin.so
class ConsoleTest : public ::testing::Test {
protected:
    // Before each test
    void SetUp() override {
        // Logging
        EnsureLogInit();
        // Programs file
        programs_ = (tmp_ / "processes.yaml").string();
        // Two programs: test (fast ticks) and slow (with a working_dir, for the listing)
        WriteFile(programs_, std::string("programs:\n") +
                                 "  test:\n    library: " + TEST_PLUGIN_PATH + "\n    args: [\"0.1\"]\n" +
                                 "  slow:\n    library: " + TEST_PLUGIN_PATH + "\n    working_dir: " +
                                 tmp_.path().string() + "\n    args: [\"0.5\"]\n");
        // Child logs
        log_dir_ = (tmp_ / "logs").string();
    }

    // Run a whole session with the given script
    Session Run(const std::vector<std::string>& script) { return RunWith(script, programs_, log_dir_); }

    // Run a session with explicit paths
    Session RunWith(const std::vector<std::string>& script, const std::string& programs, const std::string& log_dir) {
        // Scripted stdin
        ScriptedInput input(script);
        // Captured stdout
        std::ostringstream output;
        // Swap std::cin's buffer (rdbuf also clears any EOF flag left by a previous session)
        std::streambuf* old_in = std::cin.rdbuf(&input);
        // Swap std::cout's buffer
        std::streambuf* old_out = std::cout.rdbuf(output.rdbuf());
        // Capture log lines
        ::testing::internal::CaptureStderr();
        // Result
        Session s;
        // Scope: the Console (and its ProcessManager) is destroyed before the streams are restored
        {
            // Console under test, with the real so_runner
            Console console(programs, log_dir, SO_RUNNER_PATH);
            // Main loop until EOF / quit
            s.code = console.Run();
        }
        // Log lines
        s.err = ::testing::internal::GetCapturedStderr();
        // Restore std::cout
        std::cout.rdbuf(old_out);
        // Restore std::cin
        std::cin.rdbuf(old_in);
        // Interface output
        s.out = output.str();
        // Done
        return s;
    }

    // Scratch folder
    TempDir tmp_;
    // processes.yaml path
    std::string programs_;
    // Log folder
    std::string log_dir_;
};

// Count occurrences of needle in text
size_t Count(const std::string& text, const std::string& needle) {
    // Number found
    size_t n = 0;
    // Position
    for (size_t pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + needle.size())) ++n;
    // Total
    return n;
}

// No input at all: one prompt, ready / shutdown / exit messages, exit code 0
TEST_F(ConsoleTest, EmptyInputExitsCleanly) {
    // EOF immediately
    const Session s = Run({});
    // Normal exit
    EXPECT_EQ(s.code, 0);
    // One prompt then the final newline
    EXPECT_EQ(s.out, "> \n");
    // 1001 with the program count
    EXPECT_NE(s.err.find("process manager ready - 2 programs from " + programs_), std::string::npos) << s.err;
    // 1006
    EXPECT_NE(s.err.find("[1006]"), std::string::npos) << s.err;
    // 1007
    EXPECT_NE(s.err.find("[1007]"), std::string::npos) << s.err;
}

// quit / exit / q end the session; later lines are not executed
class QuitAliasTest : public ConsoleTest, public ::testing::WithParamInterface<std::string> {};

// Each alias
TEST_P(QuitAliasTest, StopsReadingCommands) {
    // Alias then a command that would print help
    const Session s = Run({GetParam(), "help"});
    // Exit 0
    EXPECT_EQ(s.code, 0);
    // Help was never printed
    EXPECT_EQ(s.out.find("start <program>"), std::string::npos) << s.out;
}

// The three aliases
INSTANTIATE_TEST_SUITE_P(Aliases, QuitAliasTest, ::testing::Values("quit", "exit", "q"));

// help and ? print the command list
class HelpAliasTest : public ConsoleTest, public ::testing::WithParamInterface<std::string> {};

// Each alias
TEST_P(HelpAliasTest, PrintsHelp) {
    // One command
    const Session s = Run({GetParam()});
    // First help line
    EXPECT_NE(s.out.find("start <program> [options...]"), std::string::npos) << s.out;
    // Last help line
    EXPECT_NE(s.out.find("Output goes to logs/<name>.log."), std::string::npos) << s.out;
}

// The two aliases
INSTANTIATE_TEST_SUITE_P(Aliases, HelpAliasTest, ::testing::Values("help", "?"));

// Blank and whitespace-only lines are ignored (just a new prompt each)
TEST_F(ConsoleTest, BlankLinesAreIgnored) {
    // Three blank-ish lines
    const Session s = Run({"", "   ", "\t"});
    // Four prompts (three lines + the one that hits EOF)
    EXPECT_EQ(Count(s.out, "> "), 4u) << s.out;
    // No warnings
    EXPECT_EQ(s.err.find("[warning]"), std::string::npos) << s.err;
}

// Unknown command followed by a word → CONSOLE_UNKNOWN_COMMAND
TEST_F(ConsoleTest, UnknownCommandWithArgumentIsReported) {
    // Unknown command + a name
    const Session s = Run({"dance x"});
    // 1003
    EXPECT_NE(s.err.find("unknown command 'dance'"), std::string::npos) << s.err;
}

// Unknown command on its own (e.g. a typo like "stauts") should also be reported as unknown.
// KNOWN DEFECT: Console::Execute checks "name is empty" before checking whether the command exists,
// so this currently logs CONSOLE_MISSING_NAME ("missing name - usage: dance <name>|all") instead.
TEST_F(ConsoleTest, UnknownCommandWithoutArgumentIsReportedAsUnknown) {
    // Typo of "status", no argument
    const Session s = Run({"stauts"});
    // Expected: 1003 unknown command
    EXPECT_NE(s.err.find("unknown command 'stauts'"), std::string::npos) << s.err;
    // And not the misleading usage hint for a command that does not exist
    EXPECT_EQ(s.err.find("usage: stauts <name>|all"), std::string::npos) << s.err;
}

// Commands that need a name report CONSOLE_MISSING_NAME without one
class MissingNameTest : public ConsoleTest, public ::testing::WithParamInterface<std::string> {};

// Each command
TEST_P(MissingNameTest, ReportsUsage) {
    // Command alone
    const Session s = Run({GetParam()});
    // 1004 naming the command
    EXPECT_NE(s.err.find("missing name - usage: " + GetParam() + " <name>|all"), std::string::npos) << s.err;
}

// Every command that takes a name
INSTANTIATE_TEST_SUITE_P(Commands, MissingNameTest,
                         ::testing::Values("pause", "resume", "run", "continue", "stop", "remove", "rm"));

// start without anything prints the start usage
TEST_F(ConsoleTest, StartWithoutNamePrintsUsage) {
    // Bare start
    const Session s = Run({"start"});
    // Usage line
    EXPECT_NE(s.out.find("usage: start <program> [options...] | start <name> <program|lib.so> [args...]"),
              std::string::npos) << s.out;
}

// programs lists every definition with library, args and working dir
TEST_F(ConsoleTest, ProgramsListsDefinitions) {
    // One command
    const Session s = Run({"programs"});
    // test row: name, library, args
    EXPECT_NE(s.out.find(std::string("  test          ") + TEST_PLUGIN_PATH + " 0.1\n"), std::string::npos) << s.out;
    // slow row shows its working dir
    EXPECT_NE(s.out.find("0.5   (cwd " + tmp_.path().string() + ")"), std::string::npos) << s.out;
}

// A missing programs file: warning at start-up, empty program list, but the console still works
TEST_F(ConsoleTest, MissingProgramsFileStillWorks) {
    // Path that does not exist
    const std::string missing = (tmp_ / "nope.yaml").string();
    // List programs, then start a .so directly
    const Session s = RunWith({"programs", std::string("start raw ") + TEST_PLUGIN_PATH + " 0.1", "status"},
                              missing, log_dir_);
    // 1002
    EXPECT_NE(s.err.find("no programs loaded from " + missing), std::string::npos) << s.err;
    // Empty listing
    EXPECT_NE(s.out.find("no programs defined (" + missing + ")"), std::string::npos) << s.out;
    // Direct .so start still works
    EXPECT_NE(s.out.find("raw"), std::string::npos) << s.out;
    // Ready message says 0 programs
    EXPECT_NE(s.err.find("ready - 0 programs"), std::string::npos) << s.err;
}

// status / ls / list with no processes print the hint
class StatusAliasTest : public ConsoleTest, public ::testing::WithParamInterface<std::string> {};

// Each alias
TEST_P(StatusAliasTest, EmptyTableHint) {
    // One command
    const Session s = Run({GetParam()});
    // Hint
    EXPECT_NE(s.out.find("no processes; use: start <program>"), std::string::npos) << s.out;
}

// The three aliases
INSTANTIATE_TEST_SUITE_P(Aliases, StatusAliasTest, ::testing::Values("status", "ls", "list"));

// An unknown program name suggests the closest one and lists all programs
TEST_F(ConsoleTest, UnknownProgramSuggestsClosest) {
    // Typo of "test"
    const Session s = Run({"start tset"});
    // 1005
    EXPECT_NE(s.err.find("'tset' is not a program in " + programs_), std::string::npos) << s.err;
    // Suggestion
    EXPECT_NE(s.out.find("did you mean 'test'?"), std::string::npos) << s.out;
    // Program list (sorted)
    EXPECT_NE(s.out.find("programs: slow test"), std::string::npos) << s.out;
    // Usage
    EXPECT_NE(s.out.find("usage: start"), std::string::npos) << s.out;
}

// A name far from every program gives no suggestion
TEST_F(ConsoleTest, UnknownProgramWithoutSuggestion) {
    // Far from everything
    const Session s = Run({"start zzzzzzzz"});
    // No suggestion
    EXPECT_EQ(s.out.find("did you mean"), std::string::npos) << s.out;
    // Still lists programs
    EXPECT_NE(s.out.find("programs: slow test"), std::string::npos) << s.out;
}

// "start test": instance named after the program; status shows it running; EOF stops it gracefully
TEST_F(ConsoleTest, StartProgramByNameLifecycle) {
    // Start, let the plugin install its SIGTERM handler, look, end
    const Session s = Run({"start test", "@sleep 300", "status"});
    // Table header
    EXPECT_NE(s.out.find("NAME          PID      STATE       UPTIME    COMMAND"), std::string::npos) << s.out;
    // Row: name, running, short command (runner and folders stripped)
    EXPECT_NE(s.out.find("running"), std::string::npos) << s.out;
    // Short command
    EXPECT_NE(s.out.find("libtest_plugin.so 0.1\n"), std::string::npos) << s.out;
    // 2001
    EXPECT_NE(s.err.find("started 'test'"), std::string::npos) << s.err;
    // The plugin handled SIGTERM at shutdown and exited 0
    EXPECT_NE(s.err.find("'test' exited with code 0"), std::string::npos) << s.err;
}

// Named instances of the same program, extra args appended after the defaults
TEST_F(ConsoleTest, NamedInstancesWithExtraArgs) {
    // Two instances
    const Session s = Run({"start op1 test", "start op2 test 7 8", "status"});
    // op1 row
    EXPECT_NE(s.out.find("op1"), std::string::npos) << s.out;
    // op2 row with appended args
    EXPECT_NE(s.out.find("libtest_plugin.so 0.1 7 8"), std::string::npos) << s.out;
    // Both stopped at the end
    EXPECT_NE(s.err.find("stopping 2 running processes..."), std::string::npos) << s.err;
}

// Words starting with '-' are options, not an instance name: "start test -x" runs program test with -x
TEST_F(ConsoleTest, OptionAfterProgramIsNotAName) {
    // Option form
    const Session s = Run({"start test -x --y", "status test"});
    // Details: name is the program name
    EXPECT_NE(s.out.find("name      test\n"), std::string::npos) << s.out;
    // Options appended
    EXPECT_NE(s.out.find(" 0.1 -x --y\n"), std::string::npos) << s.out;
}

// start <name> <path/to/lib.so> loads any .so directly
TEST_F(ConsoleTest, StartDirectSoPath) {
    // Absolute .so path
    const Session s = Run({std::string("start raw ") + TEST_PLUGIN_PATH + " 0.2", "status raw"});
    // Command shows so_runner + .so + args
    EXPECT_NE(s.out.find(std::string("command   ") + SO_RUNNER_PATH + " " + TEST_PLUGIN_PATH + " 0.2"),
              std::string::npos) << s.out;
    // Running
    EXPECT_NE(s.out.find("state     running"), std::string::npos) << s.out;
}

// A relative .so path is made absolute against the console's current folder
TEST_F(ConsoleTest, RelativeSoPathIsMadeAbsolute) {
    // Relative path to the plugin
    const std::string rel = fs::relative(TEST_PLUGIN_PATH, fs::current_path()).string();
    // Start with it
    const Session s = Run({"start rel " + rel, "status rel"});
    // Absolute in the command
    EXPECT_NE(s.out.find(" " + fs::absolute(rel).string()), std::string::npos) << s.out;
}

// A .so that does not exist: so_runner starts, fails dlopen and exits 127
TEST_F(ConsoleTest, MissingSoExitsWith127) {
    // Start, give so_runner time to fail, look
    const Session s = Run({"start bad /no/such/lib.so", "@sleep 1000", "status bad"});
    // 127 = load failed
    EXPECT_NE(s.out.find("state     exited(127)"), std::string::npos) << s.out;
    // so_runner wrote RUNNER_LOAD_FAILED into the child's log
    EXPECT_NE(ReadFile(tmp_ / "logs/bad.log").find("[5003]"), std::string::npos);
}

// pause / status / resume / status for one process
TEST_F(ConsoleTest, PauseAndResumeOne) {
    // Session
    const Session s = Run({"start t1 test", "@sleep 200", "pause t1", "status t1", "resume t1", "status t1"});
    // Paused in the first details
    const size_t paused = s.out.find("state     paused");
    // Found
    ASSERT_NE(paused, std::string::npos) << s.out;
    // Running in the second details, after the paused one
    EXPECT_NE(s.out.find("state     running", paused), std::string::npos) << s.out;
    // 2006
    EXPECT_NE(s.err.find("paused 't1'"), std::string::npos) << s.err;
    // 2007
    EXPECT_NE(s.err.find("resumed 't1'"), std::string::npos) << s.err;
}

// pause all / run all affect every process
TEST_F(ConsoleTest, PauseAllAndRunAll) {
    // Two processes
    const Session s = Run({"start a test", "start b test", "@sleep 200", "pause all", "status", "run all", "status"});
    // Both paused
    EXPECT_EQ(Count(s.err, "[2006]"), 2u) << s.err;
    // Both resumed
    EXPECT_EQ(Count(s.err, "[2007]"), 2u) << s.err;
    // The first table shows two paused rows
    EXPECT_GE(Count(s.out, "paused"), 2u) << s.out;
}

// pause all with no processes does nothing
TEST_F(ConsoleTest, PauseAllWithNothingIsQuiet) {
    // No processes
    const Session s = Run({"pause all", "resume all"});
    // No not-found errors (no targets at all)
    EXPECT_EQ(s.err.find("[2002]"), std::string::npos) << s.err;
}

// stop one, see exited(0), remove it, table empty again
TEST_F(ConsoleTest, StopThenRemove) {
    // Session
    const Session s = Run({"start t1 test", "@sleep 200", "stop t1", "status", "remove t1", "status"});
    // Graceful exit shown in the table
    EXPECT_NE(s.out.find("exited(0)"), std::string::npos) << s.out;
    // 2010
    EXPECT_NE(s.err.find("removed 't1'"), std::string::npos) << s.err;
    // Final table empty
    EXPECT_NE(s.out.find("no processes; use:"), std::string::npos) << s.out;
}

// rm is an alias of remove, and a running process cannot be removed
TEST_F(ConsoleTest, RemoveRunningIsRefused) {
    // Session
    const Session s = Run({"start t1 test", "rm t1"});
    // 2005
    EXPECT_NE(s.err.find("'t1' is still running; stop it first"), std::string::npos) << s.err;
}

// stop all stops everything and prints the table
TEST_F(ConsoleTest, StopAllPrintsTable) {
    // Two processes
    const Session s = Run({"start a test", "start b test", "@sleep 200", "stop all"});
    // Table header printed by stop all
    EXPECT_NE(s.out.find("NAME          PID"), std::string::npos) << s.out;
    // Both exited cleanly
    EXPECT_EQ(Count(s.out, "exited(0)"), 2u) << s.out;
    // Nothing left to stop at EOF (the final StopAll is silent): exactly one 2011
    EXPECT_EQ(Count(s.err, "[2011]"), 1u) << s.err;
}

// Starting a name that is already running is refused
TEST_F(ConsoleTest, DuplicateStartIsRefused) {
    // Same name twice
    const Session s = Run({"start test", "start test"});
    // 2004
    EXPECT_NE(s.err.find("'test' is already running"), std::string::npos) << s.err;
}

// status of an unknown name → MGR_NOT_FOUND
TEST_F(ConsoleTest, StatusOfUnknownName) {
    // Unknown
    const Session s = Run({"status ghost"});
    // 2002
    EXPECT_NE(s.err.find("no process named 'ghost'"), std::string::npos) << s.err;
}

// Details show the log path, and the plugin really writes ticks into it
TEST_F(ConsoleTest, DetailsShowLogAndPluginWritesIt) {
    // Session
    const Session s = Run({"start t1 test", "@sleep 400", "status t1"});
    // Log path in the details
    const std::string log = (tmp_ / "logs" / "t1.log").string();
    // Shown
    EXPECT_NE(s.out.find("log       " + log), std::string::npos) << s.out;
    // Plugin output (TEST_TICK via so_runner's glog to stderr → log file)
    const std::string text = ReadFile(log);
    // Ticks
    EXPECT_NE(text.find("[6002]"), std::string::npos) << text;
    // Graceful stop at the end
    EXPECT_NE(text.find("[6003]"), std::string::npos) << text;
}

// Without a log folder the details show "-"
TEST_F(ConsoleTest, DetailsShowDashWithoutLogDir) {
    // Empty log dir
    const Session s = RunWith({"start t1 test", "status t1"}, programs_, "");
    // Dash
    EXPECT_NE(s.out.find("log       -\n"), std::string::npos) << s.out;
}

// A program with working_dir runs there
TEST_F(ConsoleTest, ProgramWorkingDirIsUsed) {
    // slow has working_dir = tmp
    const Session s = Run({"start slow", "status slow"});
    // cwd line
    EXPECT_NE(s.out.find("cwd       " + tmp_.path().string() + "\n"), std::string::npos) << s.out;
}

// A very long command is cut to 100 characters with "..."
TEST_F(ConsoleTest, LongCommandIsTruncated) {
    // Start with many long arguments
    std::string cmd = "start long test";
    // 20 × 12 characters
    for (int i = 0; i < 20; ++i) cmd += " argument" + std::to_string(100 + i);
    // Session
    const Session s = Run({cmd, "status"});
    // The row for "long"
    const size_t row = s.out.find("\nlong");
    // Found
    ASSERT_NE(row, std::string::npos) << s.out;
    // The row's text
    const std::string line = s.out.substr(row + 1, s.out.find('\n', row + 1) - row - 1);
    // Ends with "..."
    EXPECT_EQ(line.substr(line.size() - 3), "...") << line;
    // Command column (after the 45 fixed columns) is exactly 100 characters
    EXPECT_EQ(line.size() - 45, 100u) << line;
}

// An invalid instance name is rejected by the manager
TEST_F(ConsoleTest, InvalidInstanceNameIsRejected) {
    // Slash in the name
    const Session s = Run({"start a/b test"});
    // 2003
    EXPECT_NE(s.err.find("invalid name 'a/b'"), std::string::npos) << s.err;
}

// Quoted arguments reach the plugin as single arguments
TEST_F(ConsoleTest, QuotedArgumentIsKeptTogether) {
    // Quoted argument with a space
    const Session s = Run({"start q test \"two words\"", "status q"});
    // Shown in the command line
    EXPECT_NE(s.out.find(" 0.1 two words\n"), std::string::npos) << s.out;
}

}  // namespace
