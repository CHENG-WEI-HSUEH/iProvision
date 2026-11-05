// Unit tests for src/manager/program_config.cpp: LoadPrograms() reading processes.yaml.

// Code under test
#include "manager/program_config.hpp"

// GoogleTest
#include <gtest/gtest.h>

// std::filesystem
#include <filesystem>
// std::string
#include <string>
// std::vector
#include <vector>

// TempDir, WriteFile, EnsureLogInit
#include "test_helpers.hpp"

// Anonymous namespace
namespace {

// Helpers
using namespace test_helpers;

// Fixture: logging + a scratch folder + helpers to write a YAML file and load it
class ProgramConfigTest : public ::testing::Test {
protected:
    // Before each test
    void SetUp() override { EnsureLogInit(); }

    // Write `yaml` to <tmp>/processes.yaml, load it into table_, return LoadPrograms' result; stderr kept in log_
    bool Load(const std::string& yaml, const std::string& file = "processes.yaml") {
        // Where the file goes
        path_ = (tmp_ / file).string();
        // Write it
        WriteFile(path_, yaml);
        // Load it, capturing the log lines
        return LoadPath(path_);
    }

    // Load an existing path into table_; stderr kept in log_
    bool LoadPath(const std::string& path) {
        // Capture stderr (APP_LOG output)
        ::testing::internal::CaptureStderr();
        // The call under test
        const bool ok = LoadPrograms(path, table_);
        // Keep the log text
        log_ = ::testing::internal::GetCapturedStderr();
        // Return the result
        return ok;
    }

    // Put a sentinel entry in the table, to check that a failed load leaves it untouched
    void Prefill() {
        // Sentinel definition
        ProgramDef old;
        // Recognisable library
        old.library = "/old/lib.so";
        // Store
        table_["old"] = old;
    }

