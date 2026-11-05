// Unit tests for src/manager/process_manager.cpp: many named children, name rules, StopAll, Remove, Status.

// Code under test
#include "manager/process_manager.hpp"

// GoogleTest
#include <gtest/gtest.h>

// std::chrono
#include <chrono>
// std::filesystem
#include <filesystem>
// std::memory
#include <memory>
// std::string
#include <string>
// std::vector
#include <vector>

// TempDir, WaitUntil, IsAlive, ElapsedMs, EnsureLogInit
#include "test_helpers.hpp"

// Anonymous namespace
namespace {

// Helpers
using namespace test_helpers;
// 100ms etc.
using namespace std::chrono_literals;

// Count how many times `needle` occurs in `text`
size_t Count(const std::string& text, const std::string& needle) {
    // Number found
    size_t n = 0;
    // Search position
    size_t pos = 0;
    // Each occurrence
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        // Found one
        ++n;
        // Continue after it
        pos += needle.size();
    }
    // Total
    return n;
}

// Fixture: logging, a scratch folder, a manager writing logs to <tmp>/logs
class ProcessManagerTest : public ::testing::Test {
protected:
    // Before each test
    void SetUp() override {
        // Logging
        EnsureLogInit();
        // Manager under test (logs folder does not exist yet: Start must create it)
        mgr_ = std::make_unique<ProcessManager>((tmp_ / "logs").string());
    }

    // After each test: stop everything quickly (stubborn children get SIGKILL after 300 ms)
    void TearDown() override { if (mgr_) mgr_->StopAll(300ms); }

    // /bin/sleep <seconds>
    static ProcessSpec Sleep(const std::string& seconds = "30") {
        // The spec
        ProcessSpec spec;
        // Absolute path
        spec.executable = "/bin/sleep";
        // Duration
        spec.args = {seconds};
        // Return
        return spec;
    }

    // /bin/sh -c <script>
    static ProcessSpec Shell(const std::string& script) {
        // The spec
        ProcessSpec spec;
        // Shell
        spec.executable = "/bin/sh";
        // Script
        spec.args = {"-c", script};
        // Return
        return spec;
    }

    // Run fn with stderr captured, returning the captured log text
    template <typename Fn>
    std::string Logged(Fn fn) {
        // Start capture
        ::testing::internal::CaptureStderr();
        // Run
        fn();
        // Stop capture
        return ::testing::internal::GetCapturedStderr();
    }

    // Wait until the named child has exited
    bool WaitExited(const std::string& name) {
        // Poll Status
        return WaitUntil([&] {
            // Snapshot
            ProcessInfo info;
            // Exists and exited
            return mgr_->Status(name, info) && info.state == ProcessState::Exited;
        });
    }

    // Scratch folder (declared first so it outlives the manager)
    TempDir tmp_;
    // The manager
    std::unique_ptr<ProcessManager> mgr_;
};

// Name length limit documented in the header
TEST(ProcessManagerConstantsTest, MaxNameLengthIs32) {
    // 32 characters
    EXPECT_EQ(kMaxNameLength, 32u);
}

// Invalid names are rejected with MGR_INVALID_NAME and nothing is started
class InvalidNameTest : public ProcessManagerTest, public ::testing::WithParamInterface<std::string> {};

// Each bad name
TEST_P(InvalidNameTest, IsRejected) {
    // Try to start
    bool ok = true;
    // Capture the log
    const std::string out = Logged([&] { ok = mgr_->Start(GetParam(), Sleep()); });
    // Rejected
    EXPECT_FALSE(ok);
    // 2003
    EXPECT_NE(out.find("[2003]"), std::string::npos) << out;
    // Nothing added
    EXPECT_TRUE(mgr_->Names().empty());
}

// The bad names
INSTANTIATE_TEST_SUITE_P(
    // Prefix
    BadNames,
    // Fixture
    InvalidNameTest,
    // Rows
    ::testing::Values(
        // Empty
        std::string(""),
        // 33 characters (one too long)
        std::string(33, 'a'),
        // Space
        std::string("a b"),
        // Slash (would escape the log folder)
        std::string("a/b"),
        // Path traversal
        std::string("../x"),
        // Dot
        std::string("a.b"),
        // Shell character
        std::string("a$"),
        // Non-ASCII
        std::string("\xE6\x89\x8B"),
        // Tab character
        std::string("a\tb")));

// Valid names are accepted
class ValidNameTest : public ProcessManagerTest, public ::testing::WithParamInterface<std::string> {};

