// Unit tests for src/app_config.{hpp,cpp}: the fixed settings and ResolveFromExecutable().

// Code under test
#include "app_config.hpp"

// GoogleTest
#include <gtest/gtest.h>

// std::filesystem
#include <filesystem>
// std::string
#include <string>

// Anonymous namespace
namespace {

// Short alias
namespace fs = std::filesystem;

// Folder of the running test executable (what ResolveFromExecutable uses as its base)
fs::path ExeDir() {
    // /proc/self/exe is a symlink to this executable
    return fs::read_symlink("/proc/self/exe").parent_path();
}

// The constants must keep the values the rest of the program relies on
TEST(AppConfigConstantsTest, HaveExpectedValues) {
    // so_runner sits next to the manager
    EXPECT_STREQ(app_config::kRunnerFile, "so_runner");
    // processes.yaml is one folder up, in configs/
    EXPECT_STREQ(app_config::kProgramsFile, "../configs/processes.yaml");
    // Child logs go to logs/ in the project root
    EXPECT_STREQ(app_config::kLogDir, "../logs");
    // Log catalogue path
    EXPECT_STREQ(app_config::kLogCatalogFile, "../configs/log_messages.yaml");
    // glog program names
    EXPECT_STREQ(app_config::kManagerName, "iprovision");
    // Runner's glog name
    EXPECT_STREQ(app_config::kRunnerName, "so_runner");
}

// A plain file name resolves to "<exe folder>/<name>"
TEST(ResolveFromExecutableTest, FileNameIsNextToExecutable) {
    // so_runner next to the test executable
    EXPECT_EQ(app_config::ResolveFromExecutable("so_runner"), (ExeDir() / "so_runner").string());
}

// The result is always absolute
TEST(ResolveFromExecutableTest, ResultIsAbsolute) {
    // Any relative input
    EXPECT_TRUE(fs::path(app_config::ResolveFromExecutable("a/b/c")).is_absolute());
}

// ".." components are collapsed (weakly_canonical)
TEST(ResolveFromExecutableTest, DotDotIsCollapsed) {
    // The resolved text
    const std::string resolved = app_config::ResolveFromExecutable("../configs/processes.yaml");
    // No ".." left
    EXPECT_EQ(resolved.find(".."), std::string::npos);
    // Equal to the parent of the exe folder + configs/processes.yaml
    EXPECT_EQ(resolved, (ExeDir().parent_path() / "configs" / "processes.yaml").string());
}

// "a/../b" collapses to "b"
TEST(ResolveFromExecutableTest, InnerDotDotIsCollapsed) {
    // Expect <exe>/b
    EXPECT_EQ(app_config::ResolveFromExecutable("a/../b"), (ExeDir() / "b").string());
}

// The real configuration files are found when the build folder is a child of the project root
TEST(ResolveFromExecutableTest, FindsRealConfigFiles) {
    // Log catalogue
    EXPECT_TRUE(fs::exists(app_config::ResolveFromExecutable(app_config::kLogCatalogFile)))
        << "build folder must be a direct child of the project root";
    // processes.yaml
    EXPECT_TRUE(fs::exists(app_config::ResolveFromExecutable(app_config::kProgramsFile)));
}

// A path that does not exist is still resolved (weakly_canonical does not require existence)
TEST(ResolveFromExecutableTest, MissingPathStillResolves) {
    // Not on disk
    EXPECT_EQ(app_config::ResolveFromExecutable("no/such/file.txt"), (ExeDir() / "no/such/file.txt").string());
}

// An absolute input replaces the base (path operator/ semantics)
TEST(ResolveFromExecutableTest, AbsoluteInputIsKept) {
    // /etc exists on every Linux system
    EXPECT_EQ(app_config::ResolveFromExecutable("/etc"), "/etc");
}

// "." resolves to the exe folder itself
TEST(ResolveFromExecutableTest, DotIsExeFolder) {
    // weakly_canonical of "<exe>/." is "<exe>"
    EXPECT_EQ(app_config::ResolveFromExecutable("."), ExeDir().string());
}

}  // namespace
