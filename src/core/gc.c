/**
 * @file gc.c
 * @brief Garbage collection and memory tracking
 *
 * Provides reference-counting based garbage collection for Kronos values.
 * Tracks all allocated objects and provides statistics. Thread-safe using
 * mutexes for concurrent access.
 */

#include "gc.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Initial capacity for the object tracking array */
#define INITIAL_TRACKED_CAPACITY 1024

/**
 * Garbage collector state
 * Tracks all allocated KronosValue objects for leak detection and statistics
 */
typedef struct {
  KronosValue **objects;  /**< Array of tracked object pointers */
  size_t count;           /**< Number of currently tracked objects */
  size_t capacity;        /**< Capacity of the objects array */
  size_t allocated_bytes; /**< Total bytes allocated (approximate) */
} GCState;

/** Global GC state (protected by gc_mutex) */
static GCState gc_state = {0};

/** Mutex for thread-safe GC operations */
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

/**
 * @brief Initialize the garbage collector
 *
 * Allocates the initial object tracking array. Safe to call multiple times
 * (will clean up previous state first).
 */
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

/**
 * @brief Cleanup the garbage collector
 *
 * Releases all tracked objects and frees the tracking array.
 * Should only be called when no more values are in use.
 */
void gc_cleanup(void) {
  pthread_mutex_lock(&gc_mutex);
  KronosValue **objects = gc_state.objects;
  size_t count = gc_state.count;
  gc_state.objects = NULL;
  gc_state.count = 0;
  gc_state.capacity = 0;
  gc_state.allocated_bytes = 0;
  pthread_mutex_unlock(&gc_mutex);

  if (!objects)
    return;

  // Finalize all objects without releasing children to avoid use-after-free.
  // Children will be finalized separately when we encounter them in the array.
  for (size_t i = 0; i < count; i++) {
    KronosValue *obj = objects[i];
    if (obj)
      value_finalize(obj);
  }
  free(objects);
}

/**
 * @brief Track a newly allocated object
 *
 * Adds the object to the tracking array for leak detection and statistics.
 * Prevents duplicate tracking of the same object.
 *
 * @param val Object to track (safe to pass NULL)
 */
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

/**
 * @brief Remove an object from tracking
 *
 * Called when an object is being freed. Removes it from the tracking array
 * and updates statistics.
 *
 * @param val Object to untrack (safe to pass NULL)
 */
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

/**
 * @brief Collect cycles (placeholder for future implementation)
 *
 * Currently a no-op. Reference counting cannot detect circular references
 * (e.g., a list containing itself). This will be implemented using
 * mark-and-sweep when needed.
 *
 * TODO: Implement mark-and-sweep cycle detection
 */
void gc_collect_cycles(void) {
  // TODO: Implement mark-and-sweep for cycles
  // This will be needed when we have circular references
  // (e.g., lists containing themselves, closures with circular refs)

  // For now, we rely on pure reference counting
}

/**
 * @brief Get total allocated memory in bytes
 *
 * Returns an approximate count of memory allocated for KronosValue objects.
 * Includes the value structures and string data.
 *
 * @return Total allocated bytes
 */
size_t gc_get_allocated_bytes(void) {
  pthread_mutex_lock(&gc_mutex);
  size_t bytes = gc_state.allocated_bytes;
  pthread_mutex_unlock(&gc_mutex);
  return bytes;
}

/**
 * @brief Get the number of currently tracked objects
 *
 * Useful for debugging and leak detection.
 *
 * @return Number of tracked objects
 */
size_t gc_get_object_count(void) {
  pthread_mutex_lock(&gc_mutex);
  size_t count = gc_state.count;
  pthread_mutex_unlock(&gc_mutex);
  return count;
}
