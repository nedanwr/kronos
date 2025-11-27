#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Global test suite
TestSuite *g_test_suite = NULL;

// Test case registry
static TestCase *g_test_cases = NULL;
static size_t g_test_case_count = 0;
static size_t g_test_case_capacity = 0;

// Current test being run
static const char *g_current_test_name = NULL;

// Colors for output
#define COLOR_GREEN "\033[0;32m"
#define COLOR_RED "\033[0;31m"
#define COLOR_YELLOW "\033[1;33m"
#define COLOR_BLUE "\033[0;34m"
#define COLOR_RESET "\033[0m"

void test_framework_init(void) {
    if (g_test_suite == NULL) {
        g_test_suite = malloc(sizeof(TestSuite));
        if (g_test_suite == NULL) {
            fprintf(stderr, "Failed to allocate test suite\n");
            exit(1);
        }
        g_test_suite->results = NULL;
        g_test_suite->count = 0;
        g_test_suite->capacity = 0;
        g_test_suite->passed = 0;
        g_test_suite->failed = 0;
    }
}

void test_framework_cleanup(void) {
    if (g_test_suite != NULL) {
        // Free test results
        for (size_t i = 0; i < g_test_suite->count; i++) {
            if (g_test_suite->results[i].message != NULL) {
                free((void*)g_test_suite->results[i].message);
            }
        }
        free(g_test_suite->results);
        free(g_test_suite);
        g_test_suite = NULL;
    }
    
    // Free test cases
    free(g_test_cases);
    g_test_cases = NULL;
    g_test_case_count = 0;
    g_test_case_capacity = 0;
}

void test_register(const char *name, TestFunc func, const char *file) {
    // Grow array if needed
    if (g_test_case_count >= g_test_case_capacity) {
        size_t new_capacity = g_test_case_capacity == 0 ? 16 : g_test_case_capacity * 2;
        TestCase *new_cases = realloc(g_test_cases, new_capacity * sizeof(TestCase));
        if (new_cases == NULL) {
            fprintf(stderr, "Failed to allocate test case array\n");
            exit(1);
        }
        g_test_cases = new_cases;
        g_test_case_capacity = new_capacity;
    }
    
    g_test_cases[g_test_case_count].name = name;
    g_test_cases[g_test_case_count].func = func;
    g_test_cases[g_test_case_count].file = file;
    g_test_case_count++;
}

void test_fail(const char *file, int line, const char *message) {
    if (g_test_suite == NULL) {
        test_framework_init();
    }
    
    // Use current test name if available
    const char *test_name = g_current_test_name != NULL ? g_current_test_name : "unknown";
    
    // Find the test result for the current test and mark it as failed
    // Look for the most recent test result with this name
    for (size_t i = g_test_suite->count; i > 0; i--) {
        TestResult *result = &g_test_suite->results[i - 1];
        if (result->name == test_name && result->passed) {
            // Mark this test as failed
            result->passed = false;
            result->file = file;
            result->line = line;
            if (result->message != NULL) {
                free((void*)result->message);
            }
            result->message = strdup(message);
            g_test_suite->failed++;
            g_test_suite->passed--; // Decrement passed count
            return;
        }
    }
    
    // If we couldn't find the test result, create a new one
    // Grow results array if needed
    if (g_test_suite->count >= g_test_suite->capacity) {
        size_t new_capacity = g_test_suite->capacity == 0 ? 16 : g_test_suite->capacity * 2;
        TestResult *new_results = realloc(g_test_suite->results, new_capacity * sizeof(TestResult));
        if (new_results == NULL) {
            fprintf(stderr, "Failed to allocate test results array\n");
            exit(1);
        }
        g_test_suite->results = new_results;
        g_test_suite->capacity = new_capacity;
    }
    
    TestResult *result = &g_test_suite->results[g_test_suite->count];
    result->name = test_name;
    result->passed = false;
    result->file = file;
    result->line = line;
    result->message = strdup(message);
    
    g_test_suite->count++;
    g_test_suite->failed++;
}

