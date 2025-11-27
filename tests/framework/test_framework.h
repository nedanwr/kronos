#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Test result tracking
typedef struct {
  const char *name;
  bool passed;
  const char *file;
  int line;
  const char *message;
} TestResult;

typedef struct {
  TestResult *results;
  size_t count;
  size_t capacity;
  size_t passed;
  size_t failed;
} TestSuite;

// Test function type
typedef void (*TestFunc)(void);

// Test registration
typedef struct {
  const char *name;
  TestFunc func;
  const char *file;
} TestCase;

// Global test suite
extern TestSuite *g_test_suite;

// Initialize test framework
void test_framework_init(void);
void test_framework_cleanup(void);

// Run a single test
void test_run(const char *name, TestFunc func, const char *file);

// Assertions
#define ASSERT_TRUE(condition)                                                 \
  do {                                                                         \
    if (!(condition)) {                                                        \
      test_fail(__FILE__, __LINE__, "Assertion failed: " #condition);          \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define ASSERT_FALSE(condition)                                                \
  do {                                                                         \
    if (condition) {                                                           \
      test_fail(__FILE__, __LINE__, "Assertion failed: !(" #condition ")");    \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define ASSERT_EQ(a, b)                                                        \
  do {                                                                         \
    if ((a) != (b)) {                                                          \
      char msg[256];                                                           \
      snprintf(msg, sizeof(msg), "Expected %s == %s, got %ld != %ld", #a, #b,  \
               (long)(a), (long)(b));                                          \
      test_fail(__FILE__, __LINE__, msg);                                      \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define ASSERT_NE(a, b)                                                        \
  do {                                                                         \
    if ((a) == (b)) {                                                          \
      char msg[256];                                                           \
      snprintf(msg, sizeof(msg), "Expected %s != %s, but both are %ld", #a,    \
               #b, (long)(a));                                                 \
      test_fail(__FILE__, __LINE__, msg);                                      \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define ASSERT_INT_EQ(a, b)                                                    \
  do {                                                                         \
    int _a = (a);                                                              \
    int _b = (b);                                                              \
    if (_a != _b) {                                                            \
      char msg[256];                                                           \
      snprintf(msg, sizeof(msg), "Expected %s == %s, got %d != %d", #a, #b,    \
               _a, _b);                                                        \
      test_fail(__FILE__, __LINE__, msg);                                      \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define ASSERT_DOUBLE_EQ(a, b)                                                 \
  do {                                                                         \
    double _a = (a);                                                           \
    double _b = (b);                                                           \
    double _diff = (_a > _b) ? (_a - _b) : (_b - _a);                          \
    if (_diff > 0.0001) {                                                      \
      char msg[256];                                                           \
      snprintf(msg, sizeof(msg), "Expected %s == %s, got %.6f != %.6f", #a,    \
               #b, _a, _b);                                                    \
      test_fail(__FILE__, __LINE__, msg);                                      \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define ASSERT_STR_EQ(a, b)                                                    \
  do {                                                                         \
    const char *_a = (a);                                                      \
    const char *_b = (b);                                                      \
    if (_a == NULL || _b == NULL) {                                            \
      if (_a != _b) {                                                          \
        char msg[256];                                                         \
        snprintf(msg, sizeof(msg), "Expected %s == %s, got %s != %s", #a, #b,  \
                 _a ? _a : "NULL", _b ? _b : "NULL");                          \
        test_fail(__FILE__, __LINE__, msg);                                    \
        return;                                                                \
      }                                                                        \
    } else if (strcmp(_a, _b) != 0) {                                          \
      char msg[512];                                                           \
      snprintf(msg, sizeof(msg), "Expected %s == %s, got \"%s\" != \"%s\"",    \
               #a, #b, _a, _b);                                                \
      test_fail(__FILE__, __LINE__, msg);                                      \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define ASSERT_STR_NE(a, b)                                                    \
  do {                                                                         \
    const char *_a = (a);                                                      \
    const char *_b = (b);                                                      \
    if (_a == NULL || _b == NULL) {                                            \
      if (_a == _b) {                                                          \
        char msg[256];                                                         \
        snprintf(msg, sizeof(msg), "Expected %s != %s, but both are NULL", #a, \
                 #b);                                                          \
        test_fail(__FILE__, __LINE__, msg);                                    \
        return;                                                                \
      }                                                                        \
    } else if (strcmp(_a, _b) == 0) {                                          \
      char msg[256];                                                           \
      snprintf(msg, sizeof(msg), "Expected %s != %s, but both are \"%s\"", #a, \
               #b, _a);                                                        \
      test_fail(__FILE__, __LINE__, msg);                                      \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define ASSERT_PTR_NULL(ptr)                                                   \
  do {                                                                         \
    if ((ptr) != NULL) {                                                       \
      char msg[256];                                                           \
      snprintf(msg, sizeof(msg), "Expected %s to be NULL, got %p", #ptr,       \
               (void *)(ptr));                                                 \
      test_fail(__FILE__, __LINE__, msg);                                      \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define ASSERT_PTR_NOT_NULL(ptr)                                               \
  do {                                                                         \
    if ((ptr) == NULL) {                                                       \
      test_fail(__FILE__, __LINE__, "Expected " #ptr " to be non-NULL");       \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define ASSERT_MSG(condition, msg)                                             \
  do {                                                                         \
    if (!(condition)) {                                                        \
      test_fail(__FILE__, __LINE__, msg);                                      \
      return;                                                                  \
    }                                                                          \
  } while (0)

// Internal function to record test failure
void test_fail(const char *file, int line, const char *message);

// Print test results
void test_print_results(void);

// Get exit code (0 if all passed, 1 if any failed)
int test_get_exit_code(void);

// Test registration macro
#define TEST(name)                                                             \
  static void test_##name(void);                                               \
  __attribute__((constructor)) static void register_test_##name(void) {        \
    test_register(#name, test_##name, __FILE__);                               \
  }                                                                            \
  static void test_##name(void)

// Internal registration function
void test_register(const char *name, TestFunc func, const char *file);

// Run all registered tests
void test_run_all(void);

#endif // TEST_FRAMEWORK_H
