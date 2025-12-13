/**
 * @file gc.c
 * @brief Garbage collection and memory tracking
 *
 * Provides reference-counting based garbage collection for Kronos values.
 * Tracks all allocated objects and provides statistics. Thread-safe using
 * mutexes for concurrent access.
 */

#include "gc.h"
#include <assert.h>
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
 * Hash set entry for tracking objects
 * Uses open addressing with linear probing
 */
typedef struct {
  KronosValue *object; /**< Tracked object pointer (NULL if empty slot) */
  bool is_tombstone;   /**< True if slot was deleted (for open addressing) */
} GCHashEntry;

/**
 * Garbage collector state
 * Tracks all allocated KronosValue objects for leak detection and statistics
 * Uses a hash set for O(1) track/untrack operations
 */
typedef struct {
  GCHashEntry *entries;   /**< Hash table entries (open addressing) */
  size_t count;           /**< Number of currently tracked objects */
  size_t capacity;        /**< Capacity of the hash table */
  size_t allocated_bytes; /**< Total bytes allocated (approximate) */
} GCState;

/** Global GC state (protected by gc_mutex) */
static GCState gc_state = {0};

/** Mutex for thread-safe GC operations */
static pthread_mutex_t gc_mutex;

/** Track whether mutex has been initialized */
static bool gc_mutex_initialized = false;

/**
 * @brief Report allocation failure
 *
 * Logs an error message. Used when allocation fails.
 * Does not abort - caller should handle the error.
 */
static void gc_report_allocation_failure(void) {
  fprintf(stderr,
          "Error: Failed to grow GC tracking table (current count: %zu, "
          "capacity: %zu)\n",
          gc_state.count, gc_state.capacity);
  fflush(stderr);
}

/**
 * @brief Hash function for object pointers
 *
 * DESIGN DECISION: Multiplicative hash (Knuth's prime 2654435761u) improves
 * distribution of aligned pointers which would otherwise cluster.
 *
 * EDGE CASES: NULL returns 0, prime multiplier spreads aligned pointers.
 *
 * @param ptr Object pointer to hash
 * @return Hash value for the pointer
 */
static size_t gc_hash_pointer(KronosValue *ptr) {
  uintptr_t addr = (uintptr_t)ptr;
  // Multiply by a large prime and shift
  return (size_t)(addr * 2654435761u);
}

/**
 * @brief Find slot for an object in the hash table
 *
 * Returns the index where the object should be stored or is stored.
 * Uses linear probing for collision resolution.
 *
 * @param object Object pointer to find
 * @param insert If true, find empty slot for insertion; if false, find existing
 * entry
 * @return Index of slot, or SIZE_MAX if not found (when insert=false)
 */
static size_t gc_find_slot_locked(KronosValue *object, bool insert) {
  if (gc_state.capacity == 0)
    return SIZE_MAX;

  size_t hash = gc_hash_pointer(object);
  size_t start_idx = hash % gc_state.capacity;
  size_t idx = start_idx;
  size_t first_tombstone = SIZE_MAX;

  // Linear probing
  while (true) {
    GCHashEntry *entry = &gc_state.entries[idx];

    if (entry->object == NULL) {
      // Empty slot found
      if (insert) {
        // Return first tombstone if found, otherwise this empty slot
        return (first_tombstone != SIZE_MAX) ? first_tombstone : idx;
      } else {
        // Not found
        return SIZE_MAX;
      }
    }

    if (entry->object == object && !entry->is_tombstone) {
      // Found the object
      return idx;
    }

    if (entry->is_tombstone && first_tombstone == SIZE_MAX) {
      // Remember first tombstone for potential reuse
      first_tombstone = idx;
    }

    // Move to next slot (linear probing)
    idx = (idx + 1) % gc_state.capacity;
    if (idx == start_idx) {
      // Wrapped around - table is full (shouldn't happen if we maintain load
      // factor)
      if (insert) {
        return (first_tombstone != SIZE_MAX) ? first_tombstone : SIZE_MAX;
      } else {
        return SIZE_MAX;
      }
    }
  }
}

/**
 * @brief Ensure hash table has sufficient capacity
 *
 * DESIGN DECISION: Grow when load factor > 75% (count * 4 > capacity * 3,
 * integer arithmetic). Double capacity each time for good amortized performance.
 *
 * EDGE CASES: Starts at 64 if capacity 0, checks SIZE_MAX/2 for overflow,
 * allocation failure returns false, rehashing skips tombstones, must hold mutex.
 *
 * @return true on success, false on allocation failure or overflow
 */
