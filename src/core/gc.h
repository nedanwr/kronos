#ifndef KRONOS_GC_H
#define KRONOS_GC_H

#include "runtime.h"
#include <stddef.h>

// Garbage collector for reference counting and cycle detection

/**
 * @brief Initialize the garbage collector.
 *
 * Must be called once before any GC operations.
 * Thread-safety: NOT thread-safe. Call from main thread during initialization.
 */
void gc_init(void);

/**
 * @brief Clean up and release all GC resources.
 *
 * Should be called once at program shutdown.
 * Thread-safety: NOT thread-safe. Call from main thread during shutdown.
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

#endif // KRONOS_GC_H
