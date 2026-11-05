// End-to-end tests for src/runner/so_runner.cpp and plugins/test_plugin.cpp.
// so_runner is started as a real child process (through ManagedProcess) and its exit code and log are checked.

// ManagedProcess, ProcessSpec
#include "manager/managed_process.hpp"

// GoogleTest
#include <gtest/gtest.h>

// std::chrono
#include <chrono>
// std::filesystem
#include <filesystem>
// std::string
#include <string>
// std::vector
#include <vector>

// TempDir, WriteFile, ReadFile, WaitUntil, EnsureLogInit
#include "test_helpers.hpp"

// Anonymous namespace
namespace {

// Helpers
using namespace test_helpers;
// 3s etc.
using namespace std::chrono_literals;

// Fixture: run so_runner with arguments, output to <tmp>/runner.log
class SoRunnerTest : public ::testing::Test {
protected:
    // Before each test
    void SetUp() override { EnsureLogInit(); }

    // Start so_runner with args; returns false if it could not be started
    bool Launch(const std::vector<std::string>& args) {
        // The spec
        ProcessSpec spec;
        // The instrumented so_runner
        spec.executable = SO_RUNNER_PATH;
        // Its arguments
        spec.args = args;
        // Log file
        spec.log_path = Log();
        // Start
        return proc_.Start(spec);
    }

    // Run so_runner to completion and return its exit code (-2 if it did not finish in time)
    int RunToExit(const std::vector<std::string>& args) {
        // Could not start
        if (!Launch(args)) return -3;
        // Did not finish
        if (!proc_.WaitExit(5s)) return -2;
        // Exit code
        return proc_.Info().exit_code;
    }

    // Log file path
    std::string Log() const { return (tmp_ / "runner.log").string(); }

    // Log file content
    std::string LogText() const { return ReadFile(Log()); }

    // Wait until the log contains needle
    bool WaitForLog(const std::string& needle) {
        // Poll the file
        return WaitUntil([&] { return LogText().find(needle) != std::string::npos; }, 5000ms);
    }