static bool gc_ensure_capacity_locked(void) {
  // Grow when load factor > 75% (integer arithmetic avoids floating-point)
  if (gc_state.capacity == 0 || (gc_state.count * 4 > gc_state.capacity * 3)) {
    size_t old_capacity = gc_state.capacity;
    size_t new_capacity =
        old_capacity == 0 ? INITIAL_TRACKED_CAPACITY : old_capacity * 2;

    // Check for overflow
    if (new_capacity > SIZE_MAX / 2) {
      gc_report_allocation_failure();
      return false;
    }

    GCHashEntry *old_entries = gc_state.entries;
    GCHashEntry *new_entries = calloc(new_capacity, sizeof(GCHashEntry));
    if (!new_entries) {
      gc_report_allocation_failure();
      return false;
    }

    // Rehash all existing entries
    if (old_entries) {
      for (size_t i = 0; i < old_capacity; i++) {
        if (old_entries[i].object && !old_entries[i].is_tombstone) {
          size_t hash = gc_hash_pointer(old_entries[i].object);
          size_t idx = hash % new_capacity;

          // Linear probing to find empty slot
          while (new_entries[idx].object != NULL) {
            idx = (idx + 1) % new_capacity;
          }

          new_entries[idx].object = old_entries[i].object;
          new_entries[idx].is_tombstone = false;
        }
      }
      free(old_entries);
    }

    gc_state.entries = new_entries;
    gc_state.capacity = new_capacity;
  }
  return true;
}

/**
 * @brief Shrink the hash table if it's significantly underutilized
 *
 * DESIGN DECISION: Shrink when count < capacity / 4 (25%) and capacity >
 * INITIAL_TRACKED_CAPACITY. Shrink to max(count * 2, INITIAL_TRACKED_CAPACITY)
 * for headroom. Allocation failure keeps larger capacity (wastes memory, not fatal).
 *
 * EDGE CASES: Never shrinks below initial capacity, rehashing skips tombstones,
 * must hold mutex.
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

    GCHashEntry *old_entries = gc_state.entries;
    GCHashEntry *new_entries = calloc(new_capacity, sizeof(GCHashEntry));
    if (new_entries) {
      // Rehash all existing entries
      size_t old_capacity = gc_state.capacity;
      for (size_t i = 0; i < old_capacity; i++) {
        if (old_entries[i].object && !old_entries[i].is_tombstone) {
          size_t hash = gc_hash_pointer(old_entries[i].object);
          size_t idx = hash % new_capacity;

          // Linear probing to find empty slot
          while (new_entries[idx].object != NULL) {
            idx = (idx + 1) % new_capacity;
          }

          new_entries[idx].object = old_entries[i].object;
          new_entries[idx].is_tombstone = false;
        }
      }
      free(old_entries);
      gc_state.entries = new_entries;
      gc_state.capacity = new_capacity;
    }
    // If calloc fails, we keep the larger capacity (not a fatal error)
  }
}

/**
 * @brief Initialize the garbage collector
 *
 * DESIGN DECISION: Idempotent - cleans up previous state before reinitializing
 * (finalizes objects with refcount == 1). Allows reinitialization without
 * explicit cleanup.
 *
 * EDGE CASES: Mutex init fatal if fails, allocation failure aborts, only
 * finalizes refcount == 1 objects, not thread-safe (call from main thread).
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
  if (gc_state.entries && gc_state.count > 0) {
    // Save entries and capacity before clearing state
    GCHashEntry *entries = gc_state.entries;
    size_t capacity = gc_state.capacity;
    gc_state.entries = NULL;
    gc_state.count = 0;
    gc_state.capacity = 0;
    gc_state.allocated_bytes = 0;
    pthread_mutex_unlock(&gc_mutex);

    // Finalize all tracked objects (similar to gc_cleanup)
    // Only finalize objects with refcount == 1 (only GC tracking reference)
    for (size_t i = 0; i < capacity; i++) {
      if (entries[i].object && !entries[i].is_tombstone) {
        KronosValue *obj = entries[i].object;
        if (obj->refcount == 1) {
          value_finalize(obj);
        }
      }
    }
    free(entries);

    // Re-acquire mutex for initialization
    pthread_mutex_lock(&gc_mutex);
  } else if (gc_state.entries) {
    // Hash table exists but is empty, just free it
    free(gc_state.entries);
    // Note: No need to set gc_state.entries = NULL here since memset follows
  }

  memset(&gc_state, 0, sizeof(GCState));
  if (!gc_ensure_capacity_locked()) {
    // Allocation failed during init - this is fatal
    fprintf(stderr, "Fatal: Failed to initialize GC tracking table\n");
    pthread_mutex_unlock(&gc_mutex);
    abort(); // Init failure is fatal
  }
  pthread_mutex_unlock(&gc_mutex);
}

/**
 * @brief Cleanup the garbage collector
 *
 * DESIGN DECISION: Uses value_finalize() (not value_release()) to avoid
 * use-after-free. Finalize doesn't recursively release children (prevents
 * double-free). Only finalizes refcount == 1 objects (external refs cleaned
 * up naturally).
 *
 * EDGE CASES: Destroys mutex, idempotent after first call, not thread-safe
 * (call from main thread).
 */
