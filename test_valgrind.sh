#!/bin/bash
# Simple script to test valgrind locally (if available) or prepare for CI

set -e

echo "=== Valgrind Memory Leak Check ==="
echo ""

# Check if valgrind is available
if ! command -v valgrind &> /dev/null; then
    echo "⚠ Valgrind not found. This script is designed for Linux CI."
    echo "To test locally, use: act -j memory-check"
    exit 0
fi

# Build if needed
if [ ! -f ./kronos ]; then
    echo "Building Kronos..."
    make clean && make
fi

# Function to check a file with valgrind
check_with_valgrind() {
    local file=$1
    local name=$2
    local expect_error=${3:-false}

    echo "Checking: $name"

    # Create a log file for valgrind output
    local logfile="/tmp/valgrind_${name}.log"

    # Run with valgrind
    if [ "$expect_error" = "true" ]; then
        # For error tests, program will exit with error, but valgrind should still pass
        valgrind \
            --leak-check=full \
            --show-leak-kinds=all \
            --track-origins=yes \
            --error-exitcode=1 \
            --log-file="$logfile" \
            ./kronos "$file" > /dev/null 2>&1 || true
    else
        # For passing tests, both program and valgrind should succeed
        valgrind \
            --leak-check=full \
            --show-leak-kinds=all \
            --track-origins=yes \
            --error-exitcode=1 \
            --log-file="$logfile" \
            ./kronos "$file" > /dev/null 2>&1
    fi

    # Check valgrind exit code
    local valgrind_exit=$?

    # Parse valgrind log for errors
    if grep -q "ERROR SUMMARY: 0 errors" "$logfile" && \
       grep -q "LEAK SUMMARY:.*definitely lost: 0" "$logfile" && \
       grep -q "LEAK SUMMARY:.*indirectly lost: 0" "$logfile"; then
        echo "  ✓ $name: No memory leaks"
        rm -f "$logfile"
        return 0
    else
        echo "  ✗ $name: Memory issues detected"
        echo "  Valgrind report:"
        grep -A 5 "ERROR SUMMARY\|LEAK SUMMARY\|definitely lost\|indirectly lost" "$logfile" | head -20
        rm -f "$logfile"
        return 1
    fi
}

failed_count=0

# Check a few key tests
echo "=== Checking Key Tests ==="
for test_file in tests/pass/logical_operators.kr tests/pass/functions_simple.kr tests/pass/variables_mutable.kr; do
    if [ -f "$test_file" ]; then
        test_name=$(basename "$test_file" .kr)
        if ! check_with_valgrind "$test_file" "$test_name" false; then
            failed_count=$((failed_count + 1))
        fi
    fi
done

# Check one error test
echo ""
echo "=== Checking Error Test ==="
if [ -f "tests/fail/immutable_reassign.kr" ]; then
    if ! check_with_valgrind "tests/fail/immutable_reassign.kr" "immutable_reassign" true; then
        failed_count=$((failed_count + 1))
    fi
fi

# Check an example
echo ""
echo "=== Checking Example ==="
if [ -f "examples/hello.kr" ]; then
    if ! check_with_valgrind "examples/hello.kr" "hello" false; then
        failed_count=$((failed_count + 1))
    fi
fi

echo ""
echo "=== Memory Check Summary ==="
if [ $failed_count -eq 0 ]; then
    echo "✓ All checked files passed memory leak check"
    exit 0
else
    echo "✗ $failed_count file(s) failed memory leak check"
    exit 1
fi

