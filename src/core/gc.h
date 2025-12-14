#ifndef KRONOS_GC_H
#define KRONOS_GC_H

#include "runtime.h"
#include <stddef.h>

// Garbage collector for reference counting and cycle detection

/**
 * @file gc.h
 * @brief Garbage collector API for Kronos
 *
 * Thread-Safety Guarantees:
 * =========================
 *
 * All GC functions are NOT thread-safe by default and require external
 * synchronization. The internal mutex (`gc_mutex`) protects the GC state
 * structure, but callers must ensure:
 *
 * 1. **Initialization/Shutdown**: `gc_init()` and `gc_cleanup()` must be called
 *    from a single thread (typically the main thread) during program
 *    initialization/shutdown. These functions are NOT safe to call
 *    concurrently.
 *
 * 2. **Tracking Operations**: `gc_track()` and `gc_untrack()` use internal
 *    mutexes for thread-safety. Multiple threads can safely call these
 *    functions concurrently. However, the caller must ensure that the
 *    KronosValue being tracked/untracked is not being modified concurrently.
 *
 * 3. **Statistics**: `gc_get_allocated_bytes()`, `gc_get_object_count()`, and
 *    `gc_stats()` use internal mutexes and are safe to call from multiple
 *    threads concurrently.
 *
 * 4. **Cycle Collection**: `gc_collect_cycles()` uses internal mutexes but
 *    should typically be called from a single thread to avoid contention.
 *
 * Best Practices:
 * - Call `gc_init()` once at program startup from the main thread
 * - Call `gc_cleanup()` once at program shutdown from the main thread
 * - All other functions can be called from any thread, but ensure KronosValue
 *   objects are not modified concurrently while being tracked/untracked
 */

/**
 * @brief Initialize the garbage collector.
 *
 * Must be called once before any GC operations.
 *
 * Thread-safety: NOT thread-safe. This function MUST be called from the main
 * thread before any other threads start calling gc_track(). The mutex is kept
 * locked during cleanup to prevent race conditions where another thread could
 * call gc_track() and corrupt the hash table state during reinitialization.
 */
void gc_init(void);

/**
 * @brief Clean up and release all GC resources.
 *
 * Should be called once at program shutdown.
 *
 * Thread-safety: NOT thread-safe. This function MUST be called from the main
 * thread. The mutex is kept locked during cleanup to prevent race conditions
 * where another thread could call gc_track() and corrupt the hash table state
 * during shutdown.
 */
void gc_cleanup(void);

/**
 * @brief Register a heap-allocated value for cycle detection tracking.
 *
 * What to track:
 * - ALL heap-allocated KronosValue objects (numbers, strings, booleans, etc.)
 * - Both container types and atomic values
 * - Objects are tracked for memory statistics and cycle detection
 *
 * When to call:
 * - Immediately after allocating a new KronosValue (via value_new_*)
 * - Before the object is used or referenced anywhere
 * - Must be called exactly once per object lifetime
 *
 * Behavior:
 * - NOT idempotent: each call increments allocation statistics and must be
 *   paired with a matching gc_untrack() to keep memory accounting correct.
 * - NULL-safe: Passing NULL is a no-op and does not affect statistics.
 * - Adds object to cycle detection tracking list
 *
 * Example usage:
 *   KronosValue *val = malloc(sizeof(KronosValue));
 *   val->type = VAL_NUMBER;
 *   val->as.number = 42.0;
 *   val->refcount = 1;
 *   gc_track(val);  // Track immediately after initialization
 *
 * @param val Value to track (may be NULL, in which case this is a no-op).
 * @note Does not modify refcount - tracking is separate from reference
 * counting.
 * @note Thread-safety: NOT thread-safe. Requires external synchronization.
 */
void gc_track(KronosValue *val);

/**
 * @brief Unregister a value from cycle detection tracking.
 *
 * When to call:
 * - Only during object destruction (when refcount reaches 0)
 * - Called by value_release() before freeing the object
 * - Must be called before the object is freed to keep statistics accurate
 * - Safe to call on untracked objects (no-op if not found)
 * - NULL-safe: Passing NULL is a no-op
 * - Balanced with gc_track(): exactly one gc_untrack() per successful
 *   gc_track(); extra calls after removal are ignored.
 * - Updates memory statistics (subtracts allocated bytes)
 * - Removes object from cycle detection tracking list
 *
 * Example usage:
 *   void value_release(KronosValue *val) {
 *       if (--val->refcount == 0) {
 *           gc_untrack(val);  // Untrack before freeing
 *           // ... free val->as.* data ...
 *           free(val);
 *       }
 *   }
 *
 * @param val Value to untrack (may be NULL, in which case this is a no-op).
 * @note Must be called before freeing the value to keep stats accurate.
 * @note Thread-safety: NOT thread-safe. Requires external synchronization.
 */
void gc_untrack(KronosValue *val);

/**
 * @brief Run cycle detection to free unreachable circular references.
 *
 * Performs mark-and-sweep garbage collection to detect and break reference
 * cycles that would otherwise leak memory. Should be called periodically or
 * when memory pressure is detected.
 *
 * @note Currently not fully implemented - reference counting handles most
 * cases.
 * @note Thread-safety: NOT thread-safe. Requires external synchronization.
 */
void gc_collect_cycles(void);

/**
 * @brief Get total bytes allocated by tracked objects.
 *
 * @return Total bytes of memory used by all tracked KronosValue objects.
 * @note Includes object headers and any associated data (e.g., string
 * contents).
 * @note Thread-safety: NOT thread-safe. Requires external synchronization.
 */
size_t gc_get_allocated_bytes(void);

/**
 * @brief Get count of currently tracked objects.
 *
 * @return Number of live KronosValue objects being tracked.
 * @note Thread-safety: NOT thread-safe. Requires external synchronization.
 */
size_t gc_get_object_count(void);

/**
 * @brief GC statistics structure
 *
 * Contains detailed memory and tracking statistics for debugging and
 * monitoring.
 */
typedef struct {
  size_t object_count;    /**< Number of currently tracked objects */
  size_t allocated_bytes; /**< Total bytes allocated by tracked objects */
  size_t array_capacity;  /**< Current capacity of the tracking array */
  size_t
      array_utilization; /**< Percentage utilization (count/capacity * 100) */
} GCStats;

/**
 * @brief Get detailed GC statistics
 *
 * Returns comprehensive statistics about the garbage collector state.
 * Useful for debugging memory issues and monitoring memory usage.
 *
 * @param stats Pointer to GCStats structure to fill (must not be NULL)
 * @note Thread-safety: NOT thread-safe. Requires external synchronization.
 */
void gc_stats(GCStats *stats);

#endif // KRONOS_GC_H