// Each good name
TEST_P(ValidNameTest, IsAccepted) {
    // Starts
    EXPECT_TRUE(mgr_->Start(GetParam(), Sleep()));
    // Listed
    EXPECT_EQ(mgr_->Names(), std::vector<std::string>{GetParam()});
}

// The good names
INSTANTIATE_TEST_SUITE_P(
    // Prefix
    GoodNames,
    // Fixture
    ValidNameTest,
    // Rows: one char, exactly 32, underscore, dash, digits, mixed case
    ::testing::Values(std::string("a"), std::string(32, 'z'), std::string("op_1"), std::string("OP-2"),
                      std::string("123"), std::string("MiXeD")));

// The log path defaults to <log_dir>/<name>.log and the folder is created
TEST_F(ProcessManagerTest, DefaultLogPathIsCreated) {
    // Start
    ASSERT_TRUE(mgr_->Start("t1", Shell("echo hi")));
    // Snapshot
    ProcessInfo info;
    // Found
    ASSERT_TRUE(mgr_->Status("t1", info));
    // <tmp>/logs/t1.log
    EXPECT_EQ(info.log_path, (tmp_ / "logs" / "t1.log").string());
    // Folder created
    EXPECT_TRUE(fs::is_directory(tmp_ / "logs"));
    // Output arrives in the file
    EXPECT_TRUE(WaitUntil([&] { return ReadFile(info.log_path) == "hi\n"; }));
}

// An explicit log_path in the spec is kept
TEST_F(ProcessManagerTest, ExplicitLogPathIsKept) {
    // Spec with its own log
    ProcessSpec spec = Shell("true");
    // Custom file
    spec.log_path = (tmp_ / "custom.log").string();
    // Start
    ASSERT_TRUE(mgr_->Start("c", spec));
    // Snapshot
    ProcessInfo info;
    // Found
    ASSERT_TRUE(mgr_->Status("c", info));
    // Not replaced
    EXPECT_EQ(info.log_path, spec.log_path);
}

// An empty log folder means "discard output"
TEST_F(ProcessManagerTest, EmptyLogDirMeansNoLog) {
    // Manager without logs
    ProcessManager quiet("");
    // Start
    ASSERT_TRUE(quiet.Start("q", Shell("echo hi")));
    // Snapshot
    ProcessInfo info;
    // Found
    ASSERT_TRUE(quiet.Status("q", info));
    // No log
    EXPECT_EQ(info.log_path, "");
}

// MGR_STARTED is logged with name, pid and log path
TEST_F(ProcessManagerTest, StartIsLogged) {
    // Capture
    const std::string out = Logged([&] { ASSERT_TRUE(mgr_->Start("lg", Sleep())); });
    // Snapshot
    ProcessInfo info;
    // Found
    ASSERT_TRUE(mgr_->Status("lg", info));
    // 2001 with the pid
    EXPECT_NE(out.find("[2001]"), std::string::npos) << out;
    // Text
    EXPECT_NE(out.find("started 'lg' (pid " + std::to_string(info.pid) + ")"), std::string::npos) << out;
}

// A second Start with the name of a running child is refused
TEST_F(ProcessManagerTest, DuplicateRunningNameIsRefused) {
    // First
    ASSERT_TRUE(mgr_->Start("dup", Sleep()));
    // Result of the second
    bool ok = true;
    // Capture
    const std::string out = Logged([&] { ok = mgr_->Start("dup", Sleep()); });
    // Refused
    EXPECT_FALSE(ok);
    // 2004
    EXPECT_NE(out.find("[2004]"), std::string::npos) << out;
    // Still one
    EXPECT_EQ(mgr_->Names().size(), 1u);
}

// A finished child's name can be reused: the old entry is replaced
TEST_F(ProcessManagerTest, ExitedNameIsReplaced) {
    // Short-lived
    ASSERT_TRUE(mgr_->Start("re", Shell("true")));
    // Wait for it
    ASSERT_TRUE(WaitExited("re"));
    // Old pid
    ProcessInfo old_info;
    // Snapshot
    ASSERT_TRUE(mgr_->Status("re", old_info));
    // Start again
    ASSERT_TRUE(mgr_->Start("re", Sleep()));
    // New snapshot
    ProcessInfo new_info;
    // Found
    ASSERT_TRUE(mgr_->Status("re", new_info));
    // A different process
    EXPECT_NE(new_info.pid, old_info.pid);
    // Running
    EXPECT_EQ(new_info.state, ProcessState::Running);
    // Only one entry
    EXPECT_EQ(mgr_->Names().size(), 1u);
}