void gc_cleanup(void) {
  pthread_mutex_lock(&gc_mutex);
  GCHashEntry *entries = gc_state.entries;
  size_t capacity = gc_state.capacity;
  gc_state.entries = NULL;
  gc_state.count = 0;
  gc_state.capacity = 0;
  gc_state.allocated_bytes = 0;
  pthread_mutex_unlock(&gc_mutex);

  if (!entries)
    return;

  // Finalize all objects without releasing children to avoid use-after-free.
  // Children will be finalized separately when we encounter them in the hash
  // table. Only finalize objects with refcount == 1 (only GC tracking
  // reference). Objects with refcount > 1 have external references and should
  // not be freed.
  for (size_t i = 0; i < capacity; i++) {
    if (entries[i].object && !entries[i].is_tombstone) {
      KronosValue *obj = entries[i].object;
      if (obj->refcount == 1) {
        // Only GC tracking reference, safe to finalize
        // Objects with refcount > 1 have external references and will be
        // cleaned up naturally when their refcount reaches 0
        value_finalize(obj);
      }
    }
  }
  free(entries);

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

  // Check if object is already tracked using hash set (O(1) lookup)
  size_t idx = gc_find_slot_locked(val, false);
  if (idx != SIZE_MAX) {
    // Already tracked, skip
#ifdef DEBUG
    // In debug builds, this indicates a bug (double-tracking)
    fprintf(stderr, "Warning: gc_track() called on already-tracked object %p\n",
            (void *)val);
#endif
    pthread_mutex_unlock(&gc_mutex);
    return;
  }

  // Object not found, ensure capacity and add it
  if (!gc_ensure_capacity_locked()) {
    // Allocation failed - log error and return early (don't abort)
    fprintf(stderr,
            "Warning: gc_track() failed to allocate memory for object %p\n",
            (void *)val);
    pthread_mutex_unlock(&gc_mutex);
    return;
  }
  idx = gc_find_slot_locked(val, true);
  if (idx == SIZE_MAX) {
    // Should not happen if capacity was ensured, but handle gracefully
    fprintf(stderr, "Warning: gc_track() failed to find slot for object %p\n",
            (void *)val);
    pthread_mutex_unlock(&gc_mutex);
    return;
  }

  gc_state.entries[idx].object = val;
  gc_state.entries[idx].is_tombstone = false;
  gc_state.count++;
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

  // Find object in hash set (O(1) average case)
  size_t idx = gc_find_slot_locked(val, false);
  if (idx == SIZE_MAX) {
#ifdef DEBUG
    // Assert: object should be tracked (catch use-after-free or double-untrack)
    fprintf(stderr,
            "Warning: gc_untrack() called on untracked object %p (possible "
            "use-after-free or double-untrack)\n",
            (void *)val);
#endif
    pthread_mutex_unlock(&gc_mutex);
    return;
  }

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
      gc_state.allocated_bytes -= val->as.list.capacity * sizeof(KronosValue *);
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

  // Remove from hash set by marking as tombstone (O(1))
  gc_state.entries[idx].object = NULL;
  gc_state.entries[idx].is_tombstone = true;
  gc_state.count--;

  // Shrink hash table if significantly underutilized
  gc_shrink_if_needed_locked();

  pthread_mutex_unlock(&gc_mutex);
}

