// Unit tests for src/console/text_utils.cpp: Tokenize, Duration, EditDistance, ClosestWord.
// Pure functions, so most scenes are table-driven (TEST_P) and each row shows up as its own test.

// Code under test
#include "console/text_utils.hpp"

// GoogleTest
#include <gtest/gtest.h>

// std::string
#include <string>
// std::vector
#include <vector>

// Anonymous namespace: helper types only visible in this file
namespace {

// ---------------------------------------------------------------------------
// Tokenize
// ---------------------------------------------------------------------------

// One Tokenize scene: a readable name, the input line, and the expected tokens
struct TokenizeCase {
    // Shown in the test name, e.g. TokenizeScenes/TokenizeTest.SplitsAsExpected/QuotedTokenKeepsSpaces
    const char* name;
    // Input line as typed by the user
    std::string input;
    // Expected tokens
    std::vector<std::string> expected;
};

// Parameterised fixture: each TokenizeCase becomes one test
class TokenizeTest : public ::testing::TestWithParam<TokenizeCase> {};

// The single check every row runs
TEST_P(TokenizeTest, SplitsAsExpected) {
    // Current row
    const TokenizeCase& c = GetParam();
    // Tokenize must produce exactly the expected list
    EXPECT_EQ(Tokenize(c.input), c.expected) << "input: [" << c.input << "]";
}

// The scenes
INSTANTIATE_TEST_SUITE_P(
    // Prefix of every generated test name
    TokenizeScenes,
    // The fixture
    TokenizeTest,
    // The rows
    ::testing::Values(
        // Nothing typed
        TokenizeCase{"EmptyLine", "", {}},
        // Only whitespace
        TokenizeCase{"OnlySpaces", "    ", {}},
        // One word
        TokenizeCase{"SingleWord", "status", {"status"}},
        // Command + name
        TokenizeCase{"TwoWords", "pause op1", {"pause", "op1"}},
        // Leading, trailing and repeated spaces are collapsed
        TokenizeCase{"CollapsesRepeatedSpaces", "  start   op1  test ", {"start", "op1", "test"}},
        // Tabs and a trailing newline are whitespace too
        TokenizeCase{"TabsAndNewlines", "stop\tall\n", {"stop", "all"}},
        // A quoted part keeps its spaces
        TokenizeCase{"QuotedTokenKeepsSpaces", "start x \"a b\"", {"start", "x", "a b"}},
        // "" is a real (empty) token, not skipped
        TokenizeCase{"EmptyQuotesIsEmptyToken", "start \"\" y", {"start", "", "y"}},
        // Quotes in the middle of a word are removed and join the parts
        TokenizeCase{"QuotesInsideWordAreRemoved", "ab\"c d\"ef", {"abc def"}},
        // An unclosed quote runs to the end of the line
        TokenizeCase{"UnclosedQuoteRunsToEnd", "start \"a b", {"start", "a b"}},
        // Two quoted parts back to back form one token
        TokenizeCase{"AdjacentQuotedParts", "\"a\"\"b\"", {"ab"}},
        // Tabs inside quotes are kept
        TokenizeCase{"TabInsideQuotes", "\"a\tb\"", {"a\tb"}},
        // UTF-8 bytes (>= 0x80) are not whitespace and must survive the unsigned char cast
        TokenizeCase{"NonAsciiBytesKept", "start \xE6\x89\x8B\xE8\xA1\x93", {"start", "\xE6\x89\x8B\xE8\xA1\x93"}},
        // Paths and option flags are ordinary words
        TokenizeCase{"PathsAndFlags", "start x ./build/lib.so --show -w", {"start", "x", "./build/lib.so", "--show", "-w"}}),
    // Use the row's name as the test name
    [](const ::testing::TestParamInfo<TokenizeCase>& info) { return std::string(info.param.name); });

// ---------------------------------------------------------------------------
// Duration
// ---------------------------------------------------------------------------

// One Duration scene: name, seconds in, text out
struct DurationCase {
    // Test name
    const char* name;
    // Seconds
    double seconds;
    // Expected HH:MM:SS
    const char* expected;
};

// Parameterised fixture
class DurationTest : public ::testing::TestWithParam<DurationCase> {};

// The check
TEST_P(DurationTest, FormatsAsHhMmSs) {
    // Current row
    const DurationCase& c = GetParam();
    // Must match exactly
    EXPECT_EQ(Duration(c.seconds), c.expected);
}

// The scenes
INSTANTIATE_TEST_SUITE_P(
    // Prefix
    DurationScenes,
    // Fixture
    DurationTest,
    // Rows
    ::testing::Values(
        // Zero
        DurationCase{"Zero", 0.0, "00:00:00"},
        // Fractions are truncated, not rounded
        DurationCase{"FractionTruncated", 0.999, "00:00:00"},
        // Just under a minute
        DurationCase{"FiftyNineSeconds", 59, "00:00:59"},
        // Exactly one minute
        DurationCase{"OneMinute", 60, "00:01:00"},
        // Just under an hour
        DurationCase{"JustUnderAnHour", 3599, "00:59:59"},
        // Exactly one hour
        DurationCase{"OneHour", 3600, "01:00:00"},
        // The example from the header comment
        DurationCase{"HeaderExample", 3725, "01:02:05"},
        // Just under a day
        DurationCase{"JustUnderADay", 86399, "23:59:59"},
        // Hours do not wrap at 24
        DurationCase{"HoursDoNotWrap", 86400, "24:00:00"},
        // More than two digits of hours still prints all of them
        DurationCase{"ThreeDigitHours", 360000, "100:00:00"}),
    // Row name as test name
    [](const ::testing::TestParamInfo<DurationCase>& info) { return std::string(info.param.name); });

// ---------------------------------------------------------------------------
// EditDistance
// ---------------------------------------------------------------------------

// One EditDistance scene
struct DistanceCase {
    // Test name
    const char* name;
    // First word
    std::string a;
    // Second word
    std::string b;
    // Expected Levenshtein distance
    size_t expected;
};

// Parameterised fixture
class EditDistanceTest : public ::testing::TestWithParam<DistanceCase> {};

// Distance must be the expected value
TEST_P(EditDistanceTest, MatchesLevenshtein) {
    // Current row
    const DistanceCase& c = GetParam();
    // a → b
    EXPECT_EQ(EditDistance(c.a, c.b), c.expected);
}

// Distance must not depend on argument order
TEST_P(EditDistanceTest, IsSymmetric) {
    // Current row
    const DistanceCase& c = GetParam();
    // b → a equals a → b
    EXPECT_EQ(EditDistance(c.b, c.a), EditDistance(c.a, c.b));
}

// The scenes
INSTANTIATE_TEST_SUITE_P(
    // Prefix
    EditDistanceScenes,
    // Fixture
    EditDistanceTest,
    // Rows
    ::testing::Values(
        // Two empty strings
        DistanceCase{"BothEmpty", "", "", 0},
        // Empty vs. word = deleting every character
        DistanceCase{"SecondEmpty", "abc", "", 3},
        // Word vs. empty = inserting every character
        DistanceCase{"FirstEmpty", "", "abc", 3},
        // Identical words
        DistanceCase{"Identical", "status", "status", 0},
        // One substitution
        DistanceCase{"OneSubstitution", "a", "b", 1},
        // One deletion (typo in a program name)
        DistanceCase{"OneDeletion", "isurgical", "isurgcal", 1},
        // Swapped letters cost two edits in plain Levenshtein
        DistanceCase{"Transposition", "start", "strat", 2},
        // Textbook example
        DistanceCase{"KittenSitting", "kitten", "sitting", 3},
        // Another textbook example
        DistanceCase{"FlawLawn", "flaw", "lawn", 2},
        // Case matters
        DistanceCase{"CaseSensitive", "Test", "test", 1},
        // Completely different
        DistanceCase{"NothingInCommon", "abc", "xyz", 3}),
    // Row name as test name
    [](const ::testing::TestParamInfo<DistanceCase>& info) { return std::string(info.param.name); });

// ---------------------------------------------------------------------------
// ClosestWord
// ---------------------------------------------------------------------------

// The program names used by most ClosestWord scenes (same as configs/processes.yaml)
const std::vector<std::string> kPrograms = {"isurgical", "test"};

// An exact name is its own best match
TEST(ClosestWordTest, ExactMatchIsReturned) {
    // Distance 0
    EXPECT_EQ(ClosestWord(kPrograms, "test"), "test");
}

// A small typo is corrected
TEST(ClosestWordTest, OneTypoIsCorrected) {
    // "isurgicl" is one deletion away from "isurgical"
    EXPECT_EQ(ClosestWord(kPrograms, "isurgicl"), "isurgical");
}

// Distance exactly at the default limit (2) is still accepted
TEST(ClosestWordTest, DistanceAtLimitIsAccepted) {
    // "tset" → "test" needs 2 edits
    EXPECT_EQ(ClosestWord(kPrograms, "tset"), "test");
}

// Distance above the limit gives no suggestion
TEST(ClosestWordTest, TooFarGivesEmpty) {
    // "xyz" is far from both names
    EXPECT_EQ(ClosestWord(kPrograms, "xyz"), "");
}

// No candidates → no suggestion
TEST(ClosestWordTest, NoCandidatesGivesEmpty) {
    // Empty list
    EXPECT_EQ(ClosestWord({}, "test"), "");
}

// max_distance = 0 accepts only exact matches
TEST(ClosestWordTest, ZeroDistanceMeansExactOnly) {
    // One edit away: rejected
    EXPECT_EQ(ClosestWord(kPrograms, "tes", 0), "");
    // Exact: accepted
    EXPECT_EQ(ClosestWord(kPrograms, "test", 0), "test");
}

// A larger limit allows bigger typos
TEST(ClosestWordTest, LargerLimitAllowsMoreEdits) {
    // "surg" → "isurgical" is 5 edits: rejected with the default limit of 2
    EXPECT_EQ(ClosestWord({"isurgical"}, "surg"), "");
    // Accepted with a limit of 5
    EXPECT_EQ(ClosestWord({"isurgical"}, "surg", 5), "isurgical");
    // With both names, "test" (4 edits from "surg") is closer than "isurgical" (5)
    EXPECT_EQ(ClosestWord(kPrograms, "surg", 5), "test");
}

// On a tie the first candidate in the list wins (strict < comparison)
TEST(ClosestWordTest, TieKeepsFirstCandidate) {
    // "aa" is one edit from both "ab" and "ac"
    EXPECT_EQ(ClosestWord({"ab", "ac"}, "aa"), "ab");
    // Order reversed → the other one wins
    EXPECT_EQ(ClosestWord({"ac", "ab"}, "aa"), "ac");
}

// The closest of several is chosen, not just the first acceptable one
TEST(ClosestWordTest, PicksTheClosestNotTheFirst) {
    // "tests" is 1 from "test" and 2 from "tesla"
    EXPECT_EQ(ClosestWord({"tesla", "test"}, "tests"), "test");
}

// An empty name matches a short candidate when within the limit
TEST(ClosestWordTest, EmptyNameMatchesShortCandidate) {
    // "" → "ab" is 2 insertions
    EXPECT_EQ(ClosestWord({"ab", "abcdef"}, ""), "ab");
}

}  // namespace
