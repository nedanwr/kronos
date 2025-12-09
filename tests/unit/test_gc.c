#include "../../src/core/gc.h"
#include "../../src/core/runtime.h"
#include "../framework/test_framework.h"

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
