// Unit tests for src/manager/managed_process.cpp: one child process started with Boost.Process v2.
// Real processes (/bin/sh, /bin/sleep, small scripts) are started so the signal handling is exercised for real.

// Code under test
#include "manager/managed_process.hpp"

// GoogleTest
#include <gtest/gtest.h>

// std::chrono
#include <chrono>
// kill, SIGSTOP
#include <csignal>
// std::filesystem
#include <filesystem>
// std::string
#include <string>
// std::vector
#include <vector>

// fork, pipe, read, write, _exit, getpgid
#include <unistd.h>
// waitpid
#include <sys/wait.h>

// TempDir, WriteFile, ReadFile, WriteScript, WaitUntil, IsAlive, KernelStateOf, ElapsedMs, EnsureLogInit
#include "test_helpers.hpp"

// Anonymous namespace
namespace {

// Helpers
using namespace test_helpers;
// 100ms etc.
using namespace std::chrono_literals;

// Shell that ignores SIGTERM and then becomes `sleep` (SIG_IGN survives exec), so only SIGKILL stops it
const char* kIgnoreTerm = "trap '' TERM; exec sleep 30";
// Shell that exits with 0 on SIGTERM (a "graceful" program)
const char* kGracefulTerm = "trap 'exit 0' TERM; while :; do sleep 0.05; done";

// Fixture: logging, an io_context, a scratch folder and spec builders
class ManagedProcessTest : public ::testing::Test {
protected:
    // Before each test
    void SetUp() override { EnsureLogInit(); }

    // Spec for /bin/sh -c "<script>", output to <tmp>/out.log
    ProcessSpec Shell(const std::string& script) {
        // The spec
        ProcessSpec spec;
        // Absolute shell path (deterministic in Info().command)
        spec.executable = "/bin/sh";
        // -c "<script>"
        spec.args = {"-c", script};
        // Output captured to a file
        spec.log_path = (tmp_ / "out.log").string();
        // Return it
        return spec;
    }

    // Spec for /bin/sleep <seconds>
    ProcessSpec Sleep(const std::string& seconds = "30") {
        // The spec
        ProcessSpec spec;
        // Absolute path
        spec.executable = "/bin/sleep";
        // Duration
        spec.args = {seconds};
        // Return it
        return spec;
    }

    // Start p with spec, capturing the log output; returns Start's result
    bool StartCaptured(ManagedProcess& p, const ProcessSpec& spec) {
        // Capture stderr
        ::testing::internal::CaptureStderr();
        // Start
        const bool ok = p.Start(spec);
        // Keep the log
        log_ = ::testing::internal::GetCapturedStderr();
        // Result
        return ok;
    }

