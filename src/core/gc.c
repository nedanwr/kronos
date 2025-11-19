#include "gc.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Simple GC tracking for reference counting
#define INITIAL_TRACKED_CAPACITY 1024

typedef struct {
  KronosValue **objects;
  size_t count;
  size_t capacity;
  size_t allocated_bytes;
} GCState;

static GCState gc_state = {0};
static pthread_mutex_t gc_mutex = PTHREAD_MUTEX_INITIALIZER;

static void gc_abort_allocation(void) {
  fprintf(stderr,
          "Fatal: Failed to grow GC tracking table (current count: %zu, "
          "capacity: %zu)\n",
          gc_state.count, gc_state.capacity);
  fflush(stderr);
  abort();
}

static void gc_ensure_capacity_locked(size_t min_capacity) {
  if (gc_state.capacity >= min_capacity)
    return;

  size_t new_capacity =
      gc_state.capacity ? gc_state.capacity : INITIAL_TRACKED_CAPACITY;

  while (new_capacity < min_capacity) {
    if (new_capacity > SIZE_MAX / 2) {
      gc_abort_allocation();
    }
    new_capacity *= 2;
  }

  KronosValue **new_objects =
      realloc(gc_state.objects, new_capacity * sizeof(KronosValue *));
  if (!new_objects) {
    gc_abort_allocation();
  }

  memset(new_objects + gc_state.capacity, 0,
         (new_capacity - gc_state.capacity) * sizeof(KronosValue *));

  gc_state.objects = new_objects;
  gc_state.capacity = new_capacity;
}

// Initialize GC
void gc_init(void) {
  pthread_mutex_lock(&gc_mutex);
  // Free any previously allocated memory if gc_init is called multiple times
  if (gc_state.objects) {
    free(gc_state.objects);
    gc_state.objects = NULL;
  }
  memset(&gc_state, 0, sizeof(GCState));
  gc_ensure_capacity_locked(INITIAL_TRACKED_CAPACITY);
  pthread_mutex_unlock(&gc_mutex);
}

// Cleanup GC
void gc_cleanup(void) {
  // All objects should have been freed by now
  // This is just a safety check
  pthread_mutex_lock(&gc_mutex);
  for (size_t i = 0; i < gc_state.count; i++) {
    KronosValue *obj = gc_state.objects[i];
    if (!obj)
      continue;

    gc_state.allocated_bytes -= sizeof(KronosValue);

    if (obj->type == VAL_STRING) {
      size_t payload = obj->as.string.length + 1;
      char *data = obj->as.string.data;
      if (data) {
        gc_state.allocated_bytes -= payload;
        free(data);
        obj->as.string.data = NULL;
      }
    }

    // Force free leaked objects
    free(obj);
    gc_state.objects[i] = NULL;
  }
  gc_state.count = 0;
  gc_state.allocated_bytes = 0;
  free(gc_state.objects);
  gc_state.objects = NULL;
  gc_state.capacity = 0;
  pthread_mutex_unlock(&gc_mutex);
}

// Track a new object
void gc_track(KronosValue *val) {
  if (!val)
    return;

  pthread_mutex_lock(&gc_mutex);
  // Check if object is already tracked to prevent duplicates
  for (size_t i = 0; i < gc_state.count; i++) {
    if (gc_state.objects[i] == val) {
      // Already tracked, skip
      pthread_mutex_unlock(&gc_mutex);
      return;
    }
  }

  // Object not found, add it
  gc_ensure_capacity_locked(gc_state.count + 1);
  gc_state.objects[gc_state.count++] = val;
  gc_state.allocated_bytes += sizeof(KronosValue);

  // Add size of string data if applicable
  if (val->type == VAL_STRING) {
    gc_state.allocated_bytes += val->as.string.length + 1;
  }
  pthread_mutex_unlock(&gc_mutex);
}

// Untrack an object (when it's being freed)
void gc_untrack(KronosValue *val) {
  if (!val)
    return;

  pthread_mutex_lock(&gc_mutex);
  for (size_t i = 0; i < gc_state.count; i++) {
    if (gc_state.objects[i] == val) {
      // Subtract from allocated bytes
      gc_state.allocated_bytes -= sizeof(KronosValue);
      if (val->type == VAL_STRING) {
        gc_state.allocated_bytes -= val->as.string.length + 1;
      }

      // Remove from array by shifting
      for (size_t j = i; j < gc_state.count - 1; j++) {
        gc_state.objects[j] = gc_state.objects[j + 1];
      }
      gc_state.count--;
      pthread_mutex_unlock(&gc_mutex);
      return;
    }
  }
  pthread_mutex_unlock(&gc_mutex);
}

// Cycle detection using mark-and-sweep
// For now, this is a placeholder since we're focusing on reference counting
// Full cycle detection will be implemented when we have complex data structures
void gc_collect_cycles(void) {
  // TODO: Implement mark-and-sweep for cycles
  // This will be needed when we have circular references
  // (e.g., lists containing themselves, closures with circular refs)

  // For now, we rely on pure reference counting
}

// Get allocated bytes
size_t gc_get_allocated_bytes(void) {
  pthread_mutex_lock(&gc_mutex);
  size_t bytes = gc_state.allocated_bytes;
  pthread_mutex_unlock(&gc_mutex);
  return bytes;
}

// Get object count
size_t gc_get_object_count(void) {
  pthread_mutex_lock(&gc_mutex);
  size_t count = gc_state.count;
  pthread_mutex_unlock(&gc_mutex);
  return count;
}