    // Scratch folder
    TempDir tmp_;
    // Last YAML path
    std::string path_;
    // The table filled by LoadPrograms
    ProgramTable table_;
    // Captured log output of the last load
    std::string log_;
};

// The project's real configs/processes.yaml loads and resolves its relative paths
TEST_F(ProgramConfigTest, LoadsRealProjectFile) {
    // Project root from CMake
    const fs::path root = fs::path(PROJECT_ROOT_DIR);
    // Load the real file
    ASSERT_TRUE(LoadPath((root / "configs/processes.yaml").string())) << log_;
    // Both programs present
    ASSERT_EQ(table_.count("isurgical"), 1u);
    // Second program
    ASSERT_EQ(table_.count("test"), 1u);
    // ../build/libisurgical_plugin.so relative to configs/
    EXPECT_EQ(table_["isurgical"].library, fs::weakly_canonical(root / "build/libisurgical_plugin.so").string());
    // working_dir: .. → project root
    EXPECT_EQ(table_["isurgical"].working_dir, fs::weakly_canonical(root).string());
    // Default arguments
    EXPECT_EQ(table_["isurgical"].args, (std::vector<std::string>{"--config", "configs/isurgical_config.yaml"}));
    // test: interval 1 s
    EXPECT_EQ(table_["test"].args, (std::vector<std::string>{"1"}));
    // test has no working_dir
    EXPECT_EQ(table_["test"].working_dir, "");
    // CFG_LOADED (4001) logged
    EXPECT_NE(log_.find("[4001]"), std::string::npos) << log_;
    // Count in the message
    EXPECT_NE(log_.find("loaded 2 programs"), std::string::npos) << log_;
}

// Missing file: false, CFG_PARSE_FAILED, table unchanged
TEST_F(ProgramConfigTest, MissingFileFailsAndKeepsTable) {
    // Sentinel
    Prefill();
    // Nothing at this path
    EXPECT_FALSE(LoadPath((tmp_ / "missing.yaml").string()));
    // Sentinel still there
    EXPECT_EQ(table_.size(), 1u);
    // And unchanged
    EXPECT_EQ(table_["old"].library, "/old/lib.so");
    // 4002 logged
    EXPECT_NE(log_.find("[4002]"), std::string::npos) << log_;
}

// YAML syntax error: false, CFG_PARSE_FAILED
TEST_F(ProgramConfigTest, SyntaxErrorFails) {
    // Unclosed flow sequence
    EXPECT_FALSE(Load("programs: [unclosed\n"));
    // 4002
    EXPECT_NE(log_.find("[4002]"), std::string::npos) << log_;
}

// Empty file: no programs: map → CFG_NO_PROGRAMS
TEST_F(ProgramConfigTest, EmptyFileHasNoPrograms) {
    // Zero bytes
    EXPECT_FALSE(Load(""));
    // 4003
    EXPECT_NE(log_.find("[4003]"), std::string::npos) << log_;
}

// Other top-level keys but no programs:
TEST_F(ProgramConfigTest, MissingProgramsKeyFails) {
    // Unrelated key
    EXPECT_FALSE(Load("other: 1\n"));
    // 4003 with the explanatory text
    EXPECT_NE(log_.find("missing 'programs:' map"), std::string::npos) << log_;
}

// programs: as a list is rejected
TEST_F(ProgramConfigTest, ProgramsAsListFails) {
    // Sequence instead of map
    EXPECT_FALSE(Load("programs: [a, b]\n"));
    // 4003
    EXPECT_NE(log_.find("[4003]"), std::string::npos) << log_;
}

// programs: as a scalar is rejected
TEST_F(ProgramConfigTest, ProgramsAsScalarFails) {
    // Plain value
    EXPECT_FALSE(Load("programs: hello\n"));
    // 4003
    EXPECT_NE(log_.find("[4003]"), std::string::npos) << log_;
}

// A program without library: → CFG_NO_LIBRARY naming the program
TEST_F(ProgramConfigTest, MissingLibraryFails) {
    // Only args
    EXPECT_FALSE(Load("programs:\n  foo:\n    args: [\"1\"]\n"));
    // 4004
    EXPECT_NE(log_.find("[4004]"), std::string::npos) << log_;
    // Names the program
    EXPECT_NE(log_.find("program 'foo' has no 'library'"), std::string::npos) << log_;
}

// args: as a string cannot be converted to a list → CFG_PARSE_FAILED
TEST_F(ProgramConfigTest, ArgsAsScalarFails) {
    // Scalar args
    EXPECT_FALSE(Load("programs:\n  foo:\n    library: /x.so\n    args: abc\n"));
    // 4002
    EXPECT_NE(log_.find("[4002]"), std::string::npos) << log_;
}

// args: as a map → CFG_PARSE_FAILED
TEST_F(ProgramConfigTest, ArgsAsMapFails) {
    // Map args
    EXPECT_FALSE(Load("programs:\n  foo:\n    library: /x.so\n    args: {a: 1}\n"));
    // 4002
    EXPECT_NE(log_.find("[4002]"), std::string::npos) << log_;
}

// A program whose body is a scalar has no library
TEST_F(ProgramConfigTest, ProgramBodyAsScalarFails) {
    // foo: value
    EXPECT_FALSE(Load("programs:\n  foo: just-a-string\n"));
    // Not loaded
    EXPECT_TRUE(table_.empty());
}

// A failure in the second program leaves the previous table untouched (all-or-nothing)
TEST_F(ProgramConfigTest, FailureMidwayKeepsOldTable) {
    // Sentinel
    Prefill();
    // First program fine, second has no library
    EXPECT_FALSE(Load("programs:\n  a:\n    library: /a.so\n  b:\n    args: []\n"));
    // Only the sentinel
    ASSERT_EQ(table_.size(), 1u);
    // Untouched
    EXPECT_EQ(table_.count("old"), 1u);
}

// A successful load replaces (not merges into) the previous table
TEST_F(ProgramConfigTest, SuccessReplacesOldTable) {
    // Sentinel
    Prefill();
    // One program
    ASSERT_TRUE(Load("programs:\n  a:\n    library: /a.so\n")) << log_;
    // Sentinel gone
    EXPECT_EQ(table_.count("old"), 0u);
    // New one present
    EXPECT_EQ(table_.count("a"), 1u);
}

// programs: {} is valid and yields an empty table
TEST_F(ProgramConfigTest, EmptyProgramsMapLoadsZero) {
    // Empty map
    EXPECT_TRUE(Load("programs: {}\n")) << log_;
    // Nothing
    EXPECT_TRUE(table_.empty());
    // Count 0 in the log
    EXPECT_NE(log_.find("loaded 0 programs"), std::string::npos) << log_;
}

// Relative library / working_dir are resolved against the YAML file's folder, with ".." collapsed
TEST_F(ProgramConfigTest, RelativePathsResolveAgainstYamlFolder) {
    // File in a sub-folder
    ASSERT_TRUE(Load("programs:\n  p:\n    library: ../lib/x.so\n    working_dir: .\n", "sub/p.yaml")) << log_;
    // <tmp>/sub/../lib/x.so → <tmp>/lib/x.so
    EXPECT_EQ(table_["p"].library, fs::weakly_canonical(tmp_.path() / "lib/x.so").string());
    // "." → <tmp>/sub
    EXPECT_EQ(table_["p"].working_dir, fs::weakly_canonical(tmp_.path() / "sub").string());
}

// Absolute paths are kept exactly
TEST_F(ProgramConfigTest, AbsolutePathsAreKept) {
    // Absolute library and working_dir
    ASSERT_TRUE(Load("programs:\n  p:\n    library: /opt/x/../y.so\n    working_dir: /var/tmp\n")) << log_;
    // Not even normalised
    EXPECT_EQ(table_["p"].library, "/opt/x/../y.so");
    // Kept
    EXPECT_EQ(table_["p"].working_dir, "/var/tmp");
}

// An empty library string stays empty (no path resolution)
TEST_F(ProgramConfigTest, EmptyLibraryStringIsKeptEmpty) {
    // library: ""
    ASSERT_TRUE(Load("programs:\n  p:\n    library: \"\"\n")) << log_;
    // Still empty
    EXPECT_EQ(table_["p"].library, "");
}

// Optional fields default to empty
TEST_F(ProgramConfigTest, OptionalFieldsDefaultToEmpty) {
    // Only library
    ASSERT_TRUE(Load("programs:\n  p:\n    library: /p.so\n")) << log_;
    // No args
    EXPECT_TRUE(table_["p"].args.empty());
    // No working dir
    EXPECT_EQ(table_["p"].working_dir, "");
}

// Explicit empty args list
TEST_F(ProgramConfigTest, EmptyArgsList) {
    // args: []
    ASSERT_TRUE(Load("programs:\n  p:\n    library: /p.so\n    args: []\n")) << log_;
    // Empty
    EXPECT_TRUE(table_["p"].args.empty());
}

// Numbers and booleans in args are read as their text
TEST_F(ProgramConfigTest, ScalarArgsAreReadAsText) {
    // Mixed scalar types
    ASSERT_TRUE(Load("programs:\n  p:\n    library: /p.so\n    args: [1, 2.5, true, \"--x y\"]\n")) << log_;
    // Original spelling
    EXPECT_EQ(table_["p"].args, (std::vector<std::string>{"1", "2.5", "true", "--x y"}));
}

// Block-style args list works like the flow style
TEST_F(ProgramConfigTest, BlockStyleArgsList) {
    // One item per line
    ASSERT_TRUE(Load("programs:\n  p:\n    library: /p.so\n    args:\n      - --config\n      - a.yaml\n")) << log_;
    // Same as ["--config", "a.yaml"]
    EXPECT_EQ(table_["p"].args, (std::vector<std::string>{"--config", "a.yaml"}));
}

// Several programs are all loaded and kept sorted by name (std::map)
TEST_F(ProgramConfigTest, MultipleProgramsSortedByName) {
    // Out of order in the file
    ASSERT_TRUE(Load("programs:\n  zeta:\n    library: /z.so\n  alpha:\n    library: /a.so\n"
                     "  mid:\n    library: /m.so\n")) << log_;
    // Collect names in iteration order
    std::vector<std::string> names;
    // Iterate the map
    for (const auto& [name, def] : table_) names.push_back(name);
    // Sorted
    EXPECT_EQ(names, (std::vector<std::string>{"alpha", "mid", "zeta"}));
    // Count in the log
    EXPECT_NE(log_.find("loaded 3 programs"), std::string::npos) << log_;
}

// A numeric program name is read as text
TEST_F(ProgramConfigTest, NumericProgramNameIsText) {
    // Key 42
    ASSERT_TRUE(Load("programs:\n  42:\n    library: /p.so\n")) << log_;
    // Stored under "42"
    EXPECT_EQ(table_.count("42"), 1u);
}

// A relative path to the YAML file itself works (base becomes absolute)
TEST_F(ProgramConfigTest, RelativeYamlPathIsAccepted) {
    // Write the file first
    WriteFile(tmp_ / "rel.yaml", "programs:\n  p:\n    library: x.so\n");
    // Path relative to the current folder
    const std::string rel = fs::relative(tmp_ / "rel.yaml", fs::current_path()).string();
    // Load through the relative path
    ASSERT_TRUE(LoadPath(rel)) << log_;
    // x.so next to the YAML file, absolute
    EXPECT_EQ(table_["p"].library, fs::weakly_canonical(tmp_.path() / "x.so").string());
}

// Extra unknown keys in a program are ignored
TEST_F(ProgramConfigTest, UnknownKeysAreIgnored) {
    // "comment" is not a known field
    ASSERT_TRUE(Load("programs:\n  p:\n    library: /p.so\n    comment: hello\n")) << log_;
    // Loaded normally
    EXPECT_EQ(table_["p"].library, "/p.so");
}

}  // namespace
