#include "../../src/core/gc.h"
#include "../../src/core/runtime.h"
#include "../framework/test_framework.h"
#include <stdint.h>

static size_t test_gc_bucket_for_ptr(KronosValue *val, size_t capacity) {
  return (size_t)(((uintptr_t)val * 2654435761u) % capacity);
}

TEST(gc_init_cleanup) {
  // Should not crash
  gc_init();
  gc_cleanup();
}

TEST(gc_track_untrack) {
  gc_init();

  KronosValue *val = value_new_number(42);
  ASSERT_PTR_NOT_NULL(val);

  // Track the value
  gc_track(val);

  // Get object count
  size_t count = gc_get_object_count();
  ASSERT_TRUE(count >= 1);

  // Untrack
  gc_untrack(val);

  // Release the value
  value_release(val);

  gc_cleanup();
}

TEST(gc_get_allocated_bytes) {
  gc_init();

  KronosValue *val1 = value_new_number(42);
  KronosValue *val2 = value_new_string("hello", 5);

  gc_track(val1);
  gc_track(val2);

  size_t bytes = gc_get_allocated_bytes();
  ASSERT_TRUE(bytes > 0);

  gc_untrack(val1);
  gc_untrack(val2);
  value_release(val1);
  value_release(val2);

  gc_cleanup();
}

TEST(gc_tuple_allocated_bytes_accounting) {
  gc_init();

  KronosValue *item = value_new_number(42);
  ASSERT_PTR_NOT_NULL(item);

  size_t before_tuple = gc_get_allocated_bytes();

  KronosValue *items[] = {item};
  KronosValue *tuple = value_new_tuple(items, 1);
  ASSERT_PTR_NOT_NULL(tuple);

  size_t after_tuple = gc_get_allocated_bytes();
  size_t expected_tuple_bytes = sizeof(KronosValue) + sizeof(KronosValue *);
  ASSERT_EQ(after_tuple - before_tuple, expected_tuple_bytes);

  value_release(tuple);
  ASSERT_EQ(gc_get_allocated_bytes(), before_tuple);

  value_release(item);
  gc_cleanup();
}

TEST(gc_get_object_count) {
  gc_init();

  size_t initial_count = gc_get_object_count();

  KronosValue *val = value_new_number(42);
  gc_track(val);

  size_t after_track = gc_get_object_count();
  ASSERT_TRUE(after_track > initial_count);

  gc_untrack(val);
  value_release(val);

  gc_cleanup();
}

TEST(gc_track_null) {
  gc_init();

  // Should not crash
  gc_track(NULL);
  gc_untrack(NULL);

  gc_cleanup();
}

TEST(gc_collect_cycles) {
  gc_init();

  // Should not crash (even if cycle detection isn't fully implemented)
  gc_collect_cycles();

  gc_cleanup();
}

TEST(gc_untrack_after_tombstone_collision) {
  gc_init();

  GCStats stats = {0};
  gc_stats(&stats);
  ASSERT_TRUE(stats.array_capacity > 0);

  enum { VALUE_COUNT = 49 };
  KronosValue *values[VALUE_COUNT];
  memset(values, 0, sizeof(values));

  for (size_t i = 0; i < VALUE_COUNT; i++) {
    values[i] = value_new_number((double)i);
    ASSERT_PTR_NOT_NULL(values[i]);
  }

  size_t first = SIZE_MAX;
  size_t second = SIZE_MAX;
  for (size_t i = 0; i < VALUE_COUNT && second == SIZE_MAX; i++) {
    size_t bucket_i = test_gc_bucket_for_ptr(values[i], stats.array_capacity);
    for (size_t j = i + 1; j < VALUE_COUNT; j++) {
      size_t bucket_j =
          test_gc_bucket_for_ptr(values[j], stats.array_capacity);
      if (bucket_i == bucket_j) {
        first = i;
        second = j;
        break;
      }
    }
  }

  bool untrack_found_second = false;
  if (first != SIZE_MAX && second != SIZE_MAX) {
    value_release(values[first]);
    values[first] = NULL;
    size_t count_after_first_release = gc_get_object_count();

    value_release(values[second]);
    values[second] = NULL;
    size_t count_after_second_release = gc_get_object_count();

    untrack_found_second =
        (count_after_second_release + 1 == count_after_first_release);
  }

  for (size_t i = 0; i < VALUE_COUNT; i++) {
    if (values[i]) {
      value_release(values[i]);
    }
  }

  gc_cleanup();

  ASSERT_TRUE(first != SIZE_MAX && second != SIZE_MAX);
  ASSERT_TRUE(untrack_found_second);
}

TEST(gc_cleanup_nested_list) {
  // Regression test for UAF bug: gc_cleanup should not access freed children
  // when finalizing parent objects. This test creates a nested list structure
  // where a child list is added to a parent list, then gc_cleanup is called.
  // The child may be freed before the parent in the tracking array, but
  // gc_cleanup should handle this correctly without use-after-free.
  gc_init();

  // Create a parent list
  KronosValue *parent = value_new_list(4);
  ASSERT_PTR_NOT_NULL(parent);

  // Create a child list
  KronosValue *child = value_new_list(4);
  ASSERT_PTR_NOT_NULL(child);

  // Add child to parent (manually manipulate list structure)
  // Ensure we have capacity
  if (parent->as.list.count >= parent->as.list.capacity) {
    // This shouldn't happen with our initial capacity, but handle it
    ASSERT_TRUE(false); // Test setup error
  }

  // Retain child since parent will hold a reference
  value_retain(child);

  // Add child to parent's items array
  parent->as.list.items[parent->as.list.count] = child;
  parent->as.list.count++;

  // Both objects are tracked by GC
  // When gc_cleanup runs, it may free child before parent (depending on
  // tracking order), but value_finalize should not try to access the
  // already-freed child. This should not crash or cause UAF.
  gc_cleanup();

  // If we get here without crashing, the fix worked
}
