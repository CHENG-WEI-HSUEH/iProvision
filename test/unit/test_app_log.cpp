// Unit tests for src/log/app_log.cpp (libapp_log.so): catalogue loading, line format, placeholders, levels,
// unknown ids, the manager's file sink.
//
// app_log::Init can run only once per process. So:
//   * AppLogTest            - runs in the gtest process, initialised once with the real configs/log_messages.yaml
//   * AppLogInitDeathTest   - every scene runs in a fresh child process (death test) with its own catalogue;
//                             the child checks the result itself and reports it through its exit code / stderr

// Code under test
#include "app_log.hpp"

// GoogleTest
#include <gtest/gtest.h>

// std::exit
#include <cstdlib>
// std::regex for the timestamp format
#include <regex>
// std::string
#include <string>

// TempDir, WriteFile, ReadFile, EnsureLogInit
#include "test_helpers.hpp"

// Anonymous namespace
namespace {

// Helpers from the shared header
using namespace test_helpers;

// Capture everything written to stderr (where app_log writes in StandardError mode) while running fn
template <typename Fn>
std::string CaptureLog(Fn fn) {
    // Redirect fd 2 into a temporary file
    ::testing::internal::CaptureStderr();
    // Run the code that logs
    fn();
    // Restore stderr and return what was written
    return ::testing::internal::GetCapturedStderr();
}

// ---------------------------------------------------------------------------
// In-process scenes, using the real catalogue
// ---------------------------------------------------------------------------

// Fixture: make sure the real catalogue is loaded
class AppLogTest : public ::testing::Test {
protected:
    // Before every test
    void SetUp() override { EnsureLogInit(); }
};

// A second Init is ignored and reports success, even with a bad path
TEST_F(AppLogTest, SecondInitIsIgnoredAndReturnsTrue) {
    // Already initialised by SetUp, so this returns true without reading the path
    EXPECT_TRUE(app_log::Init("/no/such/catalog.yaml", "again", app_log::Output::StandardError));
    // The real catalogue is still in use (MGR_NOT_FOUND has code 2002)
    EXPECT_NE(CaptureLog([] { APP_LOG("MGR_NOT_FOUND", {{"name", "x"}}); }).find("[2002]"), std::string::npos);
}

// A known id produces "[level] [code] [module] [function] description" with placeholders filled in
TEST_F(AppLogTest, KnownIdFillsEveryPlaceholder) {
    // Write MGR_STARTED with all three placeholders
    const std::string out = CaptureLog([] {
        // {name} {pid} {log}
        APP_LOG("MGR_STARTED", {{"name", "op1"}, {"pid", "42"}, {"log", "/tmp/op1.log"}});
    });
    // [function] is the C++ function that called APP_LOG (gtest's TestBody wraps the lambda's operator())
    EXPECT_NE(out.find("[info] [2001] [manager] ["), std::string::npos) << out;
    // Description with every value
    EXPECT_NE(out.find("] started 'op1' (pid 42), log: /tmp/op1.log"), std::string::npos) << out;
}

// Every line starts with "YYYY-MM-DD HH:MM:SS.mmm " (format and milliseconds from settings:)
TEST_F(AppLogTest, LineStartsWithTimestampWithMilliseconds) {
    // Any message
    const std::string out = CaptureLog([] { APP_LOG("CONSOLE_EXIT"); });
    // Timestamp, then the level
    EXPECT_TRUE(std::regex_search(out, std::regex(R"(^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3} \[info\])")))
        << out;
}

// Warning-level messages carry [warning]
TEST_F(AppLogTest, WarningLevelIsShown) {
    // CONSOLE_UNKNOWN_COMMAND is a warning
    const std::string out = CaptureLog([] { APP_LOG("CONSOLE_UNKNOWN_COMMAND", {{"command", "dance"}}); });
    // Level, code, module
    EXPECT_NE(out.find("[warning] [1003] [console]"), std::string::npos) << out;
    // Text
    EXPECT_NE(out.find("unknown command 'dance'"), std::string::npos) << out;
}

// Error-level messages carry [error]
TEST_F(AppLogTest, ErrorLevelIsShown) {
    // MGR_NOT_FOUND is an error
    const std::string out = CaptureLog([] { APP_LOG("MGR_NOT_FOUND", {{"name", "ghost"}}); });
    // Level, code, module
    EXPECT_NE(out.find("[error] [2002] [manager]"), std::string::npos) << out;
}

// A placeholder without a value stays as "{name}" in the text
TEST_F(AppLogTest, MissingPlaceholderIsLeftAsIs) {
    // No params
    const std::string out = CaptureLog([] { APP_LOG("MGR_NOT_FOUND"); });
    // Literal braces kept
    EXPECT_NE(out.find("no process named '{name}'"), std::string::npos) << out;
}

// Values for placeholders the description does not use are ignored
TEST_F(AppLogTest, ExtraParamsAreIgnored) {
    // "unused" does not appear in the description
    const std::string out = CaptureLog([] { APP_LOG("MGR_NOT_FOUND", {{"name", "a"}, {"unused", "zzz"}}); });
    // Normal text
    EXPECT_NE(out.find("no process named 'a'"), std::string::npos) << out;
    // The extra value is not printed
    EXPECT_EQ(out.find("zzz"), std::string::npos) << out;
}

// A value that contains its own placeholder is inserted once (no infinite loop, no re-expansion)
TEST_F(AppLogTest, ValueContainingOwnPlaceholderIsNotReExpanded) {
    // name = "{name}{name}"
    const std::string out = CaptureLog([] { APP_LOG("MGR_NOT_FOUND", {{"name", "{name}{name}"}}); });
    // Inserted literally
    EXPECT_NE(out.find("no process named '{name}{name}'"), std::string::npos) << out;
}

// Params are applied in order, so a later param can fill a placeholder that an earlier value introduced
TEST_F(AppLogTest, LaterParamCanExpandEarlierValue) {
    // MGR_EXITED: "'{name}' exited with code {code}"; name's value contains "{code}"
    const std::string out = CaptureLog([] { APP_LOG("MGR_EXITED", {{"name", "{code}"}, {"code", "7"}}); });
    // Documented current behaviour: both {code} occurrences become 7
    EXPECT_NE(out.find("'7' exited with code 7"), std::string::npos) << out;
}

// Empty values are allowed
TEST_F(AppLogTest, EmptyValueIsAllowed) {
    // name = ""
    const std::string out = CaptureLog([] { APP_LOG("MGR_NOT_FOUND", {{"name", ""}}); });
    // Quotes with nothing in between
    EXPECT_NE(out.find("no process named ''"), std::string::npos) << out;
}

// An id missing from the catalogue is logged with LOG_UNKNOWN_ID, listing every param
TEST_F(AppLogTest, UnknownIdUsesLogUnknownIdWithParams) {
    // Not in log_messages.yaml
    const std::string out = CaptureLog([] { APP_LOG("NO_SUCH_ID", {{"a", "1"}, {"b", "2"}}); });
    // LOG_UNKNOWN_ID: error, code 9002, module log
    EXPECT_NE(out.find("[error] [9002] [log]"), std::string::npos) << out;
    // Id and the params so nothing is lost
    EXPECT_NE(out.find("unknown log id 'NO_SUCH_ID' {a=1, b=2}"), std::string::npos) << out;
}

// Unknown id without params prints "{}"
TEST_F(AppLogTest, UnknownIdWithoutParamsPrintsEmptyBraces) {
    // No params
    const std::string out = CaptureLog([] { APP_LOG("ANOTHER_MISSING_ID"); });
    // Empty list
    EXPECT_NE(out.find("unknown log id 'ANOTHER_MISSING_ID' {}"), std::string::npos) << out;
}

// Write() with a null function name prints "[-]"
TEST_F(AppLogTest, NullFunctionNameIsShownAsDash) {
    // Call Write directly instead of the macro
    const std::string out = CaptureLog([] { app_log::Write("CONSOLE_EXIT", nullptr); });
    // Placeholder for the function
    EXPECT_NE(out.find("[console] [-] process manager exited"), std::string::npos) << out;
}

// Write() with an explicit function name prints it
TEST_F(AppLogTest, ExplicitFunctionNameIsShown) {
    // Function name "MyFunction"
    const std::string out = CaptureLog([] { app_log::Write("CONSOLE_EXIT", "MyFunction"); });
    // Shown between brackets
    EXPECT_NE(out.find("[console] [MyFunction] process manager exited"), std::string::npos) << out;
}

// Every message id used in the code base exists in the real catalogue (none falls back to 9002)
TEST_F(AppLogTest, AllIdsUsedBySourcesExistInCatalog) {
    // Ids referenced by src/, plugins/ (kept in sync with the sources by hand)
    const char* ids[] = {"CONSOLE_READY", "CONSOLE_PROGRAMS_UNAVAILABLE", "CONSOLE_UNKNOWN_COMMAND",
                         "CONSOLE_MISSING_NAME", "CONSOLE_UNKNOWN_PROGRAM", "CONSOLE_SHUTDOWN", "CONSOLE_EXIT",
                         "MGR_STARTED", "MGR_NOT_FOUND", "MGR_INVALID_NAME", "MGR_ALREADY_RUNNING",
                         "MGR_STILL_RUNNING", "MGR_PAUSED", "MGR_RESUMED", "MGR_STOPPING", "MGR_EXITED",
                         "MGR_REMOVED", "MGR_STOPPING_ALL", "MGR_KILLED", "PROC_ALREADY_STARTED",
                         "PROC_NOT_IN_PATH", "PROC_NOT_EXECUTABLE", "PROC_LOG_OPEN_FAILED", "PROC_SPAWN_FAILED",
                         "PROC_NOT_RUNNING", "PROC_IS_STOPPING", "PROC_SIGNAL_FAILED", "PROC_STATE_TIMEOUT",
                         "CFG_LOADED", "CFG_PARSE_FAILED", "CFG_NO_PROGRAMS", "CFG_NO_LIBRARY", "RUNNER_USAGE",
                         "RUNNER_LOADING", "RUNNER_LOAD_FAILED", "RUNNER_NO_ENTRY", "RUNNER_FINISHED",
                         "TEST_STARTED", "TEST_TICK", "TEST_STOPPED", "ISURGICAL_BAD_ARGS", "ISURGICAL_STARTING",
                         "ISURGICAL_FINISHED"};
    // Check each one
    for (const char* id : ids) {
        // Write it
        const std::string out = CaptureLog([id] { app_log::Write(id, "check"); });
        // Must not be reported as unknown
        EXPECT_EQ(out.find("unknown log id"), std::string::npos) << id << " is missing from log_messages.yaml";
    }
}

// ---------------------------------------------------------------------------
// Fresh-process scenes: Init with custom catalogues
// ---------------------------------------------------------------------------

// Fixture for the death tests: a scratch folder for catalogues and log files
class AppLogInitDeathTest : public ::testing::Test {
protected:
    // Re-exec style so each child starts with a clean (uninitialised) app_log
    void SetUp() override { GTEST_FLAG_SET(death_test_style, "threadsafe"); }
    // Write a catalogue file and return its path
    std::string Catalog(const std::string& text) {
        // catalog.yaml inside the scratch folder
        const fs::path path = tmp_ / "catalog.yaml";
        // Write it
        WriteFile(path, text);
        // Return as string
        return path.string();
    }
    // Scratch folder (the child process re-creates the same object, but writes the same content)
    TempDir tmp_;
};

// A minimal valid messages: block used by several scenes
const char* kMinimalMessages =
    // Map of messages
    "messages:\n"
    // An info message with a repeated placeholder
    "  T_INFO:\n    code: 7001\n    level: info\n    module: unit\n    description: \"info {x}-{x}\"\n"
    // A warning
    "  T_WARN:\n    code: 7002\n    level: warning\n    module: unit\n    description: \"warn line\"\n"
    // An error
    "  T_ERR:\n    code: 7003\n    level: error\n    module: unit\n    description: \"error line\"\n"
    // A message overriding [function]
    "  T_FUNC:\n    code: 7004\n    level: info\n    module: unit\n    function: custom_fn\n    description: \"f\"\n"
    // A message with only a description (every other field defaults)
    "  T_BARE:\n    description: \"bare message\"\n";

// The catalogue file does not exist: Init returns false, logs the built-in 9001 message
TEST_F(AppLogInitDeathTest, MissingCatalogFallsBackToBuiltInDefaults) {
    // In a child
    EXPECT_EXIT(
        {
            // Load a missing file
            const bool ok = app_log::Init("/no/such/catalog.yaml", "t", app_log::Output::StandardError);
            // Every id is now unknown → built-in 9002 message
            APP_LOG("MGR_STARTED", {{"name", "x"}});
            // 0 = Init reported failure (expected)
            std::exit(ok ? 1 : 0);
        },
        // Exit code 0
        ::testing::ExitedWithCode(0),
        // stderr: 9001 for the catalogue, then 9002 for the unknown id
        "\\[error\\] \\[9001\\] \\[log\\].*cannot load log catalogue /no/such/catalog.yaml.*"
        "\\[9002\\].*unknown log id 'MGR_STARTED' \\{name=x\\}");
}

// YAML syntax error: Init returns false
TEST_F(AppLogInitDeathTest, InvalidYamlSyntaxFails) {
    // Broken YAML
    const std::string path = Catalog("messages: [unclosed\n");
    // In a child
    EXPECT_EXIT(
        {
            // Load
            const bool ok = app_log::Init(path, "t", app_log::Output::StandardError);
            // 0 = failed as expected
            std::exit(ok ? 1 : 0);
        },
        // Exit 0
        ::testing::ExitedWithCode(0),
        // 9001 reported
        "\\[9001\\].*cannot load log catalogue");
}

// No messages: map: Init returns false with a clear reason
TEST_F(AppLogInitDeathTest, MissingMessagesMapFails) {
    // Only settings
    const std::string path = Catalog("settings:\n  min_level: info\n");
    // In a child
    EXPECT_EXIT(
        {
            // Load
            const bool ok = app_log::Init(path, "t", app_log::Output::StandardError);
            // 0 = failed
            std::exit(ok ? 1 : 0);
        },
        // Exit 0
        ::testing::ExitedWithCode(0),
        // Reason text
        "missing 'messages:' map");
}

// messages: is a list instead of a map
TEST_F(AppLogInitDeathTest, MessagesAsListFails) {
    // A sequence
    const std::string path = Catalog("messages:\n  - a\n  - b\n");
    // In a child
    EXPECT_EXIT(
        {
            // Load
            const bool ok = app_log::Init(path, "t", app_log::Output::StandardError);
            // 0 = failed
            std::exit(ok ? 1 : 0);
        },
        // Exit 0
        ::testing::ExitedWithCode(0),
        // Same reason
        "missing 'messages:' map");
}

// settings.min_level must be info / warning / error
TEST_F(AppLogInitDeathTest, InvalidMinLevelFails) {
    // "debug" is not accepted
    const std::string path = Catalog(std::string("settings:\n  min_level: debug\n") + kMinimalMessages);
    // In a child
    EXPECT_EXIT(
        {
            // Load
            const bool ok = app_log::Init(path, "t", app_log::Output::StandardError);
            // 0 = failed
            std::exit(ok ? 1 : 0);
        },
        // Exit 0
        ::testing::ExitedWithCode(0),
        // Reason names the bad value
        "settings.min_level must be info, warning or error \\(got 'debug'\\)");
}

// A message with an invalid level fails the whole catalogue
TEST_F(AppLogInitDeathTest, InvalidMessageLevelFails) {
    // level: fatal
    const std::string path = Catalog("messages:\n  BAD:\n    level: fatal\n    description: x\n");
    // In a child
    EXPECT_EXIT(
        {
            // Load
            const bool ok = app_log::Init(path, "t", app_log::Output::StandardError);
            // 0 = failed
            std::exit(ok ? 1 : 0);
        },
        // Exit 0
        ::testing::ExitedWithCode(0),
        // Reason names the message and the value
        "message 'BAD': level must be info, warning or error \\(got 'fatal'\\)");
}

// A field with the wrong type (milliseconds: not-a-bool) is a YAML conversion error
TEST_F(AppLogInitDeathTest, WrongFieldTypeFails) {
    // "maybe" is not a bool
    const std::string path = Catalog(std::string("settings:\n  milliseconds: maybe\n") + kMinimalMessages);
    // In a child
    EXPECT_EXIT(
        {
            // Load
            const bool ok = app_log::Init(path, "t", app_log::Output::StandardError);
            // 0 = failed
            std::exit(ok ? 1 : 0);
        },
        // Exit 0
        ::testing::ExitedWithCode(0),
        // 9001 reported
        "\\[9001\\]");
}

// A valid custom catalogue: repeated placeholders, [function] override, field defaults, built-in unknown-id text
TEST_F(AppLogInitDeathTest, CustomCatalogFormatsEveryField) {
    // Only messages:, settings use the defaults
    const std::string path = Catalog(kMinimalMessages);
    // In a child
    EXPECT_EXIT(
        {
            // Load (must succeed)
            if (!app_log::Init(path, "t", app_log::Output::StandardError)) std::exit(1);
            // "{x}-{x}" → "5-5"
            APP_LOG("T_INFO", {{"x", "5"}});
            // function: custom_fn overrides the C++ name
            APP_LOG("T_FUNC");
            // Defaults: code 0000, level error, module log
            APP_LOG("T_BARE");
            // No LOG_UNKNOWN_ID in this catalogue → built-in 9002
            APP_LOG("NOPE");
            // Done
            std::exit(0);
        },
        // Exit 0
        ::testing::ExitedWithCode(0),
        // All four lines in order
        "\\[info\\] \\[7001\\] \\[unit\\] \\[.*\\] info 5-5.*"
        "\\[7004\\] \\[unit\\] \\[custom_fn\\] f.*"
        "\\[error\\] \\[0000\\] \\[log\\] \\[.*\\] bare message.*"
        "\\[error\\] \\[9002\\] \\[log\\].*unknown log id 'NOPE' \\{\\}");
}

// LOG_UNKNOWN_ID in the catalogue overrides the built-in unknown-id message
TEST_F(AppLogInitDeathTest, LogUnknownIdCanBeOverridden) {
    // Custom code and text
    const std::string path = Catalog(
        "messages:\n  LOG_UNKNOWN_ID:\n    code: 7777\n    level: warning\n    module: custom\n"
        "    description: \"missing {id} with {params}\"\n");
    // In a child
    EXPECT_EXIT(
        {
            // Load
            if (!app_log::Init(path, "t", app_log::Output::StandardError)) std::exit(1);
            // Unknown id
            APP_LOG("WHATEVER", {{"k", "v"}});
            // Done
            std::exit(0);
        },
        // Exit 0
        ::testing::ExitedWithCode(0),
        // The custom text
        "\\[warning\\] \\[7777\\] \\[custom\\] .*missing WHATEVER with \\{k=v\\}");
}

// timestamp_format and milliseconds: false are honoured
TEST_F(AppLogInitDeathTest, TimestampFormatAndNoMilliseconds) {
    // HH:MM only, no ".mmm"
    const std::string path =
        Catalog(std::string("settings:\n  timestamp_format: \"%H:%M\"\n  milliseconds: false\n") + kMinimalMessages);
    // In a child
    EXPECT_EXIT(
        {
            // Load
            if (!app_log::Init(path, "t", app_log::Output::StandardError)) std::exit(1);
            // One line
            APP_LOG("T_WARN");
            // Done
            std::exit(0);
        },
        // Exit 0
        ::testing::ExitedWithCode(0),
        // "HH:MM [warning]" at the start of a line, no milliseconds
        "(^|\n)[0-9][0-9]:[0-9][0-9] \\[warning\\] \\[7002\\]");
}

// A timestamp longer than the 64-byte buffer is dropped instead of overflowing (strftime returns 0)
TEST_F(AppLogInitDeathTest, OverlongTimestampFormatIsDropped) {
    // 40 × "%Y" = 160 characters of output
    std::string fmt;
    // Build the format
    for (int i = 0; i < 40; ++i) fmt += "%Y";
    // No milliseconds, so the line starts directly with " [info]"
    const std::string path = Catalog("settings:\n  timestamp_format: \"" + fmt + "\"\n  milliseconds: false\n" +
                                     kMinimalMessages);
    // In a child
    EXPECT_EXIT(
        {
            // Load
            if (!app_log::Init(path, "t", app_log::Output::StandardError)) std::exit(1);
            // One line
            APP_LOG("T_INFO", {{"x", "1"}});
            // Done
            std::exit(0);
        },
        // Exit 0
        ::testing::ExitedWithCode(0),
        // Empty timestamp: the line begins with " [info]"
        "(^|\n) \\[info\\] \\[7001\\]");
}

// Manager mode: lines go to manager_log_file (relative to the yaml folder, folders created), min_level filters
TEST_F(AppLogInitDeathTest, ManagerFileSinkWritesAndFiltersByLevel) {
    // warning and above only; terminal off; file in a sub-folder that does not exist yet
    const std::string path = Catalog(std::string("settings:\n  min_level: warning\n  console: false\n"
                                                 "  color: false\n  manager_log_file: logs/sub/manager.log\n") +
                                     kMinimalMessages);
    // Where the file must appear (relative to the catalogue's folder)
    const std::string log_file = (tmp_ / "logs/sub/manager.log").string();
    // In a child
    EXPECT_EXIT(
        {
            // Manager output mode
            if (!app_log::Init(path, "t", app_log::Output::ConsoleAndFile)) std::exit(1);
            // Below min_level: dropped
            APP_LOG("T_INFO", {{"x", "1"}});
            // Kept
            APP_LOG("T_WARN");
            // Kept
            APP_LOG("T_ERR");
            // Read the file back
            const std::string text = ReadFile(log_file);
            // Bit 1: info leaked in
            int bad = text.find("info 1-1") != std::string::npos ? 2 : 0;
            // Bit 2: warning missing
            if (text.find("[warning] [7002] [unit]") == std::string::npos) bad |= 4;
            // Bit 3: error missing
            if (text.find("[error] [7003] [unit]") == std::string::npos) bad |= 8;
            // Bit 4: glog's own prefix/header leaked into the file
            if (text.find("Log file created") != std::string::npos) bad |= 16;
            // 0 = everything as expected
            std::exit(bad);
        },
        // Exit 0
        ::testing::ExitedWithCode(0),
        // No stderr expectation (console is off)
        "");
}

// Manager mode with console on and no file: lines appear in the terminal (stderr)
TEST_F(AppLogInitDeathTest, ManagerConsoleOnlyWritesToTerminal) {
    // Console on, no colour, no file
    const std::string path =
        Catalog(std::string("settings:\n  console: true\n  color: false\n  manager_log_file: \"\"\n") +
                kMinimalMessages);
    // In a child
    EXPECT_EXIT(
        {
            // Manager mode
            if (!app_log::Init(path, "t", app_log::Output::ConsoleAndFile)) std::exit(1);
            // One info line
            APP_LOG("T_INFO", {{"x", "9"}});
            // Done
            std::exit(0);
        },
        // Exit 0
        ::testing::ExitedWithCode(0),
        // Visible on stderr
        "\\[info\\] \\[7001\\] \\[unit\\] .*info 9-9");
}

// Manager mode with an absolute log file path and console on: line in both places, earlier content kept
TEST_F(AppLogInitDeathTest, ManagerFileIsAppendedAndConsoleMirrors) {
    // Absolute file path
    const std::string log_file = (tmp_ / "abs.log").string();
    // Pre-existing content
    WriteFile(log_file, "previous run\n");
    // Console on, absolute file
    const std::string path = Catalog("settings:\n  console: true\n  color: false\n  manager_log_file: " +
                                     log_file + "\n" + kMinimalMessages);
    // In a child
    EXPECT_EXIT(
        {
            // Manager mode
            if (!app_log::Init(path, "t", app_log::Output::ConsoleAndFile)) std::exit(1);
            // One line
            APP_LOG("T_ERR");
            // File content
            const std::string text = ReadFile(log_file);
            // Old line kept and new line appended
            const bool ok = text.rfind("previous run\n", 0) == 0 && text.find("error line") != std::string::npos;
            // 0 = ok
            std::exit(ok ? 0 : 1);
        },
        // Exit 0
        ::testing::ExitedWithCode(0),
        // Also on the terminal
        "\\[error\\] \\[7003\\].*error line");
}

// Manager mode where the log file cannot be opened: logging keeps working on the terminal
TEST_F(AppLogInitDeathTest, UnopenableManagerFileDropsLinesSafely) {
    // A path under /proc cannot be created
    const std::string path = Catalog(std::string("settings:\n  console: true\n  color: false\n"
                                                 "  manager_log_file: /proc/no_such_dir/manager.log\n") +
                                     kMinimalMessages);
    // In a child
    EXPECT_EXIT(
        {
            // Manager mode
            if (!app_log::Init(path, "t", app_log::Output::ConsoleAndFile)) std::exit(1);
            // Must not crash
            APP_LOG("T_WARN");
            // Done
            std::exit(0);
        },
        // Exit 0
        ::testing::ExitedWithCode(0),
        // Still on the terminal
        "warn line");
}

}  // namespace
