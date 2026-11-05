// Unit tests for src/console/shutdown_signal.cpp: InstallShutdownSignals / ShutdownRequested.
//
// Signals change process-wide state, so every scene that installs handlers or raises a signal runs inside a
// death test (a fresh child process). The parent gtest process is never signalled.

// Code under test
#include "console/shutdown_signal.hpp"

// GoogleTest
#include <gtest/gtest.h>

// raise, SIGINT, SIGTERM, SIGHUP
#include <csignal>
// std::exit
#include <cstdlib>
// std::string
#include <string>

// Anonymous namespace
namespace {

// Every signal the manager treats as "shut down"
class ShutdownSignalDeathTest : public ::testing::TestWithParam<int> {
protected:
    // Re-execute the binary for each death test: safest style when the parent has threads or global state
    void SetUp() override { GTEST_FLAG_SET(death_test_style, "threadsafe"); }
};

// Without any signal the flag is false (checked in the parent; no signal has ever been raised here)
TEST(ShutdownSignalTest, NotRequestedInitially) {
    // Fresh process state
    EXPECT_FALSE(ShutdownRequested());
}

// Installing handlers alone does not set the flag
TEST(ShutdownSignalTest, InstallDoesNotSetFlag) {
    // Death-test style
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    // In a child: install, check the flag, exit 0 if still false
    EXPECT_EXIT(
        {
            // Install the three handlers
            InstallShutdownSignals();
            // 0 = flag still false (expected), 1 = wrongly set
            std::exit(ShutdownRequested() ? 1 : 0);
        },
        // Child must exit normally with code 0
        ::testing::ExitedWithCode(0),
        // No stderr expectation
        "");
}

// The first signal only sets the flag; the process keeps running
TEST_P(ShutdownSignalDeathTest, FirstSignalSetsFlagAndProcessSurvives) {
    // The signal of this row
    const int sig = GetParam();
    // In a child
    EXPECT_EXIT(
        {
            // Install
            InstallShutdownSignals();
            // Send the signal to ourselves (handler runs synchronously before raise returns)
            std::raise(sig);
            // Survived; 0 = flag set (expected)
            std::exit(ShutdownRequested() ? 0 : 1);
        },
        // Normal exit with 0
        ::testing::ExitedWithCode(0),
        // No stderr expectation
        "");
}

// SA_RESETHAND: the second identical signal uses the default action, which terminates the process
TEST_P(ShutdownSignalDeathTest, SecondSignalUsesDefaultActionAndKills) {
    // The signal of this row
    const int sig = GetParam();
    // In a child
    EXPECT_EXIT(
        {
            // Install
            InstallShutdownSignals();
            // First: handled
            std::raise(sig);
            // Second: default action (terminate)
            std::raise(sig);
            // Must not get here
            std::exit(0);
        },
        // Killed by that very signal
        ::testing::KilledBySignal(sig),
        // No stderr expectation
        "");
}

// Run the two scenes above for SIGINT (Ctrl+C), SIGTERM (kill) and SIGHUP (terminal closed)
INSTANTIATE_TEST_SUITE_P(
    // Prefix
    Signals,
    // Fixture
    ShutdownSignalDeathTest,
    // Rows
    ::testing::Values(SIGINT, SIGTERM, SIGHUP),
    // Readable names
    [](const ::testing::TestParamInfo<int>& info) {
        // Map number → name
        switch (info.param) {
            // Ctrl+C
            case SIGINT: return std::string("SIGINT");
            // kill
            case SIGTERM: return std::string("SIGTERM");
            // terminal closed
            default: return std::string("SIGHUP");
        }
    });

// Each signal has its own one-shot handler: SIGINT then SIGTERM are both handled
TEST(ShutdownSignalTest, DifferentSignalsAreEachHandledOnce) {
    // Death-test style
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    // In a child
    EXPECT_EXIT(
        {
            // Install
            InstallShutdownSignals();
            // First SIGINT: handled
            std::raise(SIGINT);
            // First SIGTERM: also handled (its own handler was not reset by SIGINT)
            std::raise(SIGTERM);
            // First SIGHUP: handled too
            std::raise(SIGHUP);
            // Survived all three
            std::exit(ShutdownRequested() ? 0 : 1);
        },
        // Normal exit
        ::testing::ExitedWithCode(0),
        // No stderr expectation
        "");
}

// Baseline: without InstallShutdownSignals, SIGTERM kills immediately
TEST(ShutdownSignalTest, WithoutInstallSignalKills) {
    // Death-test style
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    // In a child
    EXPECT_EXIT(
        {
            // Default action
            std::raise(SIGTERM);
            // Must not get here
            std::exit(0);
        },
        // Killed by SIGTERM
        ::testing::KilledBySignal(SIGTERM),
        // No stderr expectation
        "");
}

// Installing twice is harmless: the handler is simply registered again
TEST(ShutdownSignalTest, InstallTwiceStillHandlesSignal) {
    // Death-test style
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    // In a child
    EXPECT_EXIT(
        {
            // First install
            InstallShutdownSignals();
            // Second install
            InstallShutdownSignals();
            // Handled
            std::raise(SIGINT);
            // Flag set
            std::exit(ShutdownRequested() ? 0 : 1);
        },
        // Normal exit
        ::testing::ExitedWithCode(0),
        // No stderr expectation
        "");
}

}  // namespace
