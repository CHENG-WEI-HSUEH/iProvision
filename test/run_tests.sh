#!/usr/bin/env bash
# =============================================================================
# Build the GoogleTest suite with coverage, run every test executable, then write
# the pass-rate + coverage report to test/reports/.
#
#   test/run_tests.sh                 build, run, report
#   test/run_tests.sh --no-build      reuse the existing build-test/ (just re-run and re-report)
#   BUILD_DIR=/path test/run_tests.sh  use another build folder (must be a direct child of the project root)
#   JOBS=4 test/run_tests.sh          parallel compile jobs (default: all cores)
#
# Output:
#   test/reports/test_report.html     pass rate + coverage, with annotated source (open in a browser)
#   test/reports/test_report.md       same summary as Markdown
#   test/reports/summary.json         machine-readable numbers (for CI)
#   test/reports/xml/<suite>.xml      raw GoogleTest results (JUnit format)
#   test/reports/logs/<suite>.log     full console output of each test executable
#
# Exit code: 0 if every test passed, 1 otherwise (the report is written either way).
# =============================================================================

# Stop on errors (-e), on unset variables (-u) and on failures inside pipes (pipefail)
set -euo pipefail

# Folder of this script: <project>/test
TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Project root: one level up
ROOT="$(dirname "$TEST_DIR")"
# Build folder; must sit directly in the project root so so_runner finds ../configs
BUILD="${BUILD_DIR:-$ROOT/build-test}"
# Report folder
REPORT="$TEST_DIR/reports"
# Compile jobs
JOBS="${JOBS:-$(nproc)}"
# Build unless --no-build was given
DO_BUILD=1
# Parse the only option
if [[ "${1:-}" == "--no-build" ]]; then DO_BUILD=0; fi

# Build step
if [[ $DO_BUILD -eq 1 ]]; then
    # Configure (Debug + coverage)
    echo "==> configure ($BUILD)"
    # -S test: the stand-alone test project; the main CMakeLists.txt is untouched
    cmake -S "$TEST_DIR" -B "$BUILD" -DCMAKE_BUILD_TYPE=Debug -DENABLE_COVERAGE=ON > /dev/null
    # Compile
    echo "==> build (-j$JOBS)"
    # Only show warnings / errors, not every compiled file
    cmake --build "$BUILD" -j"$JOBS" 2>&1 | grep -Ev '^\[ *[0-9]+%\]|^-- |Built target|Scanning' || true
    # The build must have succeeded (grep above swallows the exit code)
    cmake --build "$BUILD" -j"$JOBS" > /dev/null
fi

# Remove old coverage counters, so the report reflects only this run
find "$BUILD" -name '*.gcda' -delete
# Fresh report folder (keep nothing from a previous run)
rm -rf "$REPORT"
# Sub-folders for gtest XML and per-suite console logs
mkdir -p "$REPORT/xml" "$REPORT/logs"
# Fresh scratch folder for the tests
rm -rf "$BUILD/tmp"
# gtest writes captured stderr here (TEST_TMPDIR) instead of /tmp
mkdir -p "$BUILD/tmp"
# Tell gtest where to put its temporary files
export TEST_TMPDIR="$BUILD/tmp"

# Overall result: 0 = all passed
STATUS=0
# Run every test executable (test_* files that are executable, not folders)
echo "==> run"
# Sorted for a stable order
for exe in $(find "$BUILD" -maxdepth 1 -type f -name 'test_*' -perm -u+x | sort); do
    # Suite name, e.g. test_console
    name="$(basename "$exe")"
    # Start time in seconds
    start=$SECONDS
    # Run it; XML for the report, everything else to its own log file
    if "$exe" --gtest_output="xml:$REPORT/xml/$name.xml" > "$REPORT/logs/$name.log" 2>&1; then
        # Passed
        result="PASS"
    else
        # Failed: remember it, but keep running the other suites
        result="FAIL"
        # Overall failure
        STATUS=1
    fi
    # One summary line per suite, e.g. "  PASS  test_console  (12 s)"
    printf "  %-4s  %-24s (%ds)\n" "$result" "$name" "$((SECONDS - start))"
done

# Generate the reports (pass rate from the XML, coverage from gcov)
echo "==> report"
# -I: isolated mode, nothing from the current folder is imported
python3 -I "$TEST_DIR/tools/gen_report.py" --root "$ROOT" --build "$BUILD" --reports "$REPORT"

# 0 = every test passed
exit $STATUS
