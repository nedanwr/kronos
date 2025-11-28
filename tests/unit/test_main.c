#include "../framework/test_framework.h"
#include "../../src/core/runtime.h"

// Declare test functions (they're registered via constructor attributes)
extern void test_run_all(void);

int main(void) {
    // Initialize runtime (required for value operations)
    runtime_init();

    test_framework_init();

    // Run all registered tests
    test_run_all();

    // Print results
    test_print_results();

    // Get exit code before cleanup
    int exit_code = test_get_exit_code();

    // Cleanup
    test_framework_cleanup();
    runtime_cleanup();

    return exit_code;
}