// A Start that fails adds nothing
TEST_F(ProcessManagerTest, FailedStartAddsNothing) {
    // Bad executable
    ProcessSpec spec;
    // Missing
    spec.executable = "/no/such/exe";
    // Fails
    EXPECT_FALSE(mgr_->Start("bad", spec));
    // Empty
    EXPECT_TRUE(mgr_->Names().empty());
}

// Replacing an exited child with a failing Start removes the old entry (documented current behaviour)
TEST_F(ProcessManagerTest, FailedRestartDropsOldExitedEntry) {
    // Short-lived
    ASSERT_TRUE(mgr_->Start("old", Shell("true")));
    // Wait
    ASSERT_TRUE(WaitExited("old"));
    // Bad spec
    ProcessSpec spec;
    // Missing
    spec.executable = "/no/such/exe";
    // Fails
    EXPECT_FALSE(mgr_->Start("old", spec));
    // The exited entry was erased before the attempt
    EXPECT_TRUE(mgr_->Names().empty());
}

// Every operation on an unknown name fails with MGR_NOT_FOUND
TEST_F(ProcessManagerTest, UnknownNameOperationsFail) {
    // Exit code slot
    int code = 0;
    // Snapshot slot
    ProcessInfo info;
    // Capture all five
    const std::string out = Logged([&] {
        // Pause
        EXPECT_FALSE(mgr_->Pause("ghost"));
        // Resume
        EXPECT_FALSE(mgr_->Resume("ghost"));
        // Stop
        EXPECT_FALSE(mgr_->Stop("ghost", code));
        // Remove
        EXPECT_FALSE(mgr_->Remove("ghost"));
        // Status
        EXPECT_FALSE(mgr_->Status("ghost", info));
    });
    // Five 2002 lines
    EXPECT_EQ(Count(out, "[2002]"), 5u) << out;
    // Name in the text
    EXPECT_NE(out.find("no process named 'ghost'"), std::string::npos) << out;
}

// Pause / Resume by name succeed and are logged
TEST_F(ProcessManagerTest, PauseAndResumeAreLogged) {
    // Long-running
    ASSERT_TRUE(mgr_->Start("pr", Sleep()));
    // Capture
    const std::string out = Logged([&] {
        // Pause
        EXPECT_TRUE(mgr_->Pause("pr"));
        // Resume
        EXPECT_TRUE(mgr_->Resume("pr"));
    });
    // 2006
    EXPECT_NE(out.find("[2006] [manager]"), std::string::npos) << out;
    // 2007
    EXPECT_NE(out.find("[2007] [manager]"), std::string::npos) << out;
}

// Pause on an exited child fails and is not logged as paused
TEST_F(ProcessManagerTest, PauseExitedFails) {
    // Short-lived
    ASSERT_TRUE(mgr_->Start("done", Shell("true")));
    // Wait
    ASSERT_TRUE(WaitExited("done"));
    // Result
    bool ok = true;
    // Capture
    const std::string out = Logged([&] { ok = mgr_->Pause("done"); });
    // Failed
    EXPECT_FALSE(ok);
    // Not paused
    EXPECT_EQ(out.find("[2006]"), std::string::npos) << out;
    // PROC_NOT_RUNNING from ManagedProcess
    EXPECT_NE(out.find("[3006]"), std::string::npos) << out;
}

// Stop returns the exit code and logs MGR_STOPPING + MGR_EXITED
TEST_F(ProcessManagerTest, StopReturnsExitCode) {
    // Long-running
    ASSERT_TRUE(mgr_->Start("st", Sleep()));
    // Exit code
    int code = 0;
    // Capture
    const std::string out = Logged([&] { EXPECT_TRUE(mgr_->Stop("st", code)); });
    // SIGTERM
    EXPECT_EQ(code, 143);
    // 2008
    EXPECT_NE(out.find("stopping 'st'..."), std::string::npos) << out;
    // 2009
    EXPECT_NE(out.find("'st' exited with code 143"), std::string::npos) << out;
}

// Stop on an already-exited child just reports its code
TEST_F(ProcessManagerTest, StopExitedReportsItsCode) {
    // exit 5
    ASSERT_TRUE(mgr_->Start("e5", Shell("exit 5")));
    // Wait
    ASSERT_TRUE(WaitExited("e5"));
    // Exit code
    int code = 0;
    // Stop
    EXPECT_TRUE(mgr_->Stop("e5", code));
    // Original code
    EXPECT_EQ(code, 5);
}

