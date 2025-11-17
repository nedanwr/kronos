#!/bin/bash

# Kronos Test Runner
# Runs all tests in tests/pass/ and tests/fail/ directories

set -e  # Exit on any error during build (disabled after build)

# Colors for output
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Counters
total_tests=0
passed_tests=0
failed_tests=0

echo "════════════════════════════════════════════════════════════"
echo "              KRONOS TEST SUITE"
echo "════════════════════════════════════════════════════════════"
echo ""

# Build the project
echo "${BLUE}Building Kronos...${NC}"
if make clean > /dev/null 2>&1 && make > /dev/null 2>&1; then
    echo "${GREEN}✓${NC} Build successful"
else
    echo "${RED}✗${NC} Build failed"
    exit 1
fi

# Disable exit-on-error for test runs (we handle errors explicitly)
set +e
echo ""

# Function to run a single test
run_test() {
    local test_file=$1
    local should_pass=$2
    local test_name=$(basename "$test_file" .kr)

    total_tests=$((total_tests + 1))

    # Run the test and capture output and exit code
    output=$(./kronos "$test_file" 2>&1)
    exit_code=$?

    if [ "$should_pass" = "true" ]; then
        # Test should succeed (exit code 0)
        if [ $exit_code -eq 0 ]; then
            echo "${GREEN}✓${NC} PASS: $test_name"
            passed_tests=$((passed_tests + 1))
            return 0
        else
            echo "${RED}✗${NC} FAIL: $test_name (expected success, got error)"
            echo "   Output: $output"
            failed_tests=$((failed_tests + 1))
            return 1
        fi
    else
        # Test should fail (exit code non-zero)
        if [ $exit_code -ne 0 ]; then
            # Extract the error message
            error_msg=$(echo "$output" | grep -o "Error:.*" | head -1)
            echo "${GREEN}✓${NC} PASS: $test_name"
            echo "   ${YELLOW}↳${NC} $error_msg"
            passed_tests=$((passed_tests + 1))
            return 0
        else
            echo "${RED}✗${NC} FAIL: $test_name (expected error, but succeeded)"
            failed_tests=$((failed_tests + 1))
            return 1
        fi
    fi
}

# Run tests that should pass
echo "${BLUE}Running passing tests...${NC}"
echo "────────────────────────────────────────────────────────────"
for test_file in tests/pass/*.kr; do
    if [ -f "$test_file" ]; then
        run_test "$test_file" "true" || true
    fi
done
echo ""

# Run tests that should fail
echo "${BLUE}Running error tests...${NC}"
echo "────────────────────────────────────────────────────────────"
for test_file in tests/fail/*.kr; do
    if [ -f "$test_file" ]; then
        run_test "$test_file" "false" || true
    fi
done
echo ""

# Summary
echo "════════════════════════════════════════════════════════════"
echo "                    TEST RESULTS"
echo "════════════════════════════════════════════════════════════"
echo ""
echo "Total tests:  $total_tests"
echo "${GREEN}Passed:       $passed_tests${NC}"
if [ $failed_tests -gt 0 ]; then
    echo "${RED}Failed:       $failed_tests${NC}"
else
    echo "Failed:       $failed_tests"
fi
echo ""

# Calculate percentage
if [ $total_tests -gt 0 ]; then
    percentage=$((passed_tests * 100 / total_tests))
    echo "Success rate: ${percentage}%"
fi
echo ""

# Exit with appropriate code
if [ $failed_tests -gt 0 ]; then
    echo "${RED}✗ TESTS FAILED${NC}"
    exit 1
else
    echo "${GREEN}✓ ALL TESTS PASSED${NC}"
    exit 0
fi

