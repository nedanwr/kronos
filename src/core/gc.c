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
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Forward declaration of MapEntry (defined in runtime.c)
typedef struct {
  KronosValue *key;
  KronosValue *value;
  bool is_tombstone;
} MapEntry;

/**
 * Initial capacity for the object tracking array
 *
 * Chosen to balance memory usage for small programs while minimizing
 * reallocations. The array grows automatically as needed (doubles when full),
 * so a smaller initial size (64) is reasonable for typical workloads.
 * This is ~5KB of memory (64 * sizeof(KronosValue*) = 64 * 8 = 512 bytes).
 *
 * For comparison:
 * - Compiler bytecode: 256 initial capacity
 * - Constant pool: 32 initial capacity
 * - Maps: 8 initial capacity
 *
 * 64 provides a good middle ground - small enough for tiny programs,
 * large enough to avoid frequent resizes for typical use cases.
 */
#define INITIAL_TRACKED_CAPACITY 64

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
static pthread_mutex_t gc_mutex;

/** Track whether mutex has been initialized */
static bool gc_mutex_initialized = false;

static void gc_abort_allocation(void) {
  fprintf(stderr,
          "Fatal: Failed to grow GC tracking table (current count: %zu, "
          "capacity: %zu)\n",
          gc_state.count, gc_state.capacity);
  fflush(stderr);
  pthread_mutex_unlock(&gc_mutex);
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
 * @brief Shrink the objects array if it's significantly underutilized
 *
 * Shrinks the array when count < capacity / 4 and capacity >
 * INITIAL_TRACKED_CAPACITY. This prevents the array from staying large after
 * many deallocations. Must be called with gc_mutex locked.
 */
static void gc_shrink_if_needed_locked(void) {
  // Only shrink if:
  // 1. Count is less than 25% of capacity (significant underutilization)
  // 2. Capacity is above initial capacity (don't shrink below initial)
  if (gc_state.count * 4 < gc_state.capacity &&
      gc_state.capacity > INITIAL_TRACKED_CAPACITY) {
    // Shrink to max(count * 2, INITIAL_TRACKED_CAPACITY)
    // This provides headroom for future allocations
    size_t new_capacity = gc_state.count * 2;
    if (new_capacity < INITIAL_TRACKED_CAPACITY) {
      new_capacity = INITIAL_TRACKED_CAPACITY;
    }

    KronosValue **new_objects =
        realloc(gc_state.objects, new_capacity * sizeof(KronosValue *));
    if (new_objects) {
      // Only update if realloc succeeded (if it fails, keep old capacity)
      gc_state.objects = new_objects;
      gc_state.capacity = new_capacity;
    }
    // If realloc fails, we keep the larger capacity (not a fatal error)
  }
}

/**
 * @brief Initialize the garbage collector
 *
 * Allocates the initial object tracking array. Safe to call multiple times
 * (will clean up previous state first).
 */
void gc_init(void) {
  // Initialize mutex if not already initialized
  if (!gc_mutex_initialized) {
    if (pthread_mutex_init(&gc_mutex, NULL) != 0) {
      fprintf(stderr, "Fatal: Failed to initialize GC mutex\n");
      abort();
    }
    gc_mutex_initialized = true;
  }

  pthread_mutex_lock(&gc_mutex);
  // Free any previously allocated memory if gc_init is called multiple times
  if (gc_state.objects && gc_state.count > 0) {
    // Save objects and count before clearing state
    KronosValue **objects = gc_state.objects;
    size_t count = gc_state.count;
    gc_state.objects = NULL;
    gc_state.count = 0;
    gc_state.capacity = 0;
    gc_state.allocated_bytes = 0;
    pthread_mutex_unlock(&gc_mutex);

    // Finalize all tracked objects (similar to gc_cleanup)
    // Only finalize objects with refcount == 1 (only GC tracking reference)
    for (size_t i = 0; i < count; i++) {
      KronosValue *obj = objects[i];
      if (obj && obj->refcount == 1) {
        value_finalize(obj);
      }
    }
    free(objects);

    // Re-acquire mutex for initialization
    pthread_mutex_lock(&gc_mutex);
  } else if (gc_state.objects) {
    // Array exists but is empty, just free it
    free(gc_state.objects);
    // Note: No need to set gc_state.objects = NULL here since memset follows
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
  // Only finalize objects with refcount == 1 (only GC tracking reference).
  // Objects with refcount > 1 have external references and should not be freed.
  for (size_t i = 0; i < count; i++) {
    KronosValue *obj = objects[i];
    if (obj && obj->refcount == 1) {
      // Only GC tracking reference, safe to finalize
      // Objects with refcount > 1 have external references and will be
      // cleaned up naturally when their refcount reaches 0
      value_finalize(obj);
    }
  }
  free(objects);

  // Destroy the mutex to prevent resource leak
  if (gc_mutex_initialized) {
    pthread_mutex_destroy(&gc_mutex);
    gc_mutex_initialized = false;
  }
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

  // Add size of type-specific data
  switch (val->type) {
  case VAL_STRING:
    gc_state.allocated_bytes += val->as.string.length + 1;
    break;
  case VAL_LIST:
    // Track list item array: capacity * sizeof(KronosValue*)
    if (val->as.list.capacity > 0) {
      gc_state.allocated_bytes += val->as.list.capacity * sizeof(KronosValue *);
    }
    break;
  case VAL_MAP: {
    // Track map entries array: capacity * sizeof(MapEntry)
    // MapEntry contains: KronosValue* key, KronosValue* value, bool
    // is_tombstone Size is approximately: capacity * (sizeof(KronosValue*) * 2
    // + sizeof(bool)) Using sizeof(void*) * 2 + sizeof(bool) for portability
    if (val->as.map.capacity > 0) {
      gc_state.allocated_bytes +=
          val->as.map.capacity * (sizeof(void *) * 2 + sizeof(bool));
    }
    break;
  }
  case VAL_FUNCTION:
    // Track function bytecode buffer
    if (val->as.function.bytecode && val->as.function.length > 0) {
      gc_state.allocated_bytes += val->as.function.length;
    }
    break;
  default:
    break;
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

      // Subtract size of type-specific data
      switch (val->type) {
      case VAL_STRING:
        gc_state.allocated_bytes -= val->as.string.length + 1;
        break;
      case VAL_LIST:
        // Subtract list item array size
        if (val->as.list.capacity > 0) {
          gc_state.allocated_bytes -=
              val->as.list.capacity * sizeof(KronosValue *);
        }
        break;
      case VAL_MAP: {
        // Subtract map entries array size
        if (val->as.map.capacity > 0) {
          gc_state.allocated_bytes -=
              val->as.map.capacity * (sizeof(void *) * 2 + sizeof(bool));
        }
        break;
      }
      case VAL_FUNCTION:
        // Subtract function bytecode buffer size
        if (val->as.function.bytecode && val->as.function.length > 0) {
          gc_state.allocated_bytes -= val->as.function.length;
        }
        break;
      default:
        break;
      }

      // Remove from array by swapping with last element (O(1) instead of O(n))
      gc_state.objects[i] = gc_state.objects[gc_state.count - 1];
      gc_state.objects[gc_state.count - 1] = NULL;
      gc_state.count--;

      // Shrink array if significantly underutilized
      gc_shrink_if_needed_locked();

      pthread_mutex_unlock(&gc_mutex);
      return;
    }
  }
  pthread_mutex_unlock(&gc_mutex);
}

/**
 * @brief Mark all objects reachable from a given object
 *
 * Helper function for mark-and-sweep. Recursively marks all objects
 * reachable from the given object.
 *
 * @param val Object to start marking from
 * @param marked Array of booleans indicating which objects are marked
 * @param object_count Number of tracked objects
 * @param objects Array of tracked objects
 */
static void gc_mark_reachable(KronosValue *val, bool *marked,
                              size_t object_count, KronosValue **objects) {
  if (!val)
    return;

  // Find index of this object in the tracked objects array
  size_t idx = SIZE_MAX;
  for (size_t i = 0; i < object_count; i++) {
    if (objects[i] == val) {
      idx = i;
      break;
    }
  }

  // If not tracked or already marked, return
  if (idx == SIZE_MAX || marked[idx])
    return;

  // Mark this object
  marked[idx] = true;

  // Recursively mark reachable objects based on type
  switch (val->type) {
  case VAL_LIST:
    // Mark all items in the list
    if (val->as.list.items) {
      for (size_t i = 0; i < val->as.list.count; i++) {
        if (val->as.list.items[i]) {
          gc_mark_reachable(val->as.list.items[i], marked, object_count,
                            objects);
        }
      }
    }
    break;
  case VAL_MAP: {
    // Mark all keys and values in the map
    MapEntry *entries = (MapEntry *)val->as.map.entries;
    if (entries) {
      for (size_t i = 0; i < val->as.map.capacity; i++) {
        if (entries[i].key && !entries[i].is_tombstone) {
          gc_mark_reachable(entries[i].key, marked, object_count, objects);
        }
        if (entries[i].value) {
          gc_mark_reachable(entries[i].value, marked, object_count, objects);
        }
      }
    }
    break;
  }
  default:
    // Other types don't contain references to other KronosValues
    break;
  }
}

/**
 * @brief Collect cycles using mark-and-sweep algorithm
 *
 * Performs mark-and-sweep garbage collection to detect and free circular
 * references that reference counting cannot handle. Objects with refcount > 1
 * are considered roots (have external references). All objects reachable from
 * roots are marked. Unmarked objects with refcount > 0 are part of cycles
 * and are freed.
 */
void gc_collect_cycles(void) {
  pthread_mutex_lock(&gc_mutex);

  if (gc_state.count == 0) {
    pthread_mutex_unlock(&gc_mutex);
    return;
  }

  // Allocate mark array
  bool *marked = calloc(gc_state.count, sizeof(bool));
  if (!marked) {
    pthread_mutex_unlock(&gc_mutex);
    return; // Out of memory, skip cycle collection
  }

  // Mark phase: Mark all objects reachable from roots (refcount > 1)
  for (size_t i = 0; i < gc_state.count; i++) {
    KronosValue *obj = gc_state.objects[i];
    if (obj && obj->refcount > 1) {
      // This object has external references, mark it and all reachable objects
      gc_mark_reachable(obj, marked, gc_state.count, gc_state.objects);
    }
  }

  // Sweep phase: Free unmarked objects (they're part of cycles)
  // Iterate backwards to safely remove elements
  for (size_t i = gc_state.count; i > 0; i--) {
    size_t idx = i - 1;
    KronosValue *obj = gc_state.objects[idx];
    if (obj && !marked[idx] && obj->refcount > 0) {
      // Unmarked object with refcount > 0 is part of a cycle
      // Decrement refcount and free if it reaches 0
      // Note: We can't use value_release() here because it would try to
      // untrack, which modifies the array we're iterating. Instead, we
      // manually decrement refcount and finalize.
      obj->refcount--;
      if (obj->refcount == 0) {
        // Remove from tracking array first
        gc_state.objects[idx] = gc_state.objects[gc_state.count - 1];
        gc_state.objects[gc_state.count - 1] = NULL;
        gc_state.count--;

        // Update allocated bytes
        gc_state.allocated_bytes -= sizeof(KronosValue);
        switch (obj->type) {
        case VAL_STRING:
          gc_state.allocated_bytes -= obj->as.string.length + 1;
          break;
        case VAL_LIST:
          if (obj->as.list.capacity > 0) {
            gc_state.allocated_bytes -=
                obj->as.list.capacity * sizeof(KronosValue *);
          }
          break;
        case VAL_MAP:
          if (obj->as.map.capacity > 0) {
            gc_state.allocated_bytes -=
                obj->as.map.capacity * (sizeof(void *) * 2 + sizeof(bool));
          }
          break;
        case VAL_FUNCTION:
          if (obj->as.function.bytecode && obj->as.function.length > 0) {
            gc_state.allocated_bytes -= obj->as.function.length;
          }
          break;
        default:
          break;
        }

        // Unlock mutex before finalizing (value_finalize may need GC)
        pthread_mutex_unlock(&gc_mutex);
        value_finalize(obj);
        pthread_mutex_lock(&gc_mutex);
      }
    }
  }

  free(marked);
  pthread_mutex_unlock(&gc_mutex);
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