// Stop on a child that ignores SIGTERM: SIGKILL after kDefaultStopTimeout (5 s) → 137, MGR_KILLED
TEST_F(ProcessManagerTest, StopStubbornChildIsKilledAfterDefaultTimeout) {
    // Ignores SIGTERM
    ASSERT_TRUE(mgr_->Start("stub", Shell("trap '' TERM; exec sleep 30")));
    // Let the trap be installed
    std::this_thread::sleep_for(100ms);
    // Exit code
    int code = 0;
    // Start time
    const auto start = std::chrono::steady_clock::now();
    // Capture
    const std::string out = Logged([&] { EXPECT_TRUE(mgr_->Stop("stub", code)); });
    // SIGKILL
    EXPECT_EQ(code, 137);
    // Waited the full default timeout
    EXPECT_GE(ElapsedMs(start), 5000);
    // 2012 with the timeout
    EXPECT_NE(out.find("'stub' did not exit within 5000 ms; sent SIGKILL"), std::string::npos) << out;
}

// StopAll stops every running child at the same time
TEST_F(ProcessManagerTest, StopAllStopsEveryone) {
    // Three children
    for (const char* n : {"a", "b", "c"}) ASSERT_TRUE(mgr_->Start(n, Sleep()));
    // Start time
    const auto start = std::chrono::steady_clock::now();
    // Capture
    const std::string out = Logged([&] { mgr_->StopAll(2000ms); });
    // Fast (SIGTERM is enough)
    EXPECT_LT(ElapsedMs(start), 2000);
    // 2011 with the count
    EXPECT_NE(out.find("stopping 3 running processes..."), std::string::npos) << out;
    // Three 2009 lines
    EXPECT_EQ(Count(out, "[2009]"), 3u) << out;
    // All exited with 143
    for (const ProcessInfo& p : mgr_->Status()) {
        // Exited
        EXPECT_EQ(p.state, ProcessState::Exited) << p.name;
        // SIGTERM
        EXPECT_EQ(p.exit_code, 143) << p.name;
    }
}

// StopAll shares one timeout between all stubborn children, then kills them together
TEST_F(ProcessManagerTest, StopAllSharesTimeoutAndKillsStubborn) {
    // Three stubborn children
    for (const char* n : {"s1", "s2", "s3"}) ASSERT_TRUE(mgr_->Start(n, Shell("trap '' TERM; exec sleep 30")));
    // Let the traps be installed
    std::this_thread::sleep_for(150ms);
    // Start time
    const auto start = std::chrono::steady_clock::now();
    // Capture
    const std::string out = Logged([&] { mgr_->StopAll(300ms); });
    // Elapsed
    const long long ms = ElapsedMs(start);
    // At least the timeout
    EXPECT_GE(ms, 300);
    // Shared deadline: well under 3 × 300 ms + kill waits
    EXPECT_LT(ms, 1500);
    // Three 2012 lines
    EXPECT_EQ(Count(out, "[2012]"), 3u) << out;
    // All 137
    for (const ProcessInfo& p : mgr_->Status()) EXPECT_EQ(p.exit_code, 137) << p.name;
}

// StopAll with nothing running is silent
TEST_F(ProcessManagerTest, StopAllWithNothingRunningIsSilent) {
    // Short-lived
    ASSERT_TRUE(mgr_->Start("x", Shell("true")));
    // Wait
    ASSERT_TRUE(WaitExited("x"));
    // Capture
    const std::string out = Logged([&] { mgr_->StopAll(); });
    // No 2011
    EXPECT_EQ(out.find("[2011]"), std::string::npos) << out;
}

// StopAll only counts and logs children that are still running
TEST_F(ProcessManagerTest, StopAllSkipsExitedChildren) {
    // Exited one
    ASSERT_TRUE(mgr_->Start("done", Shell("true")));
    // Wait
    ASSERT_TRUE(WaitExited("done"));
    // Running one
    ASSERT_TRUE(mgr_->Start("live", Sleep()));
    // Capture
    const std::string out = Logged([&] { mgr_->StopAll(1000ms); });
    // Only one
    EXPECT_NE(out.find("stopping 1 running processes..."), std::string::npos) << out;
    // Only "live" logged as exited
    EXPECT_EQ(Count(out, "[2009]"), 1u) << out;
}