/**
 * @brief Mark all objects reachable from a given object
 *
 * Helper function for mark-and-sweep. Recursively marks all objects
 * reachable from the given object.
 *
 * @param val Object to start marking from
 * @param marked Hash table mapping object pointers to marked status
 * @param capacity Capacity of the hash table
 * @param entries Hash table entries
 */
static void gc_mark_reachable(KronosValue *val, bool *marked, size_t capacity,
                              GCHashEntry *entries) {
  if (!val)
    return;

  // Find index of this object in the hash table
  size_t hash = gc_hash_pointer(val);
  size_t start_idx = hash % capacity;
  size_t idx = start_idx;

  // Linear probing to find the object
  while (true) {
    if (entries[idx].object == val && !entries[idx].is_tombstone) {
      // Found the object
      break;
    }
    if (entries[idx].object == NULL) {
      // Not tracked
      return;
    }
    idx = (idx + 1) % capacity;
    if (idx == start_idx) {
      // Wrapped around, not found
      return;
    }
  }

  // If already marked, return
  if (marked[idx])
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
          gc_mark_reachable(val->as.list.items[i], marked, capacity, entries);
        }
      }
    }
    break;
  case VAL_MAP: {
    // Mark all keys and values in the map
    MapEntry *map_entries = (MapEntry *)val->as.map.entries;
    if (map_entries) {
      for (size_t i = 0; i < val->as.map.capacity; i++) {
        if (map_entries[i].key && !map_entries[i].is_tombstone) {
          gc_mark_reachable(map_entries[i].key, marked, capacity, entries);
        }
        if (map_entries[i].value) {
          gc_mark_reachable(map_entries[i].value, marked, capacity, entries);
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

  if (gc_state.count == 0 || gc_state.capacity == 0) {
    pthread_mutex_unlock(&gc_mutex);
    return;
  }

  // Allocate array to track which objects are marked (indexed by hash table
  // slot)
  bool *marked = calloc(gc_state.capacity, sizeof(bool));
  if (!marked) {
    pthread_mutex_unlock(&gc_mutex);
    return; // Can't allocate, skip cycle collection
  }

  // Mark phase: Mark all objects reachable from roots (objects with refcount >
  // 1)
  for (size_t i = 0; i < gc_state.capacity; i++) {
    if (gc_state.entries[i].object && !gc_state.entries[i].is_tombstone) {
      KronosValue *obj = gc_state.entries[i].object;
      if (obj->refcount > 1) {
        // This object has external references, mark it and all reachable
        // objects
        gc_mark_reachable(obj, marked, gc_state.capacity, gc_state.entries);
      }
    }
  }

  // Sweep phase: Free unmarked objects (they're part of cycles)
  // Iterate through hash table
  for (size_t i = 0; i < gc_state.capacity; i++) {
    if (gc_state.entries[i].object && !gc_state.entries[i].is_tombstone) {
      KronosValue *obj = gc_state.entries[i].object;
      if (!marked[i] && obj->refcount > 0) {
        // Unmarked object with refcount > 0 is part of a cycle
        // Decrement refcount and free if it reaches 0
        // Note: We can't use value_release() here because it would try to
        // untrack, which modifies the hash table we're iterating. Instead, we
        // manually decrement refcount and finalize.
        obj->refcount--;
        if (obj->refcount == 0) {
          // Remove from hash table by marking as tombstone
          gc_state.entries[i].object = NULL;
          gc_state.entries[i].is_tombstone = true;
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

/**
 * @brief Get detailed GC statistics
 *
 * Returns comprehensive statistics about the garbage collector state.
 * Useful for debugging memory issues and monitoring memory usage.
 *
 * @param stats Pointer to GCStats structure to fill (must not be NULL)
 */
void gc_stats(GCStats *stats) {
  if (!stats)
    return;

  pthread_mutex_lock(&gc_mutex);
  stats->object_count = gc_state.count;
  stats->allocated_bytes = gc_state.allocated_bytes;
  stats->array_capacity = gc_state.capacity;
  stats->array_utilization =
      gc_state.capacity > 0
          ? (size_t)((gc_state.count * 100) / gc_state.capacity)
          : 0;
  pthread_mutex_unlock(&gc_mutex);
}
