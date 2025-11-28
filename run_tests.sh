#!/bin/bash

# Kronos Test Runner
# Runs unit tests and integration tests

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
unit_tests_passed=0
unit_tests_failed=0
integration_tests_passed=0
integration_tests_failed=0

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

# ============================================================================
# UNIT TESTS
# ============================================================================
echo "${BLUE}════════════════════════════════════════════════════════════${NC}"
echo "${BLUE}              RUNNING UNIT TESTS${NC}"
echo "${BLUE}════════════════════════════════════════════════════════════${NC}"
echo ""

# Build unit tests
echo "${BLUE}Building unit tests...${NC}"
if make test-unit > /dev/null 2>&1; then
    echo "${GREEN}✓${NC} Unit tests built successfully"
else
    echo "${RED}✗${NC} Unit test build failed"
    echo "Running make test-unit to see errors:"
    make test-unit
    exit 1
fi
echo ""

# Run unit tests
if [ -f "tests/unit/kronos_unit_tests" ]; then
    ./tests/unit/kronos_unit_tests
    unit_exit_code=$?
    
    if [ $unit_exit_code -eq 0 ]; then
        echo ""
        echo "${GREEN}✓ Unit tests passed${NC}"
    else
        echo ""
        echo "${RED}✗ Unit tests failed${NC}"
        unit_tests_failed=1
    fi
else
    echo "${RED}✗ Unit test executable not found${NC}"
    unit_tests_failed=1
fi

echo ""
echo ""

# ============================================================================
# INTEGRATION TESTS
# ============================================================================
echo "${BLUE}════════════════════════════════════════════════════════════${NC}"
echo "${BLUE}           RUNNING INTEGRATION TESTS${NC}"
echo "${BLUE}════════════════════════════════════════════════════════════${NC}"
echo ""

# Function to run a single integration test
run_integration_test() {
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
            integration_tests_passed=$((integration_tests_passed + 1))
            return 0
        else
            echo "${RED}✗${NC} FAIL: $test_name (expected success, got error)"
            echo "   Output: $output"
            failed_tests=$((failed_tests + 1))
            integration_tests_failed=$((integration_tests_failed + 1))
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
            integration_tests_passed=$((integration_tests_passed + 1))
            return 0
        else
            echo "${RED}✗${NC} FAIL: $test_name (expected error, but succeeded)"
            failed_tests=$((failed_tests + 1))
            integration_tests_failed=$((integration_tests_failed + 1))
            return 1
        fi
    fi
}

# Run integration tests that should pass
echo "${BLUE}Running passing integration tests...${NC}"
echo "────────────────────────────────────────────────────────────"
for test_file in tests/integration/pass/*.kr; do
    if [ -f "$test_file" ]; then
        run_integration_test "$test_file" "true" || true
    fi
done
echo ""

# Run integration tests that should fail
echo "${BLUE}Running error integration tests...${NC}"
echo "────────────────────────────────────────────────────────────"
for test_file in tests/integration/fail/*.kr; do
    if [ -f "$test_file" ]; then
        run_integration_test "$test_file" "false" || true
    fi
done
echo ""

# ============================================================================
# SUMMARY
# ============================================================================
echo "════════════════════════════════════════════════════════════"
echo "                    TEST RESULTS"
echo "════════════════════════════════════════════════════════════"
echo ""

# Unit test summary
if [ $unit_tests_failed -eq 0 ]; then
    echo "${GREEN}Unit Tests:     PASSED${NC}"
else
    echo "${RED}Unit Tests:     FAILED${NC}"
fi

# Integration test summary
echo "Integration Tests:"
echo "  Total:  $total_tests"
echo "  ${GREEN}Passed: $integration_tests_passed${NC}"
if [ $integration_tests_failed -gt 0 ]; then
    echo "  ${RED}Failed: $integration_tests_failed${NC}"
else
    echo "  Failed: $integration_tests_failed"
fi

# Overall summary
echo ""
echo "────────────────────────────────────────────────────────────"
echo "Overall:"
echo "  Total tests:  $total_tests"
echo "  ${GREEN}Passed:       $passed_tests${NC}"
if [ $failed_tests -gt 0 ] || [ $unit_tests_failed -ne 0 ]; then
    echo "  ${RED}Failed:       $((failed_tests + unit_tests_failed))${NC}"
else
    echo "  Failed:       0"
fi

# Calculate percentage
if [ $total_tests -gt 0 ]; then
    percentage=$((passed_tests * 100 / total_tests))
    echo "  Success rate: ${percentage}%"
fi
echo ""

# Exit with appropriate code
if [ $failed_tests -gt 0 ] || [ $unit_tests_failed -ne 0 ]; then
    echo "${RED}✗ TESTS FAILED${NC}"
    exit 1
else
    echo "${GREEN}✓ ALL TESTS PASSED${NC}"
    exit 0
fi