// Remove refuses a running child
TEST_F(ProcessManagerTest, RemoveRunningIsRefused) {
    // Running
    ASSERT_TRUE(mgr_->Start("busy", Sleep()));
    // Result
    bool ok = true;
    // Capture
    const std::string out = Logged([&] { ok = mgr_->Remove("busy"); });
    // Refused
    EXPECT_FALSE(ok);
    // 2005
    EXPECT_NE(out.find("[2005]"), std::string::npos) << out;
    // Still listed
    EXPECT_EQ(mgr_->Names().size(), 1u);
}

// Remove forgets an exited child
TEST_F(ProcessManagerTest, RemoveExitedForgetsIt) {
    // Short-lived
    ASSERT_TRUE(mgr_->Start("old", Shell("true")));
    // Wait
    ASSERT_TRUE(WaitExited("old"));
    // Result
    bool ok = false;
    // Capture
    const std::string out = Logged([&] { ok = mgr_->Remove("old"); });
    // Removed
    EXPECT_TRUE(ok);
    // 2010
    EXPECT_NE(out.find("removed 'old'"), std::string::npos) << out;
    // Gone
    EXPECT_TRUE(mgr_->Names().empty());
}

// Status() and Names() are sorted by name regardless of start order
TEST_F(ProcessManagerTest, StatusAndNamesAreSorted) {
    // Start out of order
    for (const char* n : {"charlie", "alpha", "bravo"}) ASSERT_TRUE(mgr_->Start(n, Sleep()));
    // Expected order
    const std::vector<std::string> sorted = {"alpha", "bravo", "charlie"};
    // Names
    EXPECT_EQ(mgr_->Names(), sorted);
    // Status names
    std::vector<std::string> status_names;
    // Collect
    for (const ProcessInfo& p : mgr_->Status()) status_names.push_back(p.name);
    // Same order
    EXPECT_EQ(status_names, sorted);
}

// An empty manager has empty lists
TEST_F(ProcessManagerTest, EmptyManagerHasEmptyLists) {
    // No names
    EXPECT_TRUE(mgr_->Names().empty());
    // No status rows
    EXPECT_TRUE(mgr_->Status().empty());
}

// Status(name) fills every field of the snapshot
TEST_F(ProcessManagerTest, StatusFillsSnapshot) {
    // Spec with a working dir
    ProcessSpec spec = Sleep();
    // Scratch folder
    spec.working_dir = tmp_.path().string();
    // Start
    ASSERT_TRUE(mgr_->Start("snap", spec));
    // Snapshot
    ProcessInfo info;
    // Found
    ASSERT_TRUE(mgr_->Status("snap", info));
    // Name
    EXPECT_EQ(info.name, "snap");
    // Pid
    EXPECT_GT(info.pid, 0);
    // State
    EXPECT_EQ(info.state, ProcessState::Running);
    // Command
    EXPECT_EQ(info.command, "/bin/sleep 30");
    // Working dir
    EXPECT_EQ(info.working_dir, tmp_.path().string());
    // Log
    EXPECT_EQ(info.log_path, (tmp_ / "logs" / "snap.log").string());
}

// Many children can run side by side
TEST_F(ProcessManagerTest, ManyChildrenSideBySide) {
    // Twenty children
    for (int i = 0; i < 20; ++i) ASSERT_TRUE(mgr_->Start("n" + std::to_string(i), Sleep()));
    // All listed
    EXPECT_EQ(mgr_->Names().size(), 20u);
    // All running
    for (const ProcessInfo& p : mgr_->Status()) EXPECT_EQ(p.state, ProcessState::Running) << p.name;
    // Pause all
    for (const std::string& n : mgr_->Names()) EXPECT_TRUE(mgr_->Pause(n));
    // All paused
    for (const ProcessInfo& p : mgr_->Status()) EXPECT_EQ(p.state, ProcessState::Paused) << p.name;
    // StopAll works on paused children too
    mgr_->StopAll(2000ms);
    // All exited
    for (const ProcessInfo& p : mgr_->Status()) EXPECT_EQ(p.state, ProcessState::Exited) << p.name;
}

// The destructor stops every running child
TEST_F(ProcessManagerTest, DestructorStopsAllChildren) {
    // Pids to check afterwards
    std::vector<int> pids;
    // Scope
    {
        // Separate manager
        ProcessManager m((tmp_ / "logs2").string());
        // Two children
        for (const char* n : {"d1", "d2"}) ASSERT_TRUE(m.Start(n, Sleep()));
        // Remember pids
        for (const ProcessInfo& p : m.Status()) pids.push_back(p.pid);
    }
    // All gone
    for (int pid : pids) EXPECT_FALSE(IsAlive(pid)) << pid;
}

}  // namespace
