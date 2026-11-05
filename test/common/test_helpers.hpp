// Shared helpers for every test executable: logging initialisation, temporary folders, file I/O, waiting.
#pragma once

// std::chrono durations and clocks
#include <chrono>
// std::filesystem
#include <filesystem>
// std::ifstream / std::ofstream
#include <fstream>
// std::function for WaitUntil predicates
#include <functional>
// std::ostringstream
#include <sstream>
// std::string
#include <string>
// std::this_thread::sleep_for
#include <thread>

// kill
#include <csignal>
// std::getenv
#include <cstdlib>
// getpid
#include <unistd.h>

// GoogleTest: current test name for folder names
#include <gtest/gtest.h>

// app_log::Init
#include "app_log.hpp"

// Everything lives in its own namespace to avoid clashing with code under test
namespace test_helpers {

// Short alias
namespace fs = std::filesystem;

// Absolute path of the project's real log catalogue
inline std::string CatalogPath() {
    // PROJECT_ROOT_DIR is passed in by test/CMakeLists.txt
    return std::string(PROJECT_ROOT_DIR) + "/configs/log_messages.yaml";
}

// Initialise app_log once per process with the real catalogue, writing to stderr only
inline void EnsureLogInit() {
    // Only the first call does anything (glog may only be initialised once)
    static bool done = false;
    // Already initialised
    if (done) return;
    // Remember
    done = true;
    // StandardError: no manager.log file is touched by the tests
    app_log::Init(CatalogPath(), "iprovision_tests", app_log::Output::StandardError);
}

// Write text to a file (creating parent folders), replacing any previous content
inline void WriteFile(const fs::path& path, const std::string& text) {
    // Make sure the folder exists
    fs::create_directories(path.parent_path());
    // Open for writing, truncating
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    // Write everything
    out << text;
}

// Read a whole file into a string ("" if it cannot be opened)
inline std::string ReadFile(const fs::path& path) {
    // Open for reading
    std::ifstream in(path, std::ios::binary);
    // Buffer
    std::ostringstream buf;
    // Copy the whole stream
    buf << in.rdbuf();
    // Return the text
    return buf.str();
}

// Write a shell script and make it executable
inline fs::path WriteScript(const fs::path& path, const std::string& body) {
    // Shebang + body
    WriteFile(path, "#!/bin/sh\n" + body + "\n");
    // rwxr-xr-x
    fs::permissions(path, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                              fs::perms::others_read | fs::perms::others_exec);
    // Return the path for convenience
    return path;
}

// Poll pred every `interval` until it returns true or `timeout` passes; returns the last result
inline bool WaitUntil(const std::function<bool()>& pred,
                      std::chrono::milliseconds timeout = std::chrono::milliseconds(3000),
                      std::chrono::milliseconds interval = std::chrono::milliseconds(10)) {
    // Deadline
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    // Until the deadline
    while (std::chrono::steady_clock::now() < deadline) {
        // Condition met
        if (pred()) return true;
        // Wait a bit
        std::this_thread::sleep_for(interval);
    }
    // One last check after the deadline
    return pred();
}

// Kernel state letter of a pid from /proc/<pid>/stat (R, S, T, Z, ...); 0 if the pid does not exist
inline char KernelStateOf(int pid) {
    // Open the stat file
    std::ifstream file("/proc/" + std::to_string(pid) + "/stat");
    // Its single line
    std::string line;
    // Process does not exist
    if (!std::getline(file, line)) return 0;
    // The command name is in parentheses and may contain ')', so use the last one
    const size_t paren = line.rfind(')');
    // The state letter follows ") "
    return (paren != std::string::npos && paren + 2 < line.size()) ? line[paren + 2] : 0;
}

// True while the pid exists and is not a zombie
inline bool IsAlive(int pid) {
    // Current state letter
    const char s = KernelStateOf(pid);
    // Missing (0) or zombie (Z) both count as dead
    return s != 0 && s != 'Z';
}

// Milliseconds elapsed since `start`
inline long long ElapsedMs(std::chrono::steady_clock::time_point start) {
    // Convert the difference to whole milliseconds
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
}

// Unique scratch folder per test: <TEST_TMP_DIR>/<Suite>.<Test>_<pid>_<n>; deleted when the object is destroyed
class TempDir {
public:
    // Create the folder
    TempDir() {
        // Per-process counter, so two TempDirs in one test do not collide
        static int counter = 0;
        // Name of the running test (or "global" outside a test)
        std::string label = "global";
        // gtest knows which test is running
        const ::testing::TestInfo* info = ::testing::UnitTest::GetInstance()->current_test_info();
        // Inside a test
        if (info) label = std::string(info->test_suite_name()) + "." + info->name();
        // Parameterised names contain '/', which is not allowed in a folder name
        for (char& c : label) if (c == '/') c = '_';
        // Full path
        path_ = fs::path(TEST_TMP_DIR) / (label + "_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
        // Start from an empty folder
        fs::remove_all(path_);
        // Create it
        fs::create_directories(path_);
    }
    // Remove the folder and everything in it (kept if KEEP_TEST_TMP is set, for debugging)
    ~TempDir() {
        // Error code: never throw from a destructor
        std::error_code ec;
        // Delete unless asked to keep it
        if (!std::getenv("KEEP_TEST_TMP")) fs::remove_all(path_, ec);
    }
    // Not copyable (two objects would delete the same folder)
    TempDir(const TempDir&) = delete;
    // Not copy-assignable
    TempDir& operator=(const TempDir&) = delete;
    // The folder
    const fs::path& path() const { return path_; }
    // A path inside the folder
    fs::path operator/(const std::string& name) const { return path_ / name; }

private:
    // The folder's absolute path
    fs::path path_;
};

}  // namespace test_helpers
