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