void test_run(const char *name, TestFunc func, const char *file) {
    if (g_test_suite == NULL) {
        test_framework_init();
    }
    
    // Grow results array if needed
    if (g_test_suite->count >= g_test_suite->capacity) {
        size_t new_capacity = g_test_suite->capacity == 0 ? 16 : g_test_suite->capacity * 2;
        TestResult *new_results = realloc(g_test_suite->results, new_capacity * sizeof(TestResult));
        if (new_results == NULL) {
            fprintf(stderr, "Failed to allocate test results array\n");
            exit(1);
        }
        g_test_suite->results = new_results;
        g_test_suite->capacity = new_capacity;
    }
    
    // Record test start
    TestResult *result = &g_test_suite->results[g_test_suite->count];
    result->name = name;
    result->passed = true;
    result->file = file;
    result->line = 0;
    result->message = NULL;
    g_test_suite->count++;
    
    // Set current test name
    const char *old_test_name = g_current_test_name;
    g_current_test_name = name;
    
    // Run the test
    func();
    
    // Restore old test name
    g_current_test_name = old_test_name;
    
    // Check if test passed (no failure was recorded)
    if (result->passed) {
        // Test completed without failure
        g_test_suite->passed++;
    }
}

void test_run_all(void) {
    if (g_test_suite == NULL) {
        test_framework_init();
    }
    
    printf("════════════════════════════════════════════════════════════\n");
    printf("              KRONOS UNIT TEST SUITE\n");
    printf("════════════════════════════════════════════════════════════\n");
    printf("\n");
    
    size_t total_tests = g_test_case_count;
    size_t current_test = 0;
    
    for (size_t i = 0; i < g_test_case_count; i++) {
        current_test++;
        printf("[%zu/%zu] Running %s... ", current_test, total_tests, g_test_cases[i].name);
        fflush(stdout);
        
        // Reset failure state for this test
        size_t start_failed = g_test_suite->failed;
        
        // Run the test
        test_run(g_test_cases[i].name, g_test_cases[i].func, g_test_cases[i].file);
        
        // Check if it passed
        if (g_test_suite->failed == start_failed) {
            printf(COLOR_GREEN "✓ PASS" COLOR_RESET "\n");
        } else {
            printf(COLOR_RED "✗ FAIL" COLOR_RESET "\n");
        }
    }
    
    printf("\n");
}

void test_print_results(void) {
    if (g_test_suite == NULL) {
        return;
    }
    
    printf("════════════════════════════════════════════════════════════\n");
    printf("                    TEST RESULTS\n");
    printf("════════════════════════════════════════════════════════════\n");
    printf("\n");
    
    // Print summary
    printf("Total tests:  %zu\n", g_test_suite->count);
    printf(COLOR_GREEN "Passed:       %zu" COLOR_RESET "\n", g_test_suite->passed);
    if (g_test_suite->failed > 0) {
        printf(COLOR_RED "Failed:       %zu" COLOR_RESET "\n", g_test_suite->failed);
    } else {
        printf("Failed:       %zu\n", g_test_suite->failed);
    }
    
    // Calculate percentage
    if (g_test_suite->count > 0) {
        int percentage = (int)((g_test_suite->passed * 100) / g_test_suite->count);
        printf("Success rate: %d%%\n", percentage);
    }
    
    printf("\n");
    
    // Print failures
    if (g_test_suite->failed > 0) {
        printf(COLOR_RED "FAILURES:" COLOR_RESET "\n");
        printf("────────────────────────────────────────────────────────────\n");
        
        for (size_t i = 0; i < g_test_suite->count; i++) {
            if (!g_test_suite->results[i].passed) {
                printf(COLOR_RED "✗ %s" COLOR_RESET "\n", g_test_suite->results[i].name);
                printf("  %s:%d\n", g_test_suite->results[i].file, g_test_suite->results[i].line);
                if (g_test_suite->results[i].message != NULL) {
                    printf("  %s\n", g_test_suite->results[i].message);
                }
                printf("\n");
            }
        }
    }
}

int test_get_exit_code(void) {
    if (g_test_suite == NULL || g_test_suite->failed > 0) {
        return 1;
    }
    return 0;
}