    // Shared io_context (owned by ProcessManager in the real program)
    boost::asio::io_context ctx_;
    // Scratch folder
    TempDir tmp_;
    // Last captured log output
    std::string log_;
};

// Every state has a lower-case name
TEST(ProcessStateTest, ToStringNamesEveryState) {
    // Running
    EXPECT_STREQ(ToString(ProcessState::Running), "running");
    // Paused
    EXPECT_STREQ(ToString(ProcessState::Paused), "paused");
    // Stopping
    EXPECT_STREQ(ToString(ProcessState::Stopping), "stopping");
    // Exited
    EXPECT_STREQ(ToString(ProcessState::Exited), "exited");
    // Out-of-range value falls through to "?"
    EXPECT_STREQ(ToString(static_cast<ProcessState>(99)), "?");
}

// The timing constants keep their documented values
TEST(ProcessConstantsTest, DocumentedTimeouts) {
    // SIGTERM → SIGKILL after 5 s
    EXPECT_EQ(kDefaultStopTimeout, 5000ms);
    // Wait 2 s after SIGKILL
    EXPECT_EQ(kKillWaitTimeout, 2000ms);
    // Poll every 50 ms
    EXPECT_EQ(kPollInterval, 50ms);
    // Pause/resume confirmation within 500 ms
    EXPECT_EQ(kStateChangeTimeout, 500ms);
    // Checked every 5 ms
    EXPECT_EQ(kStateCheckInterval, 5ms);
}

// A process that was never started
TEST_F(ManagedProcessTest, NeverStartedHasDefaults) {
    // Not started
    ManagedProcess p(ctx_, "idle");
    // Snapshot
    const ProcessInfo info = p.Info();
    // Name kept
    EXPECT_EQ(info.name, "idle");
    // No pid
    EXPECT_EQ(info.pid, -1);
    // Reported as exited
    EXPECT_EQ(info.state, ProcessState::Exited);
    // No exit code
    EXPECT_EQ(info.exit_code, -1);
    // No uptime
    EXPECT_EQ(info.uptime_s, 0);
    // No command
    EXPECT_EQ(info.command, "");
    // Counts as exited
    EXPECT_TRUE(p.Exited());
    // Stop returns -1
    EXPECT_EQ(p.Stop(), -1);
    // RequestStop / Kill are no-ops
    p.RequestStop();
    // No crash
    p.Kill();
    // WaitExit returns at once
    EXPECT_TRUE(p.WaitExit(0ms));
    // Name accessor
    EXPECT_EQ(p.Name(), "idle");
}

// Start a long-running process: Running, pid, command line
TEST_F(ManagedProcessTest, StartReportsRunning) {
    // The process
    ManagedProcess p(ctx_, "s");
    // Start sleep 30
    ASSERT_TRUE(StartCaptured(p, Sleep())) << log_;
    // Snapshot
    const ProcessInfo info = p.Info();
    // Real pid
    EXPECT_GT(info.pid, 0);
    // Running
    EXPECT_EQ(info.state, ProcessState::Running);
    // Executable + args
    EXPECT_EQ(info.command, "/bin/sleep 30");
    // Default working dir = manager's current dir
    EXPECT_EQ(info.working_dir, fs::current_path().string());
    // Not exited
    EXPECT_FALSE(p.Exited());
    // Clean up
    p.Stop();
}

// An executable name without '/' is looked up in PATH
TEST_F(ManagedProcessTest, BareNameIsFoundInPath) {
    // The process
    ManagedProcess p(ctx_, "path");
    // "sleep" from PATH
    ProcessSpec spec = Sleep();
    // Bare name
    spec.executable = "sleep";
    // Starts
    ASSERT_TRUE(StartCaptured(p, spec)) << log_;
    // Command shows an absolute path
    EXPECT_EQ(p.Info().command.front(), '/');
    // Ends with the name and argument
    EXPECT_NE(p.Info().command.find("sleep 30"), std::string::npos);
    // Clean up
    p.Stop();
}

// A bare name that is not in PATH fails with PROC_NOT_IN_PATH
TEST_F(ManagedProcessTest, BareNameNotInPathFails) {
    // The process
    ManagedProcess p(ctx_, "nopath");
    // Spec
    ProcessSpec spec;
    // Unlikely name
    spec.executable = "definitely_not_a_program_xyz";
    // Fails
    EXPECT_FALSE(StartCaptured(p, spec));
    // 3002
    EXPECT_NE(log_.find("[3002]"), std::string::npos) << log_;
    // Still not started
    EXPECT_EQ(p.Info().pid, -1);
}

// A path that does not exist fails with PROC_NOT_EXECUTABLE
TEST_F(ManagedProcessTest, MissingExecutableFails) {
    // The process
    ManagedProcess p(ctx_, "missing");
    // Spec
    ProcessSpec spec;
    // Nothing there
    spec.executable = "/no/such/program";
    // Fails
    EXPECT_FALSE(StartCaptured(p, spec));
    // 3003 with errno text
    EXPECT_NE(log_.find("[3003]"), std::string::npos) << log_;
    // Reason
    EXPECT_NE(log_.find("No such file or directory"), std::string::npos) << log_;
}

// A file without execute permission fails with PROC_NOT_EXECUTABLE
TEST_F(ManagedProcessTest, NonExecutableFileFails) {
    // Plain file
    WriteFile(tmp_ / "plain.sh", "echo hi\n");
    // rw-r--r--
    fs::permissions(tmp_ / "plain.sh", fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
                                           fs::perms::others_read);
    // The process
    ManagedProcess p(ctx_, "noexec");
    // Spec
    ProcessSpec spec;
    // That file
    spec.executable = (tmp_ / "plain.sh").string();
    // Fails
    EXPECT_FALSE(StartCaptured(p, spec));
    // Permission denied
    EXPECT_NE(log_.find("Permission denied"), std::string::npos) << log_;
}

// A relative executable path is made absolute (before the child changes directory)
TEST_F(ManagedProcessTest, RelativeExecutableBecomesAbsolute) {
    // Script in the scratch folder
    const fs::path script = WriteScript(tmp_ / "hello.sh", "echo hello");
    // Path relative to the current folder
    const std::string rel = fs::relative(script, fs::current_path()).string();
    // The process
    ManagedProcess p(ctx_, "rel");
    // Spec
    ProcessSpec spec;
    // Relative path
    spec.executable = rel;
    // Run it in a different folder: only works if the path was made absolute first
    spec.working_dir = "/";
    // Starts
    ASSERT_TRUE(StartCaptured(p, spec)) << log_;
    // Exits by itself
    ASSERT_TRUE(p.WaitExit(3s));
    // Exit code 0 = the script really ran
    EXPECT_EQ(p.Info().exit_code, 0);
    // Absolute in the command line
    EXPECT_EQ(p.Info().command, fs::absolute(rel).string());
}

// The log file cannot be opened → PROC_LOG_OPEN_FAILED, nothing started
TEST_F(ManagedProcessTest, UnopenableLogFails) {
    // The process
    ManagedProcess p(ctx_, "badlog");
    // Spec
    ProcessSpec spec = Sleep();
    // Folder does not exist
    spec.log_path = "/no/such/dir/x.log";
    // Fails
    EXPECT_FALSE(StartCaptured(p, spec));
    // 3004
    EXPECT_NE(log_.find("[3004]"), std::string::npos) << log_;
    // Not started
    EXPECT_EQ(p.Info().pid, -1);
}

// A working directory that does not exist makes the spawn fail → PROC_SPAWN_FAILED
TEST_F(ManagedProcessTest, MissingWorkingDirFailsToSpawn) {
    // The process
    ManagedProcess p(ctx_, "badcwd");
    // Spec
    ProcessSpec spec = Shell("true");
    // Does not exist
    spec.working_dir = (tmp_ / "nope").string();
    // Fails
    EXPECT_FALSE(StartCaptured(p, spec));
    // 3005
    EXPECT_NE(log_.find("[3005]"), std::string::npos) << log_;
}

// An executable file that is not a valid program fails to spawn
TEST_F(ManagedProcessTest, InvalidExecutableFormatFailsToSpawn) {
    // Executable bit, but neither ELF nor "#!"
    WriteFile(tmp_ / "junk", "\x01\x02\x03 not a program");
    // rwx------
    fs::permissions(tmp_ / "junk", fs::perms::owner_all);
    // The process
    ManagedProcess p(ctx_, "junk");
    // Spec
    ProcessSpec spec;
    // That file
    spec.executable = (tmp_ / "junk").string();
    // Fails (execve returns ENOEXEC)
    EXPECT_FALSE(StartCaptured(p, spec));
    // 3005
    EXPECT_NE(log_.find("[3005]"), std::string::npos) << log_;
}

// A ManagedProcess can be started only once
TEST_F(ManagedProcessTest, SecondStartFails) {
    // The process
    ManagedProcess p(ctx_, "twice");
    // First start
    ASSERT_TRUE(p.Start(Sleep()));
    // Second start
    EXPECT_FALSE(StartCaptured(p, Sleep()));
    // 3001
    EXPECT_NE(log_.find("[3001]"), std::string::npos) << log_;
    // Clean up
    p.Stop();
}

// Exit codes 0..255 are reported exactly
class ExitCodeTest : public ManagedProcessTest, public ::testing::WithParamInterface<int> {};

// "exit N" → exit_code N
TEST_P(ExitCodeTest, ExitCodeIsReported) {
    // The process
    ManagedProcess p(ctx_, "code");
    // exit N
    ASSERT_TRUE(p.Start(Shell("exit " + std::to_string(GetParam()))));
    // Exits by itself
    ASSERT_TRUE(p.WaitExit(3s));
    // Snapshot
    const ProcessInfo info = p.Info();
    // Exited
    EXPECT_EQ(info.state, ProcessState::Exited);
    // Same code
    EXPECT_EQ(info.exit_code, GetParam());
}

// A few representative codes
INSTANTIATE_TEST_SUITE_P(Codes, ExitCodeTest, ::testing::Values(0, 1, 3, 42, 127, 255));

// Killed by a signal → 128 + signal number
TEST_F(ManagedProcessTest, KilledBySignalGives128PlusSignal) {
    // The process kills itself with SIGKILL
    ManagedProcess p(ctx_, "selfkill");
    // kill -9 $$
    ASSERT_TRUE(p.Start(Shell("kill -9 $$")));
    // Exits
    ASSERT_TRUE(p.WaitExit(3s));
    // 128 + 9
    EXPECT_EQ(p.Info().exit_code, 137);
}

// stdout and stderr both go to the log file
TEST_F(ManagedProcessTest, StdoutAndStderrGoToLog) {
    // The process
    ManagedProcess p(ctx_, "out");
    // One line to each stream
    ASSERT_TRUE(p.Start(Shell("echo to-stdout; echo to-stderr 1>&2")));
    // Wait
    ASSERT_TRUE(p.WaitExit(3s));
    // Log content
    const std::string text = ReadFile(tmp_ / "out.log");
    // stdout
    EXPECT_NE(text.find("to-stdout"), std::string::npos) << text;
    // stderr
    EXPECT_NE(text.find("to-stderr"), std::string::npos) << text;
    // Path reported
    EXPECT_EQ(p.Info().log_path, (tmp_ / "out.log").string());
}

// The log is appended to, earlier runs are kept
TEST_F(ManagedProcessTest, LogIsAppended) {
    // Earlier content
    WriteFile(tmp_ / "out.log", "earlier\n");
    // The process
    ManagedProcess p(ctx_, "append");
    // One line
    ASSERT_TRUE(p.Start(Shell("echo later")));
    // Wait
    ASSERT_TRUE(p.WaitExit(3s));
    // Both lines, in order
    EXPECT_EQ(ReadFile(tmp_ / "out.log"), "earlier\nlater\n");
}

// No log path: output is discarded and the process still runs
TEST_F(ManagedProcessTest, EmptyLogPathDiscardsOutput) {
    // The process
    ManagedProcess p(ctx_, "quiet");
    // Spec without log
    ProcessSpec spec = Shell("echo nowhere");
    // Clear it
    spec.log_path.clear();
    // Starts
    ASSERT_TRUE(p.Start(spec));
    // Exits normally
    ASSERT_TRUE(p.WaitExit(3s));
    // Code 0
    EXPECT_EQ(p.Info().exit_code, 0);
    // Empty log path
    EXPECT_EQ(p.Info().log_path, "");
}

// working_dir is applied to the child
TEST_F(ManagedProcessTest, WorkingDirIsApplied) {
    // The process
    ManagedProcess p(ctx_, "cwd");
    // Print the current folder
    ProcessSpec spec = Shell("pwd");
    // The scratch folder
    spec.working_dir = tmp_.path().string();
    // Start
    ASSERT_TRUE(p.Start(spec));
    // Wait
    ASSERT_TRUE(p.WaitExit(3s));
    // pwd printed the scratch folder
    EXPECT_EQ(ReadFile(tmp_ / "out.log"), tmp_.path().string() + "\n");
    // Reported too
    EXPECT_EQ(p.Info().working_dir, tmp_.path().string());
}

// Arguments are passed verbatim: spaces, empty strings, quotes, no shell expansion
TEST_F(ManagedProcessTest, ArgumentsArePassedVerbatim) {
    // The process
    ManagedProcess p(ctx_, "args");
    // Print every argument followed by '|'
    ProcessSpec spec = Shell("printf '%s|' \"$@\"");
    // $0 = "_", then the interesting ones
    spec.args.insert(spec.args.end(), {"_", "a b", "", "$HOME", "*", "\"q\""});
    // Start
    ASSERT_TRUE(p.Start(spec));
    // Wait
    ASSERT_TRUE(p.WaitExit(3s));
    // Exactly as given
    EXPECT_EQ(ReadFile(tmp_ / "out.log"), "a b||$HOME|*|\"q\"|");
}

// stdin is /dev/null: a program reading stdin gets EOF immediately
TEST_F(ManagedProcessTest, StdinIsDevNull) {
    // The process
    ManagedProcess p(ctx_, "stdin");
    // cat would block forever on a terminal
    ASSERT_TRUE(p.Start(Shell("cat; echo done")));
    // Finishes quickly
    ASSERT_TRUE(p.WaitExit(3s));
    // Reached the echo
    EXPECT_EQ(ReadFile(tmp_ / "out.log"), "done\n");
}

// Pause → kernel state T; Resume → running again
TEST_F(ManagedProcessTest, PauseAndResume) {
    // The process
    ManagedProcess p(ctx_, "pr");
    // Long-running
    ASSERT_TRUE(p.Start(Sleep()));
    // Pause
    ASSERT_TRUE(p.Pause());
    // Kernel says stopped
    EXPECT_EQ(KernelStateOf(p.Info().pid), 'T');
    // Snapshot says paused
    EXPECT_EQ(p.Info().state, ProcessState::Paused);
    // Resume
    ASSERT_TRUE(p.Resume());
    // Kernel says not stopped
    EXPECT_NE(KernelStateOf(p.Info().pid), 'T');
    // Snapshot says running
    EXPECT_EQ(p.Info().state, ProcessState::Running);
    // Clean up
    p.Stop();
}

// Pausing twice and resuming a running process are both harmless
TEST_F(ManagedProcessTest, RepeatedPauseAndResumeAreHarmless) {
    // The process
    ManagedProcess p(ctx_, "rep");
    // Long-running
    ASSERT_TRUE(p.Start(Sleep()));
    // Resume while running: no effect, success
    EXPECT_TRUE(p.Resume());
    // First pause
    EXPECT_TRUE(p.Pause());
    // Second pause: already stopped, success
    EXPECT_TRUE(p.Pause());
    // Resume
    EXPECT_TRUE(p.Resume());
    // Running
    EXPECT_EQ(p.Info().state, ProcessState::Running);
    // Clean up
    p.Stop();
}

// A pause done by someone else (kill -STOP) is visible in Info()
TEST_F(ManagedProcessTest, ExternalStopIsDetected) {
    // The process
    ManagedProcess p(ctx_, "ext");
    // Long-running
    ASSERT_TRUE(p.Start(Sleep()));
    // Pause from outside the manager
    ::kill(p.Info().pid, SIGSTOP);
    // Eventually reported as paused
    EXPECT_TRUE(WaitUntil([&] { return p.Info().state == ProcessState::Paused; }));
    // Clean up (Stop sends SIGCONT first)
    EXPECT_EQ(p.Stop(), 143);
}

// Pause / Resume on an exited process fail with PROC_NOT_RUNNING
TEST_F(ManagedProcessTest, PauseResumeExitedFail) {
    // The process
    ManagedProcess p(ctx_, "gone");
    // Exits immediately
    ASSERT_TRUE(p.Start(Shell("true")));
    // Wait
    ASSERT_TRUE(p.WaitExit(3s));
    // Capture
    ::testing::internal::CaptureStderr();
    // Pause fails
    EXPECT_FALSE(p.Pause());
    // Resume fails
    EXPECT_FALSE(p.Resume());
    // Log text
    const std::string out = ::testing::internal::GetCapturedStderr();
    // 3006 for both
    EXPECT_NE(out.find("[3006]"), std::string::npos) << out;
}

// Stop on a program that does not handle SIGTERM → killed by SIGTERM → 143
TEST_F(ManagedProcessTest, StopSendsSigtermGiving143) {
    // The process
    ManagedProcess p(ctx_, "term");
    // sleep dies on SIGTERM
    ASSERT_TRUE(p.Start(Sleep()));
    // Start time
    const auto start = std::chrono::steady_clock::now();
    // Stop
    EXPECT_EQ(p.Stop(), 143);
    // Did not need the SIGKILL timeout
    EXPECT_LT(ElapsedMs(start), 2000);
    // Exited
    EXPECT_EQ(p.Info().state, ProcessState::Exited);
}

// Stop on a program that handles SIGTERM and exits 0 → 0
TEST_F(ManagedProcessTest, StopGracefulProgramGives0) {
    // The process
    ManagedProcess p(ctx_, "graceful");
    // trap 'exit 0' TERM
    ASSERT_TRUE(p.Start(Shell(kGracefulTerm)));
    // Give the shell time to install its trap
    std::this_thread::sleep_for(100ms);
    // Clean exit
    EXPECT_EQ(p.Stop(), 0);
}

// Stop on a paused process: SIGCONT is sent first so SIGTERM can be handled
TEST_F(ManagedProcessTest, StopPausedProcess) {
    // The process
    ManagedProcess p(ctx_, "pausedstop");
    // Graceful program
    ASSERT_TRUE(p.Start(Shell(kGracefulTerm)));
    // Let the trap be installed
    std::this_thread::sleep_for(100ms);
    // Pause it
    ASSERT_TRUE(p.Pause());
    // Start time
    const auto start = std::chrono::steady_clock::now();
    // Stop must not hang until SIGKILL
    EXPECT_EQ(p.Stop(), 0);
    // Quick
    EXPECT_LT(ElapsedMs(start), 2000);
}

// A program that ignores SIGTERM is killed after the timeout → 137
TEST_F(ManagedProcessTest, StopEscalatesToSigkill) {
    // The process
    ManagedProcess p(ctx_, "stubborn");
    // Ignores SIGTERM
    ASSERT_TRUE(p.Start(Shell(kIgnoreTerm)));
    // Let the trap be installed before exec
    std::this_thread::sleep_for(100ms);
    // Start time
    const auto start = std::chrono::steady_clock::now();
    // Short timeout
    EXPECT_EQ(p.Stop(200ms), 137);
    // Waited at least the timeout
    EXPECT_GE(ElapsedMs(start), 200);
}

// While stopping (SIGTERM sent, not yet exited) pause is refused and the state is Stopping
TEST_F(ManagedProcessTest, PauseWhileStoppingIsRefused) {
    // The process
    ManagedProcess p(ctx_, "stopping");
    // Ignores SIGTERM, so it stays in "stopping"
    ASSERT_TRUE(p.Start(Shell(kIgnoreTerm)));
    // Let the trap be installed
    std::this_thread::sleep_for(100ms);
    // Step 1 only
    p.RequestStop();
    // Reported as stopping
    EXPECT_EQ(p.Info().state, ProcessState::Stopping);
    // Capture
    ::testing::internal::CaptureStderr();
    // Refused
    EXPECT_FALSE(p.Pause());
    // Log
    const std::string out = ::testing::internal::GetCapturedStderr();
    // 3007
    EXPECT_NE(out.find("[3007]"), std::string::npos) << out;
    // Still alive after SIGTERM
    EXPECT_FALSE(p.WaitExit(100ms));
    // Step 2
    p.Kill();
    // Gone
    ASSERT_TRUE(p.WaitExit(2s));
    // SIGKILL code
    EXPECT_EQ(p.Info().exit_code, 137);
}

// WaitExit gives up after the timeout while the process keeps running
TEST_F(ManagedProcessTest, WaitExitTimesOut) {
    // The process
    ManagedProcess p(ctx_, "wait");
    // Long-running
    ASSERT_TRUE(p.Start(Sleep()));
    // Start time
    const auto start = std::chrono::steady_clock::now();
    // Not finished within 150 ms
    EXPECT_FALSE(p.WaitExit(150ms));
    // Waited at least that long
    EXPECT_GE(ElapsedMs(start), 150);
    // Still running
    EXPECT_EQ(p.Info().state, ProcessState::Running);
    // Clean up
    p.Stop();
}

// Stopping twice returns the same code; RequestStop / Kill after exit are no-ops
TEST_F(ManagedProcessTest, StopAfterExitIsIdempotent) {
    // The process
    ManagedProcess p(ctx_, "again");
    // Long-running
    ASSERT_TRUE(p.Start(Sleep()));
    // First stop
    EXPECT_EQ(p.Stop(), 143);
    // Second stop
    EXPECT_EQ(p.Stop(), 143);
    // No-op
    p.RequestStop();
    // No-op
    p.Kill();
    // Unchanged
    EXPECT_EQ(p.Info().exit_code, 143);
}

// Uptime grows while running and freezes after exit
TEST_F(ManagedProcessTest, UptimeGrowsThenFreezes) {
    // The process
    ManagedProcess p(ctx_, "uptime");
    // Runs ~0.3 s
    ASSERT_TRUE(p.Start(Sleep("0.3")));
    // First reading
    const double t1 = p.Info().uptime_s;
    // Wait a bit
    std::this_thread::sleep_for(100ms);
    // Second reading is larger
    EXPECT_GT(p.Info().uptime_s, t1);
    // Wait for the end
    ASSERT_TRUE(p.WaitExit(3s));
    // Frozen value
    const double frozen = p.Info().uptime_s;
    // About 0.3 s
    EXPECT_GE(frozen, 0.25);
    // Wait more
    std::this_thread::sleep_for(100ms);
    // Same value
    EXPECT_DOUBLE_EQ(p.Info().uptime_s, frozen);
}

// The child leads its own process group (Ctrl+C in the terminal only reaches the manager)
TEST_F(ManagedProcessTest, ChildIsOwnProcessGroupLeader) {
    // The process
    ManagedProcess p(ctx_, "pgid");
    // Long-running
    ASSERT_TRUE(p.Start(Sleep()));
    // Its pid
    const int pid = p.Info().pid;
    // Group id == pid
    EXPECT_EQ(::getpgid(pid), pid);
    // Different from ours
    EXPECT_NE(::getpgid(pid), ::getpgid(0));
    // Clean up
    p.Stop();
}

// The destructor stops a running child
TEST_F(ManagedProcessTest, DestructorStopsChild) {
    // Remember the pid
    int pid = -1;
    // Scope
    {
        // The process
        ManagedProcess p(ctx_, "dtor");
        // Long-running
        ASSERT_TRUE(p.Start(Sleep()));
        // Its pid
        pid = p.Info().pid;
        // Alive inside the scope
        ASSERT_TRUE(IsAlive(pid));
    }
    // Gone (and reaped) after the destructor
    EXPECT_FALSE(IsAlive(pid));
}

// If the manager dies unexpectedly, PR_SET_PDEATHSIG makes the kernel SIGKILL the child
TEST_F(ManagedProcessTest, ChildDiesWithManager) {
    // Pipe to receive the grandchild pid from the helper
    int fds[2];
    // Create it
    ASSERT_EQ(::pipe(fds), 0);
    // Fork a helper that plays the role of the manager
    const pid_t helper = ::fork();
    // Fork failed
    ASSERT_GE(helper, 0);
    // In the helper
    if (helper == 0) {
        // Own io_context
        boost::asio::io_context ctx;
        // Leaked on purpose: the helper "crashes" without running destructors
        ManagedProcess* p = new ManagedProcess(ctx, "orphan");
        // Start sleep 30
        ProcessSpec spec;
        // Absolute path
        spec.executable = "/bin/sleep";
        // Long
        spec.args = {"30"};
        // Start, or report -1
        const int pid = p->Start(spec) ? p->Info().pid : -1;
        // Send the pid to the test
        if (::write(fds[1], &pid, sizeof pid) != sizeof pid) ::_exit(2);
        // Die abruptly: no destructor, no Stop()
        ::_exit(0);
    }
    // Parent: close the write end
    ::close(fds[1]);
    // Grandchild pid
    int pid = -1;
    // Read it
    ASSERT_EQ(::read(fds[0], &pid, sizeof pid), static_cast<ssize_t>(sizeof pid));
    // Close the read end
    ::close(fds[0]);
    // Reap the helper
    ::waitpid(helper, nullptr, 0);
    // Started fine
    ASSERT_GT(pid, 0);
    // The kernel kills the grandchild once its parent is gone
    EXPECT_TRUE(WaitUntil([&] { return !IsAlive(pid); }, 3000ms)) << "child " << pid << " outlived its manager";
    // Safety net if the test failed
    if (IsAlive(pid)) ::kill(pid, SIGKILL);
}

}  // namespace