    // Scratch folder
    TempDir tmp_;
    // io_context for the process
    boost::asio::io_context ctx_;
    // The so_runner process
    ManagedProcess proc_{ctx_, "runner"};
};

// No arguments: usage, exit code 2
TEST_F(SoRunnerTest, NoArgumentsPrintsUsageAndExits2) {
    // Run
    EXPECT_EQ(RunToExit({}), 2);
    // RUNNER_USAGE
    EXPECT_NE(LogText().find("[5001] [runner]"), std::string::npos) << LogText();
    // Text
    EXPECT_NE(LogText().find("usage: so_runner <library.so> [args...]"), std::string::npos) << LogText();
}

// The real catalogue is found relative to so_runner (no "unknown log id" fallbacks)
TEST_F(SoRunnerTest, CatalogIsFoundNextToRunner) {
    // Any run
    RunToExit({});
    // No 9001 (catalogue failed) and no 9002 (unknown id)
    EXPECT_EQ(LogText().find("[900"), std::string::npos) << LogText();
}

// Library that does not exist: RUNNER_LOAD_FAILED, exit 127
TEST_F(SoRunnerTest, MissingLibraryExits127) {
    // Run
    EXPECT_EQ(RunToExit({"/no/such/lib.so"}), 127);
    // 5003
    EXPECT_NE(LogText().find("cannot load /no/such/lib.so"), std::string::npos) << LogText();
}

// A file that is not a shared object: RUNNER_LOAD_FAILED, exit 127
TEST_F(SoRunnerTest, NotASharedObjectExits127) {
    // Text file named .so
    WriteFile(tmp_ / "fake.so", "this is not an ELF file\n");
    // Run
    EXPECT_EQ(RunToExit({(tmp_ / "fake.so").string()}), 127);
    // 5003
    EXPECT_NE(LogText().find("[5003]"), std::string::npos) << LogText();
}

// A real .so without plugin_main: RUNNER_NO_ENTRY, exit 127
TEST_F(SoRunnerTest, LibraryWithoutPluginMainExits127) {
    // libapp_log.so has no plugin_main
    EXPECT_EQ(RunToExit({APP_LOG_LIB_PATH}), 127);
    // 5004
    EXPECT_NE(LogText().find("has no plugin_main()"), std::string::npos) << LogText();
}

// The plugin's return value becomes so_runner's exit code
class PluginExitCodeTest : public SoRunnerTest, public ::testing::WithParamInterface<int> {};

// Each code
TEST_P(PluginExitCodeTest, ReturnValueIsExitCode) {
    // The fixture plugin returns argv[1]
    EXPECT_EQ(RunToExit({EXIT_CODE_PLUGIN_PATH, std::to_string(GetParam())}), GetParam());
    // RUNNER_FINISHED with that code
    EXPECT_NE(LogText().find("returned " + std::to_string(GetParam())), std::string::npos) << LogText();
}

// Representative codes
INSTANTIATE_TEST_SUITE_P(Codes, PluginExitCodeTest, ::testing::Values(0, 1, 42, 200));

// The plugin sees argv[0] = path of the .so, argv[1..] = the user's arguments (shifted by one)
TEST_F(SoRunnerTest, PluginSeesShiftedArgv) {
    // Two user arguments
    ASSERT_EQ(RunToExit({EXIT_CODE_PLUGIN_PATH, "0", "hello world"}), 0);
    // Log text
    const std::string text = LogText();
    // Three entries
    EXPECT_NE(text.find("argc=3\n"), std::string::npos) << text;
    // argv[0] is the .so
    EXPECT_NE(text.find(std::string("argv[0]=") + EXIT_CODE_PLUGIN_PATH + "\n"), std::string::npos) << text;
    // argv[1]
    EXPECT_NE(text.find("argv[1]=0\n"), std::string::npos) << text;
    // argv[2] keeps its space
    EXPECT_NE(text.find("argv[2]=hello world\n"), std::string::npos) << text;
    // RUNNER_LOADING lists the arguments
    EXPECT_NE(text.find(std::string("loading ") + EXIT_CODE_PLUGIN_PATH + " 0 hello world"), std::string::npos)
        << text;
}

// RUNNER_LOADING is the first line of the run and carries the child's pid
TEST_F(SoRunnerTest, LoadingLineIsFirstAndHasPid) {
    // Run
    ASSERT_EQ(RunToExit({EXIT_CODE_PLUGIN_PATH, "0"}), 0);
    // First line
    const std::string first = LogText().substr(0, LogText().find('\n'));
    // 5002
    EXPECT_NE(first.find("[5002]"), std::string::npos) << first;
    // pid of the child
    EXPECT_NE(first.find("pid " + std::to_string(proc_.Info().pid) + ": loading"), std::string::npos) << first;
}

// libtest_plugin.so: ticks, then a graceful SIGTERM stop with exit code 0
TEST_F(SoRunnerTest, TestPluginTicksAndStopsGracefully) {
    // 50 ms interval
    ASSERT_TRUE(Launch({TEST_PLUGIN_PATH, "0.05"}));
    // A few ticks
    ASSERT_TRUE(WaitForLog("tick 3")) << LogText();
    // SIGTERM
    EXPECT_EQ(proc_.Stop(), 0);
    // Log text
    const std::string text = LogText();
    // TEST_STARTED with the formatted interval
    EXPECT_NE(text.find("interval 0.05 s"), std::string::npos) << text;
    // TEST_STOPPED
    EXPECT_NE(text.find("SIGTERM received after"), std::string::npos) << text;
    // RUNNER_FINISHED
    EXPECT_NE(text.find("returned 0"), std::string::npos) << text;
}

// libtest_plugin.so without arguments defaults to a 1 s interval
TEST_F(SoRunnerTest, TestPluginDefaultIntervalIsOneSecond) {
    // No interval argument
    ASSERT_TRUE(Launch({TEST_PLUGIN_PATH}));
    // Start-up line
    ASSERT_TRUE(WaitForLog("interval 1.00 s")) << LogText();
    // First tick happens immediately
    ASSERT_TRUE(WaitForLog("tick 1")) << LogText();
    // Graceful stop (may take up to one interval)
    EXPECT_EQ(proc_.Stop(), 0);
}

// A paused test plugin still stops gracefully (SIGCONT before SIGTERM)
TEST_F(SoRunnerTest, PausedTestPluginStopsGracefully) {
    // Fast ticks
    ASSERT_TRUE(Launch({TEST_PLUGIN_PATH, "0.05"}));
    // Running
    ASSERT_TRUE(WaitForLog("tick 2")) << LogText();
    // Pause
    ASSERT_TRUE(proc_.Pause());
    // Stop
    EXPECT_EQ(proc_.Stop(), 0);
    // Handled SIGTERM itself
    EXPECT_NE(LogText().find("[6003]"), std::string::npos) << LogText();
}

// While paused, the plugin does not tick
TEST_F(SoRunnerTest, PausedTestPluginDoesNotTick) {
    // Fast ticks
    ASSERT_TRUE(Launch({TEST_PLUGIN_PATH, "0.05"}));
    // Running
    ASSERT_TRUE(WaitForLog("tick 2")) << LogText();
    // Pause
    ASSERT_TRUE(proc_.Pause());
    // Let any in-flight line land
    std::this_thread::sleep_for(100ms);
    // Snapshot
    const std::string before = LogText();
    // Wait while paused
    std::this_thread::sleep_for(300ms);
    // Nothing new
    EXPECT_EQ(LogText(), before);
    // Resume
    ASSERT_TRUE(proc_.Resume());
    // Ticks continue
    EXPECT_TRUE(WaitUntil([&] { return LogText().size() > before.size(); }, 2000ms));
    // Clean up
    EXPECT_EQ(proc_.Stop(), 0);
}

// Every so_runner / plugin line has the standard format "<timestamp> [level] [code] [module] [function] text"
TEST_F(SoRunnerTest, EveryLogLineHasStandardFormat) {
    // Short run
    ASSERT_TRUE(Launch({TEST_PLUGIN_PATH, "0.05"}));
    // Some output
    ASSERT_TRUE(WaitForLog("tick 2")) << LogText();
    // Stop
    proc_.Stop();
    // Text
    std::istringstream in(LogText());
    // Each line
    std::string line;
    // Number of lines checked
    int lines = 0;
    // Read all
    while (std::getline(in, line)) {
        // Starts with a date
        EXPECT_TRUE(line.size() > 24 && line[4] == '-' && line[7] == '-' && line[10] == ' ') << line;
        // Has a level
        EXPECT_NE(line.find(" [info] ["), std::string::npos) << line;
        // Counted
        ++lines;
    }
    // At least loading + started + 2 ticks + stopped + finished
    EXPECT_GE(lines, 6);
}

}  // namespace
