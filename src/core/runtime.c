/**
 * @file runtime.c
 * @brief Runtime value system and memory management
 *
 * Provides the core value representation system for Kronos. Handles:
 * - Value creation (numbers, strings, booleans, lists, functions)
 * - Reference counting for automatic memory management
 * - String interning for optimization
 * - Value comparison and type checking
 * - Value printing and formatting
 */

// Enable POSIX functions like strdup on Linux
#define _POSIX_C_SOURCE 200809L

#include "runtime.h"
#include "gc.h"
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Epsilon value for floating-point comparisons
 *
 * WHY: Floating-point rounding errors make exact equality unreliable. Epsilon
 * comparison handles cases like 0.1 + 0.2 != 0.3.
 *
 * DESIGN DECISION: 1e-9 balances precision and practicality. May be too strict
 * for very large numbers (>1e9) or too lenient for very small (<1e-9). Relative
 * epsilon would be more accurate (see ROADMAP.md).
 *
 * EDGE CASES: NaN != NaN (correct), INF comparisons handled via NaN fallback,
 * very large numbers may have precision issues.
 */
#define VALUE_COMPARE_EPSILON (1e-9)

/**
 * Size of the string interning hash table
 *
 * DESIGN DECISION: Fixed-size 1024 (~8KB) balances memory with collision rate.
 * Linear probing handles collisions. Dynamic growth would be better for large
 * programs (see ROADMAP.md).
 *
 * EDGE CASES: Collisions handled via linear probing, table full causes O(n)
 * worst-case, thread-safe via intern_mutex.
 */
#define INTERN_TABLE_SIZE 1024

/** Maximum depth for printing nested structures to prevent stack overflow */
#define VALUE_PRINT_MAX_DEPTH 64

/** Maximum depth for comparing nested structures to prevent stack overflow */
#define VALUE_EQUALS_MAX_DEPTH 64

/** Map entry structure for hash table implementation */
typedef struct {
  KronosValue *key;
  KronosValue *value;
  bool is_tombstone;
} MapEntry;

/** Hash table for string interning (reduces memory for duplicate strings) */
static KronosValue *intern_table[INTERN_TABLE_SIZE] = {0};

/** Mutex for thread-safe intern table operations */
static pthread_mutex_t intern_mutex = PTHREAD_MUTEX_INITIALIZER;

/** Condition variable for waiting on initialization completion */
static pthread_cond_t init_cond = PTHREAD_COND_INITIALIZER;

/** Reference counter for runtime initialization (allows multiple VMs to share
 * runtime) */
static size_t runtime_refcount = 0;

/** Flag indicating that initialization is currently in progress */
static bool init_in_progress = false;

/**
 * @brief Hash function for strings (FNV-1a algorithm)
 *
 * DESIGN DECISION: FNV-1a chosen for simplicity, speed, and good distribution.
 * 32-bit variant sufficient for 1024-entry table. Standard constants used.
 *
 * EDGE CASES: Empty strings hash to initial value, NULL is undefined behavior,
 * O(n) performance for long strings.
 *
 * @param str String to hash (must not be NULL)
 * @param len Length of the string
 * @return 32-bit hash value
 */
static uint32_t hash_string(const char *str, size_t len) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < len; i++) {
    hash ^= (uint8_t)str[i];
    hash *= 16777619;
  }
  return hash;
}

/**
 * @brief Initialize the runtime system
 *
 * DESIGN DECISION: Reference counting (vs singleton) allows proper cleanup and
 * thread-safe sharing across multiple VMs. First call initializes, subsequent
 * calls increment refcount.
 *
 * EDGE CASES: Multiple VMs share runtime, cleanup when refcount reaches 0,
 * thread-safe via intern_mutex and init_cond, must call runtime_cleanup() after
 * all VMs freed. Uses init_in_progress flag and condition variable to prevent
 * double-initialization race condition when multiple threads call runtime_init()
 * concurrently.
 */
void runtime_init(void) {
  pthread_mutex_lock(&intern_mutex);

  // Wait if initialization is in progress
  while (init_in_progress) {
    pthread_cond_wait(&init_cond, &intern_mutex);
  }

  if (runtime_refcount == 0) {
    // First initialization - clear intern table and initialize GC
    memset(intern_table, 0, sizeof(intern_table));

    // Set flag to indicate initialization is in progress
    init_in_progress = true;
    pthread_mutex_unlock(&intern_mutex);

    // Call gc_init() without holding the mutex (may be long-running)
    gc_init();

    // Re-acquire mutex to update state
    pthread_mutex_lock(&intern_mutex);
    init_in_progress = false;
    runtime_refcount++;

    // Signal waiting threads that initialization is complete
    pthread_cond_broadcast(&init_cond);
  } else {
    // Runtime already initialized, just increment refcount
    runtime_refcount++;
  }

  pthread_mutex_unlock(&intern_mutex);
}

/**
 * @brief Release intern table references while holding intern_mutex
 *
 * @param count_external_refs Whether to count entries with refcount > 1
 * @return Number of entries that had references beyond the intern table
 */
static size_t runtime_release_interned_strings_locked(bool count_external_refs) {
  size_t active_refs = 0;
  for (size_t i = 0; i < INTERN_TABLE_SIZE; i++) {
    KronosValue *entry = intern_table[i];
    if (!entry) {
      continue;
    }

    if (count_external_refs && entry->refcount > 1) {
      active_refs++;
    }

    // Release the intern table's owning reference.
    value_release(entry);
    intern_table[i] = NULL;
  }

  return active_refs;
}

void runtime_release_interned_strings(void) {
  pthread_mutex_lock(&intern_mutex);
  (void)runtime_release_interned_strings_locked(false);
  pthread_mutex_unlock(&intern_mutex);
}

/**
 * @brief Cleanup the runtime system
 *
 * Releases all interned strings and shuts down the garbage collector.
 * Uses reference counting - only performs actual cleanup when the last
 * reference is released, allowing multiple VMs to share the runtime.
 * IMPORTANT: This must only be called after all external references to
 * interned strings have been released, otherwise values may be freed
 * prematurely.
 */
void runtime_cleanup(void) {
  pthread_mutex_lock(&intern_mutex);
  if (runtime_refcount == 0) {
    // Already cleaned up or never initialized
    pthread_mutex_unlock(&intern_mutex);
    return;
  }

  runtime_refcount--;
  if (runtime_refcount > 0) {
    // Other VMs still using the runtime, don't cleanup yet
    pthread_mutex_unlock(&intern_mutex);
    return;
  }

  // Last reference - perform actual cleanup
  size_t active_refs = runtime_release_interned_strings_locked(true);
  pthread_mutex_unlock(&intern_mutex);

  if (active_refs > 0) {
    fprintf(stderr,
            "Warning: runtime_cleanup() called with %zu interned strings still "
            "referenced externally. "
            "These may be freed prematurely.\n",
            active_refs);
  }

  gc_cleanup();
}

/**
 * @brief Create a new number value
 *
 * Allocates a KronosValue representing a floating-point number.
 * The value is tracked by the garbage collector and uses reference counting.
 *
 * EDGE CASES:
 * - NaN: Stored as-is; comparisons use epsilon (NaN != NaN as expected)
 * - Infinity: Stored as-is; +/-INF handled correctly in comparisons
 * - Subnormal numbers: Preserved (no normalization)
 * - Zero: Both +0.0 and -0.0 are stored (IEEE 754 distinguishes them, but
 *   our equality comparison treats them as equal via epsilon)
 *
 * @param num The numeric value (can be NaN, INF, or any valid double)
 * @return New value, or NULL on allocation failure
 */
KronosValue *value_new_number(double num) {
  KronosValue *val = malloc(sizeof(KronosValue));
  if (!val)
    return NULL;

  val->type = VAL_NUMBER;
  val->refcount = 1;
  val->as.number = num;

  gc_track(val);
  return val;
}

/**
 * @brief Create a new string value
 *
 * Allocates a KronosValue containing a copy of the provided string.
 * The string data is stored with a null terminator for C compatibility.
 *
 * @param str String data (may contain null bytes, will be copied)
 * @param len Length of the string (not including null terminator)
 * @return New value, or NULL on allocation failure
 */
KronosValue *value_new_string(const char *str, size_t len) {
  KronosValue *val = malloc(sizeof(KronosValue));
  if (!val)
    return NULL;

  val->type = VAL_STRING;
  val->refcount = 1;
  val->as.string.data = malloc(len + 1);
  if (!val->as.string.data) {
    free(val);
    return NULL;
  }

  memcpy(val->as.string.data, str, len);
  val->as.string.data[len] = '\0';
  val->as.string.length = len;
  val->as.string.hash = hash_string(str, len);

  gc_track(val);
  return val;
}

/**
 * @brief Create a new boolean value
 *
 * @param val Boolean value (true or false)
 * @return New value, or NULL on allocation failure
 */
KronosValue *value_new_bool(bool val) {
  KronosValue *v = malloc(sizeof(KronosValue));
  if (!v)
    return NULL;

  v->type = VAL_BOOL;
  v->refcount = 1;
  v->as.boolean = val;

  gc_track(v);
  return v;
}

/**
 * @brief Create a new nil (null) value
 *
 * Represents the absence of a value. Used for uninitialized variables
 * and as a default return value.
 *
 * @return New nil value, or NULL on allocation failure
 */
KronosValue *value_new_nil(void) {
  KronosValue *val = malloc(sizeof(KronosValue));
  if (!val)
    return NULL;

  val->type = VAL_NIL;
  val->refcount = 1;

  gc_track(val);
  return val;
}

/**
 * @brief Create a new function value
 *
 * Stores compiled bytecode for a user-defined function. The bytecode
 * and parameter names are copied into the value.
 *
 * @param bytecode Function bytecode (will be copied)
 * @param length Length of bytecode in bytes
 * @param arity Number of parameters the function expects
 * @param param_names Array of parameter name strings (will be copied), or NULL
 * @return New function value, or NULL on allocation failure
 */
KronosValue *value_new_function(uint8_t *bytecode, size_t length, int arity,
                                int required_arity, bool has_variadic,
                                char **param_names,
                                KronosValue **param_defaults) {
  if (!bytecode || length == 0)
    return NULL;

  KronosValue *val = malloc(sizeof(KronosValue));
  if (!val)
    return NULL;

  uint8_t *buffer = malloc(length);
  if (!buffer) {
    free(val);
    return NULL;
  }
  memcpy(buffer, bytecode, length);

  // Copy parameter names if provided
  char **names_copy = NULL;
  if (param_names && arity > 0) {
    names_copy = malloc(sizeof(char *) * arity);
    if (!names_copy) {
      free(buffer);
      free(val);
      return NULL;
    }
    for (int i = 0; i < arity; i++) {
      names_copy[i] = strdup(param_names[i]);
      if (!names_copy[i]) {
        // Cleanup on allocation failure
        for (int j = 0; j < i; j++) {
          free(names_copy[j]);
        }
        free(names_copy);
        free(buffer);
        free(val);
        return NULL;
      }
    }
  }

  // Copy default values if provided
  KronosValue **defaults_copy = NULL;
  if (param_defaults && arity > 0) {
    defaults_copy = malloc(sizeof(KronosValue *) * arity);
    if (!defaults_copy) {
      if (names_copy) {
        for (int i = 0; i < arity; i++) {
          free(names_copy[i]);
        }
        free(names_copy);
      }
      free(buffer);
      free(val);
      return NULL;
    }
    for (int i = 0; i < arity; i++) {
      if (param_defaults[i]) {
        value_retain(param_defaults[i]);
        defaults_copy[i] = param_defaults[i];
      } else {
        defaults_copy[i] = NULL;
      }
    }
  }

  val->type = VAL_FUNCTION;
  val->refcount = 1;
  val->as.function.bytecode = buffer;
  val->as.function.length = length;
  val->as.function.arity = arity;
  val->as.function.required_arity = required_arity;
  val->as.function.has_variadic = has_variadic;
  val->as.function.param_names = names_copy;
  val->as.function.param_defaults = defaults_copy;

  gc_track(val);
  return val;
}

/**
 * @brief Create a new list value
 *
 * Allocates a dynamically-growing array to hold list elements.
 * Starts with the specified capacity (or 4 if 0) and grows as needed.
 *
 * @param initial_capacity Initial capacity (0 means use default of 4)
 * @return New empty list, or NULL on allocation failure
 */
KronosValue *value_new_list(size_t initial_capacity) {
  size_t capacity = initial_capacity == 0 ? 4 : initial_capacity;

  KronosValue *val = malloc(sizeof(KronosValue));
  if (!val)
    return NULL;

  KronosValue **items = calloc(capacity, sizeof(KronosValue *));
  if (!items) {
    free(val);
    return NULL;
  }

  val->type = VAL_LIST;
  val->refcount = 1;
  val->as.list.items = items;
  val->as.list.count = 0;
  val->as.list.capacity = capacity;

  gc_track(val);
  return val;
}

/**
 * @brief Create a new channel value
 *
 * Wraps a channel for inter-thread communication (future feature).
 * The channel is not owned by the value and must be managed separately.
 *
 * @param channel Channel to wrap (must not be NULL)
 * @return New channel value, or NULL on allocation failure
 */
KronosValue *value_new_channel(Channel *channel) {
  if (!channel)
    return NULL;

  KronosValue *val = malloc(sizeof(KronosValue));
  if (!val)
    return NULL;

  val->type = VAL_CHANNEL;
  val->refcount = 1;
  val->as.channel = channel;

  gc_track(val);
  return val;
}

/**
 * @brief Create a new range value
 *
 * Creates a range object representing values from start to end (inclusive)
 * with the given step. The step defaults to 1.0 if 0.0 is provided.
 *
 * DESIGN DECISION: We store the range parameters (start, end, step) rather than
 * pre-computing all values. This allows lazy evaluation during iteration and
 * supports very large ranges without memory overhead. The VM's iteration logic
 * handles the actual value generation.
 *
 * EDGE CASES:
 * - Step of 0.0: Invalid (would cause infinite loop), defaults to 1.0 with warning
 * - Negative steps: Supported for reverse iteration (e.g., range 10 to 1 by -1)
 * - Empty ranges: range 1 to 10 by -1 produces no values (correct behavior)
 * - Floating-point steps: Supported (e.g., range 0 to 1 by 0.1)
 * - Very large ranges: No memory overhead (values computed on-demand)
 * - NaN/Infinity: Stored as-is; iteration logic must handle these cases
 *
 * Negative steps are supported for reverse iteration. The iteration logic in
 * the VM handles negative steps correctly by checking the step direction. No
 * validation is performed here as ranges with mismatched step direction and
 * start/end relationship simply result in empty ranges (e.g., range 1 to 10 by
 * -1 produces no values, which is correct behavior).
 *
 * @param start Starting value (inclusive)
 * @param end Ending value (inclusive)
 * @param step Step size (defaults to 1.0 if 0.0, negative steps allowed for
 * reverse iteration)
 * @return New range value, or NULL on allocation failure
 */
KronosValue *value_new_range(double start, double end, double step) {
  KronosValue *val = malloc(sizeof(KronosValue));
  if (!val)
    return NULL;

  val->type = VAL_RANGE;
  val->refcount = 1;
  val->as.range.start = start;
  val->as.range.end = end;
  // Step of 0.0 is invalid (would cause infinite loop). Default to 1.0 with
  // warning.
  if (step == 0.0) {
    fprintf(stderr,
            "Warning: Range step of 0.0 is invalid, defaulting to 1.0\n");
    val->as.range.step = 1.0;
  } else {
    val->as.range.step = step;
  }

  gc_track(val);
  return val;
}

/**
 * @brief Create a new tuple value
 *
 * Creates an immutable fixed-size container for multiple values. Tuples are
 * used for multiple return values and destructuring assignments. The items
 * array is copied and each item is retained.
 *
 * DESIGN DECISION: Tuples are immutable (no append, set operations). This
 * distinguishes them from lists and makes their semantics clearer for
 * multiple return values.
 *
 * EDGE CASES:
 * - count of 0: Creates empty tuple (valid but rarely useful)
 * - count of 1: Creates single-element tuple (distinct from the value itself)
 * - NULL items: Returns NULL (invalid input)
 * - NULL item in array: Stored as-is (caller's responsibility)
 *
 * @param items Array of values to include (will be copied and retained)
 * @param count Number of items
 * @return New tuple value, or NULL on allocation failure or NULL items
 */
KronosValue *value_new_tuple(KronosValue **items, size_t count) {
  if (!items && count > 0)
    return NULL;

  KronosValue *val = malloc(sizeof(KronosValue));
  if (!val)
    return NULL;

  KronosValue **tuple_items = NULL;
  if (count > 0) {
    tuple_items = malloc(count * sizeof(KronosValue *));
    if (!tuple_items) {
      free(val);
      return NULL;
    }
    for (size_t i = 0; i < count; i++) {
      tuple_items[i] = items[i];
      value_retain(items[i]);
    }
  }

  val->type = VAL_TUPLE;
  val->refcount = 1;
  val->as.tuple.items = tuple_items;
  val->as.tuple.count = count;

  gc_track(val);
  return val;
}

/**
 * @brief Hash function for map keys
 *
 * DESIGN DECISIONS: Strings use pre-computed hash, numbers hash bit
 * representation (handles NaN/INF), booleans 0/1, null constant 0xDEADBEEF,
 * lists/maps content-based, functions/channels pointer-based (reference equality).
 *
 * EDGE CASES: NULL returns 0, NaN hashes by bits, empty collections hash to
 * initial FNV value, circular refs not handled (caller must avoid cycles).
 *
 * @param key Key value to hash (should not be NULL)
 * @return 32-bit hash value
 */
static uint32_t hash_value(KronosValue *key) {
  if (!key)
    return 0;

  switch (key->type) {
  case VAL_STRING:
    return key->as.string.hash;
  case VAL_NUMBER: {
    // Hash the bits of the double
    union {
      double d;
      uint64_t u;
    } converter;
    converter.d = key->as.number;
    return (uint32_t)(converter.u ^ (converter.u >> 32));
  }
  case VAL_BOOL:
    return key->as.boolean ? 1 : 0;
  case VAL_NIL:
    return 0xDEADBEEF; // Arbitrary constant for null
  case VAL_RANGE: {
    // Hash range components
    uint32_t h = 2166136261u;
    union {
      double d;
      uint64_t u;
    } converter;
    converter.d = key->as.range.start;
    h ^= (uint32_t)(converter.u ^ (converter.u >> 32));
    h *= 16777619;
    converter.d = key->as.range.end;
    h ^= (uint32_t)(converter.u ^ (converter.u >> 32));
    h *= 16777619;
    converter.d = key->as.range.step;
    h ^= (uint32_t)(converter.u ^ (converter.u >> 32));
    return h;
  }
  case VAL_LIST: {
    // Hash list by hashing each element (content-based)
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < key->as.list.count; i++) {
      h ^= hash_value(key->as.list.items[i]);
      h *= 16777619;
    }
    return h;
  }
  case VAL_MAP: {
    // Hash map by hashing key-value pairs (content-based)
    // Note: Order-dependent, but maps are unordered anyway
    uint32_t h = 2166136261u;
    MapEntry *entries = (MapEntry *)key->as.map.entries;
    for (size_t i = 0; i < key->as.map.capacity; i++) {
      if (entries[i].key && !entries[i].is_tombstone) {
        h ^= hash_value(entries[i].key);
        h *= 16777619;
        if (entries[i].value) {
          h ^= hash_value(entries[i].value);
          h *= 16777619;
        }
      }
    }
    return h;
  }
  case VAL_TUPLE: {
    // Hash tuple by hashing each element (content-based, order-dependent)
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < key->as.tuple.count; i++) {
      h ^= hash_value(key->as.tuple.items[i]);
      h *= 16777619;
    }
    return h;
  }
  case VAL_FUNCTION:
  case VAL_CHANNEL:
  default:
    // WHY: Functions and channels are reference types - identity matters more
    // than content. Two functions with identical bytecode are still different
    // function objects (they may have different closures, different creation
    // contexts, etc.). Using pointer hashing ensures reference equality
    // semantics.
    //
    // DESIGN DECISION: We multiply by a large prime (2654435761u, from Knuth's
    // multiplicative hash) to improve distribution of pointer addresses, which
    // are often aligned and can cluster.
    return (uint32_t)((uintptr_t)key * 2654435761u);
  }
}

/**
 * @brief Create a new map value
 *
 * Allocates a hash table to hold key-value pairs.
 * Starts with the specified capacity (or 8 if 0) and grows as needed.
 *
 * @param initial_capacity Initial capacity (0 means use default of 8)
 * @return New empty map, or NULL on allocation failure
 */
KronosValue *value_new_map(size_t initial_capacity) {
  size_t capacity = initial_capacity == 0 ? 8 : initial_capacity;

  KronosValue *val = malloc(sizeof(KronosValue));
  if (!val)
    return NULL;

  MapEntry *entries = calloc(capacity, sizeof(MapEntry));

  if (!entries) {
    free(val);
    return NULL;
  }

  val->type = VAL_MAP;
  val->refcount = 1;
  val->as.map.entries = (void *)entries;
  val->as.map.count = 0;
  val->as.map.capacity = capacity;

  gc_track(val);
  return val;
}

/**
 * @brief Increment the reference count of a value
 *
 * DESIGN DECISION: Saturating arithmetic prevents overflow (saturates at
 * UINT32_MAX with warning). Safer than freeing prematurely. Overflow extremely
 * unlikely in practice.
 *
 * EDGE CASES: NULL is no-op, overflow saturates with warning, not thread-safe.
 *
 * @param val Value to retain (safe to pass NULL)
 */
void value_retain(KronosValue *val) {
  if (val) {
    // Use saturating arithmetic: if already at max, leave it there
    // This prevents overflow while avoiding abrupt termination
    if (val->refcount < UINT32_MAX) {
      val->refcount++;
    } else {
      // Refcount already at maximum - value is effectively permanently retained
      // Log warning but continue execution (better than abort())
      fprintf(stderr,
              "Warning: KronosValue refcount at maximum (%u), "
              "saturating to prevent overflow\n",
              UINT32_MAX);
    }
  }
}

/**
 * @brief Push a value onto the release stack
 *
 * Grows the stack if needed. If realloc fails, logs a warning and returns
 * false. The caller should handle the failure appropriately (e.g., by
 * releasing the value directly, which will recurse but avoids memory leak).
 *
 * @param stack Pointer to stack array
 * @param count Pointer to current count
 * @param capacity Pointer to current capacity
 * @param val Value to push
 * @return true on success, false if realloc failed
 */
static bool release_stack_push(KronosValue ***stack, size_t *count,
                               size_t *capacity, KronosValue *val) {
  if (*count == *capacity) {
    size_t new_capacity = (*capacity == 0) ? 8 : (*capacity * 2);
    KronosValue **new_stack =
        realloc(*stack, new_capacity * sizeof(KronosValue *));
    if (!new_stack) {
      fprintf(stderr, "Warning: Failed to grow release stack (memory "
                      "exhaustion). Falling back to recursive release.\n");
      return false;
    }
    *stack = new_stack;
    *capacity = new_capacity;
  }

  (*stack)[(*count)++] = val;
  return true;
}

/**
 * @brief Recursive fallback for value release when stack growth fails
 *
 * Uses recursion instead of the iterative release stack. This is only used
 * when release_stack_push() fails due to memory pressure.
 *
 * @param val Value to release (safe to pass NULL)
 */
static void value_release_recursive_fallback(KronosValue *val) {
  if (!val) {
    return;
  }

  if (val->refcount == 0) {
    fprintf(stderr, "KronosValue refcount underflow\n");
    return;
  }

  val->refcount--;
  if (val->refcount > 0) {
    return;
  }

  gc_untrack(val);

  switch (val->type) {
  case VAL_STRING:
    free(val->as.string.data);
    break;
  case VAL_FUNCTION:
    free(val->as.function.bytecode);
    if (val->as.function.param_names) {
      for (int i = 0; i < val->as.function.arity; i++) {
        free(val->as.function.param_names[i]);
      }
      free(val->as.function.param_names);
    }
    if (val->as.function.param_defaults) {
      for (int i = 0; i < val->as.function.arity; i++) {
        if (val->as.function.param_defaults[i]) {
          value_release_recursive_fallback(val->as.function.param_defaults[i]);
        }
      }
      free(val->as.function.param_defaults);
    }
    break;
  case VAL_LIST:
    if (val->as.list.items) {
      for (size_t i = 0; i < val->as.list.count; i++) {
        if (val->as.list.items[i]) {
          value_release_recursive_fallback(val->as.list.items[i]);
        }
      }
    }
    free(val->as.list.items);
    break;
  case VAL_MAP: {
    MapEntry *entries = (MapEntry *)val->as.map.entries;
    if (entries) {
      for (size_t i = 0; i < val->as.map.capacity; i++) {
        if (entries[i].key && !entries[i].is_tombstone) {
          value_release_recursive_fallback(entries[i].key);
          if (entries[i].value) {
            value_release_recursive_fallback(entries[i].value);
          }
        }
      }
    }
    free(entries);
    break;
  }
  case VAL_TUPLE:
    if (val->as.tuple.items) {
      for (size_t i = 0; i < val->as.tuple.count; i++) {
        if (val->as.tuple.items[i]) {
          value_release_recursive_fallback(val->as.tuple.items[i]);
        }
      }
    }
    free(val->as.tuple.items);
    break;
  case VAL_CHANNEL:
    // Channels are currently managed externally.
    break;
  case VAL_RANGE:
    // Ranges don't own other values, just store numbers.
    break;
  default:
    break;
  }

  free(val);
}

/**
 * @brief Finalize an object without releasing children
 *
 * Used during gc_cleanup to avoid use-after-free issues. This function
 * frees the object's own memory (strings, bytecode, containers) but does
 * NOT recursively release child values, since they will be freed separately
 * by gc_cleanup.
 *
 * @param val Value to finalize (safe to pass NULL)
 */
void value_finalize(KronosValue *val) {
  if (!val)
    return;

  gc_untrack(val);

  // Free any owned memory, but don't release children
  switch (val->type) {
  case VAL_STRING:
    free(val->as.string.data);
    break;
  case VAL_FUNCTION:
    free(val->as.function.bytecode);
    // Free parameter names if present
    if (val->as.function.param_names) {
      for (int i = 0; i < val->as.function.arity; i++) {
        free(val->as.function.param_names[i]);
      }
      free(val->as.function.param_names);
    }
    // Free param_defaults array (don't release values - gc_cleanup handles them)
    if (val->as.function.param_defaults) {
      free(val->as.function.param_defaults);
    }
    break;
  case VAL_LIST:
    // Free the items array, but don't release the child values
    // (they will be freed separately by gc_cleanup)
    free(val->as.list.items);
    break;
  case VAL_MAP: {
    // Free the entries array, but don't release keys/values
    // (they will be freed separately by gc_cleanup)
    free(val->as.map.entries);
    break;
  }
  case VAL_TUPLE:
    // Free the items array, but don't release the child values
    // (they will be freed separately by gc_cleanup)
    free(val->as.tuple.items);
    break;
  case VAL_CHANNEL:
    // Channels are currently managed externally.
    break;
  case VAL_RANGE:
    // Ranges don't own other values, just store numbers
    break;
  default:
    break;
  }

  free(val);
}

/**
 * @brief Decrement the reference count of a value
 *
 * DESIGN DECISION: Iterative (stack-based) approach avoids stack overflow with
 * deeply nested structures. Falls back to recursion if stack growth fails
 * (prevents memory leak, may overflow stack).
 *
 * EDGE CASES: NULL is no-op, underflow logged (double-free bug), circular refs
 * require GC cycle detection (see gc_collect_cycles()).
 *
 * @param val Value to release (safe to pass NULL)
 */
void value_release(KronosValue *val) {
  if (!val)
    return;

  if (val->refcount == 0) {
    fprintf(stderr, "KronosValue refcount underflow\n");
    return;
  }

  KronosValue **stack = NULL;
  size_t stack_count = 0;
  size_t stack_capacity = 0;
  if (!release_stack_push(&stack, &stack_count, &stack_capacity, val)) {
    // Initial stack push failed; use recursive fallback so release still
    // happens under memory pressure.
    value_release_recursive_fallback(val);
    return;
  }

  while (stack_count > 0) {
    KronosValue *current = stack[--stack_count];
    if (!current)
      continue;

    if (current->refcount == 0) {
      fprintf(stderr, "KronosValue refcount underflow\n");
      continue;
    }

    current->refcount--;
    if (current->refcount > 0)
      continue;

    gc_untrack(current);

    // Free any owned memory
    switch (current->type) {
    case VAL_STRING:
      free(current->as.string.data);
      break;
    case VAL_FUNCTION:
      free(current->as.function.bytecode);
      // Free parameter names if present
      if (current->as.function.param_names) {
        for (int i = 0; i < current->as.function.arity; i++) {
          free(current->as.function.param_names[i]);
        }
        free(current->as.function.param_names);
      }
      // Release default values and free array
      if (current->as.function.param_defaults) {
        for (int i = 0; i < current->as.function.arity; i++) {
          if (current->as.function.param_defaults[i]) {
            if (!release_stack_push(&stack, &stack_count, &stack_capacity,
                                    current->as.function.param_defaults[i])) {
              value_release_recursive_fallback(
                  current->as.function.param_defaults[i]);
            }
          }
        }
        free(current->as.function.param_defaults);
      }
      break;
    case VAL_LIST:
      for (size_t i = 0; i < current->as.list.count; i++) {
        KronosValue *child = current->as.list.items[i];
        if (child) {
          if (!release_stack_push(&stack, &stack_count, &stack_capacity,
                                  child)) {
            // Stack push failed - release directly (recursive fallback)
            value_release_recursive_fallback(child);
          }
        }
      }
      free(current->as.list.items);
      break;
    case VAL_MAP: {
      MapEntry *entries = (MapEntry *)current->as.map.entries;
      for (size_t i = 0; i < current->as.map.capacity; i++) {
        if (entries[i].key && !entries[i].is_tombstone) {
          if (!release_stack_push(&stack, &stack_count, &stack_capacity,
                                  entries[i].key)) {
            // Stack push failed - release directly (recursive fallback)
            value_release_recursive_fallback(entries[i].key);
          }
          if (entries[i].value) {
            if (!release_stack_push(&stack, &stack_count, &stack_capacity,
                                    entries[i].value)) {
              // Stack push failed - release directly (recursive fallback)
              value_release_recursive_fallback(entries[i].value);
            }
          }
        }
      }
      free(entries);
      break;
    }
    case VAL_TUPLE:
      for (size_t i = 0; i < current->as.tuple.count; i++) {
        KronosValue *child = current->as.tuple.items[i];
        if (child) {
          if (!release_stack_push(&stack, &stack_count, &stack_capacity,
                                  child)) {
            // Stack push failed - release directly (recursive fallback)
            value_release_recursive_fallback(child);
          }
        }
      }
      free(current->as.tuple.items);
      break;
    case VAL_CHANNEL:
      // Channels are currently managed externally.
      break;
    case VAL_RANGE:
      // Ranges don't own other values, just store numbers
      break;
    default:
      break;
    }

    free(current);
  }

  free(stack);
}

/**
 * @brief Print a value to a file stream (internal recursive version with depth
 * limit)
 *
 * @param out File stream to print to
 * @param val Value to print
 * @param depth Current recursion depth (to prevent stack overflow)
 */
static void value_fprint_recursive(FILE *out, KronosValue *val, int depth) {
  if (!out)
    out = stdout;

  if (!val) {
    fprintf(out, "null");
    return;
  }

  switch (val->type) {
  case VAL_NUMBER: {
    double intpart;
    double frac = modf(val->as.number, &intpart);
    if (frac == 0.0) {
      fprintf(out, "%.0f", val->as.number);
    } else {
      fprintf(out, "%g", val->as.number);
    }
    break;
  }
  case VAL_STRING:
    fprintf(out, "%s", val->as.string.data);
    break;
  case VAL_BOOL:
    fprintf(out, "%s", val->as.boolean ? "true" : "false");
    break;
  case VAL_NIL:
    fprintf(out, "null");
    break;
  case VAL_FUNCTION:
    fprintf(out, "<function>");
    break;
  case VAL_LIST:
    if (depth >= VALUE_PRINT_MAX_DEPTH) {
      fprintf(out, "[<max depth exceeded>]");
      break;
    }
    fprintf(out, "[");
    for (size_t i = 0; i < val->as.list.count; i++) {
      if (i > 0)
        fprintf(out, ", ");
      value_fprint_recursive(out, val->as.list.items[i], depth + 1);
    }
    fprintf(out, "]");
    break;
  case VAL_CHANNEL:
    fprintf(out, "<channel>");
    break;
  case VAL_RANGE: {
    double intpart;
    double frac_start = modf(val->as.range.start, &intpart);
    double frac_end = modf(val->as.range.end, &intpart);
    double frac_step = modf(val->as.range.step, &intpart);

    if (frac_start == 0.0) {
      fprintf(out, "%.0f", val->as.range.start);
    } else {
      fprintf(out, "%g", val->as.range.start);
    }
    fprintf(out, " to ");
    if (frac_end == 0.0) {
      fprintf(out, "%.0f", val->as.range.end);
    } else {
      fprintf(out, "%g", val->as.range.end);
    }
    if (val->as.range.step != 1.0) {
      fprintf(out, " by ");
      if (frac_step == 0.0) {
        fprintf(out, "%.0f", val->as.range.step);
      } else {
        fprintf(out, "%g", val->as.range.step);
      }
    }
    break;
  }
  case VAL_MAP: {
    if (depth >= VALUE_PRINT_MAX_DEPTH) {
      fprintf(out, "{<max depth exceeded>}");
      break;
    }
    MapEntry *entries = (MapEntry *)val->as.map.entries;
    fprintf(out, "{");
    bool first = true;
    for (size_t i = 0; i < val->as.map.capacity; i++) {
      if (entries[i].key && !entries[i].is_tombstone) {
        if (!first)
          fprintf(out, ", ");
        first = false;
        value_fprint_recursive(out, entries[i].key, depth + 1);
        fprintf(out, ": ");
        value_fprint_recursive(out, entries[i].value, depth + 1);
      }
    }
    fprintf(out, "}");
    break;
  }
  case VAL_TUPLE:
    if (depth >= VALUE_PRINT_MAX_DEPTH) {
      fprintf(out, "(<max depth exceeded>)");
      break;
    }
    fprintf(out, "(");
    for (size_t i = 0; i < val->as.tuple.count; i++) {
      if (i > 0)
        fprintf(out, ", ");
      value_fprint_recursive(out, val->as.tuple.items[i], depth + 1);
    }
    fprintf(out, ")");
    break;
  default:
    fprintf(out, "<unknown>");
    break;
  }
}

/**
 * @brief Print a value to a file stream
 *
 * Formats the value in a human-readable way:
 * - Numbers: printed as integers if whole, otherwise as floats
 * - Strings: printed as-is
 * - Booleans: "true" or "false"
 * - Nil: "null"
 * - Lists: [item1, item2, ...]
 * - Maps: {key1: value1, key2: value2, ...}
 *
 * Uses depth limiting to prevent stack overflow from deeply nested structures.
 *
 * @param out File stream to print to (defaults to stdout if NULL)
 * @param val Value to print (prints "null" if NULL)
 */
void value_fprint(FILE *out, KronosValue *val) {
  if (!out)
    out = stdout;
  value_fprint_recursive(out, val, 0);
}

/**
 * @brief Print a value to stdout
 *
 * Convenience wrapper for value_fprint(stdout, val).
 *
 * @param val Value to print
 */
void value_print(KronosValue *val) { value_fprint(stdout, val); }

/**
 * @brief Check if a value is truthy
 *
 * Used for conditionals and boolean operations:
 * - Nil: false
 * - Boolean: its value
 * - Number: false if 0.0, true otherwise
 * - String: false if empty, true otherwise
 * - Other types: true
 *
 * @param val Value to check
 * @return true if truthy, false otherwise
 */
bool value_is_truthy(KronosValue *val) {
  if (!val)
    return false;

  switch (val->type) {
  case VAL_NIL:
    return false;
  case VAL_BOOL:
    return val->as.boolean;
  case VAL_NUMBER:
    return val->as.number != 0.0;
  case VAL_STRING:
    return val->as.string.length > 0;
  default:
    return true;
  }
}

/**
 * @brief Check if two values are equal (internal recursive version with depth
 * limit)
 *
 * @param a First value
 * @param b Second value
 * @param depth Current recursion depth (to prevent stack overflow)
 * @param visited_a Array of visited value pointers from 'a' (for cycle
 * detection)
 * @param visited_b Array of visited value pointers from 'b' (for cycle
 * detection)
 * @param visited_count Current count of visited values
 * @param visited_capacity Capacity of visited arrays
 * @return true if values are equal, false otherwise
 */
static bool value_equals_recursive(KronosValue *a, KronosValue *b, int depth,
                                   KronosValue ***visited_a,
                                   KronosValue ***visited_b,
                                   size_t *visited_count,
                                   size_t *visited_capacity) {
  if (a == b)
    return true;
  if (!a || !b)
    return false;
  if (a->type != b->type)
    return false;

  // Check depth limit
  if (depth >= VALUE_EQUALS_MAX_DEPTH) {
    // At max depth, use pointer equality as fallback
    return a == b;
  }

  // Check for cycles (simple linear search - acceptable for limited depth)
  for (size_t i = 0; i < *visited_count; i++) {
    if ((*visited_a)[i] == a && (*visited_b)[i] == b) {
      // Same pair already being compared - cycle detected, consider equal
      return true;
    }
  }

  // Add to visited set
  if (*visited_count >= *visited_capacity) {
    size_t new_capacity = (*visited_capacity == 0) ? 8 : (*visited_capacity * 2);
    KronosValue **new_visited_a = NULL;
    KronosValue **new_visited_b = NULL;

    if (new_capacity > SIZE_MAX / sizeof(KronosValue *)) {
      new_capacity = 0;
    }
    if (new_capacity > 0) {
      new_visited_a = malloc(new_capacity * sizeof(KronosValue *));
      new_visited_b = malloc(new_capacity * sizeof(KronosValue *));
    }

    if (new_visited_a && new_visited_b) {
      if (*visited_count > 0) {
        memcpy(new_visited_a, *visited_a,
               *visited_count * sizeof(KronosValue *));
        memcpy(new_visited_b, *visited_b,
               *visited_count * sizeof(KronosValue *));
      }
      free(*visited_a);
      free(*visited_b);
      *visited_a = new_visited_a;
      *visited_b = new_visited_b;
      *visited_capacity = new_capacity;
    } else {
      free(new_visited_a);
      free(new_visited_b);
    }
  }
  if (*visited_count < *visited_capacity) {
    (*visited_a)[*visited_count] = a;
    (*visited_b)[*visited_count] = b;
    (*visited_count)++;
  }

  switch (a->type) {
  case VAL_NUMBER:
    return fabs(a->as.number - b->as.number) < VALUE_COMPARE_EPSILON;
  case VAL_STRING:
    return a->as.string.length == b->as.string.length &&
           memcmp(a->as.string.data, b->as.string.data, a->as.string.length) ==
               0;
  case VAL_BOOL:
    return a->as.boolean == b->as.boolean;
  case VAL_NIL:
    return true;
  case VAL_LIST:
    if (a->as.list.count != b->as.list.count)
      return false;
    for (size_t i = 0; i < a->as.list.count; i++) {
      if (!value_equals_recursive(a->as.list.items[i], b->as.list.items[i],
                                  depth + 1, visited_a, visited_b,
                                  visited_count, visited_capacity))
        return false;
    }
    return true;
  case VAL_RANGE:
    return fabs(a->as.range.start - b->as.range.start) <
               VALUE_COMPARE_EPSILON &&
           fabs(a->as.range.end - b->as.range.end) < VALUE_COMPARE_EPSILON &&
           fabs(a->as.range.step - b->as.range.step) < VALUE_COMPARE_EPSILON;
  case VAL_MAP: {
    if (a->as.map.count != b->as.map.count)
      return false;
    MapEntry *a_entries = (MapEntry *)a->as.map.entries;
    MapEntry *b_entries = (MapEntry *)b->as.map.entries;
    // For each key in a, check if it exists in b with same value
    for (size_t i = 0; i < a->as.map.capacity; i++) {
      if (a_entries[i].key && !a_entries[i].is_tombstone) {
        // Find matching key in b
        bool found = false;
        for (size_t j = 0; j < b->as.map.capacity; j++) {
          if (b_entries[j].key && !b_entries[j].is_tombstone &&
              value_equals_recursive(a_entries[i].key, b_entries[j].key,
                                     depth + 1, visited_a, visited_b,
                                     visited_count, visited_capacity)) {
            if (!value_equals_recursive(a_entries[i].value, b_entries[j].value,
                                        depth + 1, visited_a, visited_b,
                                        visited_count, visited_capacity))
              return false;
            found = true;
            break;
          }
        }
        if (!found)
          return false;
      }
    }
    return true;
  }
  case VAL_TUPLE:
    if (a->as.tuple.count != b->as.tuple.count)
      return false;
    for (size_t i = 0; i < a->as.tuple.count; i++) {
      if (!value_equals_recursive(a->as.tuple.items[i], b->as.tuple.items[i],
                                  depth + 1, visited_a, visited_b,
                                  visited_count, visited_capacity))
        return false;
    }
    return true;
  default:
    return a == b; // Pointer equality for complex types
  }
}

/**
 * @brief Check if two values are equal
 *
 * DESIGN DECISIONS: Pointer equality first (fast path), different types never
 * equal, numbers use epsilon, strings byte-by-byte, lists element-by-element,
 * maps order-independent, functions/channels pointer equality, depth limiting
 * prevents stack overflow, cycle detection prevents infinite recursion.
 *
 * EDGE CASES: NULL == NULL true, NaN != NaN, INF handled correctly, empty list
 * != null (different types), circular refs detected as equal, max depth 64
 * falls back to pointer equality.
 *
 * @param a First value
 * @param b Second value
 * @return true if equal, false otherwise
 */
bool value_equals(KronosValue *a, KronosValue *b) {
  KronosValue **visited_a = NULL;
  KronosValue **visited_b = NULL;
  size_t visited_count = 0;
  size_t visited_capacity = 0;

  bool result = value_equals_recursive(a, b, 0, &visited_a, &visited_b,
                                       &visited_count, &visited_capacity);

  free(visited_a);
  free(visited_b);
  return result;
}

/**
 * @brief Find entry index in map for a given key
 *
 * DESIGN DECISION: Linear probing (vs chaining) chosen for simplicity and cache
 * locality. Clustering possible but hash functions provide good distribution.
 *
 * EDGE CASES: Collisions handled via linear probing, tombstones tracked for
 * reuse, full table returns tombstone/hash index, NULL key returns false.
 *
 * @param map Map to search in (must be VAL_MAP type)
 * @param key Key to find (must not be NULL)
 * @param out_index Output parameter for the index (set even if not found)
 * @return true if key found, false otherwise
 */
static bool map_find_entry(KronosValue *map, KronosValue *key,
                           size_t *out_index) {
  if (map->type != VAL_MAP || !key)
    return false;

  uint32_t hash = hash_value(key);
  size_t capacity = map->as.map.capacity;
  size_t index = hash % capacity;

  MapEntry *entries = (MapEntry *)map->as.map.entries;

  // Track first tombstone slot found (can be reused for insertion)
  size_t first_tombstone = SIZE_MAX;

  // Linear probing
  for (size_t i = 0; i < capacity; i++) {
    size_t probe = (index + i) % capacity;
    if (!entries[probe].key) {
      *out_index = probe;
      return false; // Empty slot, key not found
    }
    if (!entries[probe].is_tombstone && value_equals(entries[probe].key, key)) {
      *out_index = probe;
      return true; // Found
    }
    // Track first tombstone for potential reuse
    if (entries[probe].is_tombstone && first_tombstone == SIZE_MAX) {
      first_tombstone = probe;
    }
  }

  // If we found a tombstone, reuse it; otherwise use original hash index
  // (This should rarely happen as map_set grows before calling this)
  if (first_tombstone != SIZE_MAX) {
    *out_index = first_tombstone;
  } else {
    *out_index = index; // Fallback (map should have been grown before this)
  }
  return false;
}

/**
 * @brief Grow map capacity and rehash all entries
 *
 * DESIGN DECISION: Double capacity each time (common strategy), balances memory
 * and rehashing frequency. O(n) rehashing amortized over insertions.
 *
 * EDGE CASES: Allocation failure returns -1, empty map still grows, tombstones
 * not reinserted (cleanup), exponential memory growth acceptable for typical use.
 *
 * @param map Map to grow (must be VAL_MAP type)
 * @return 0 on success, -1 on allocation failure
 */
static int map_grow(KronosValue *map) {
  size_t old_capacity = map->as.map.capacity;
  size_t new_capacity = old_capacity * 2;

  MapEntry *old_entries = (MapEntry *)map->as.map.entries;
  MapEntry *new_entries = calloc(new_capacity, sizeof(MapEntry));

  if (!new_entries)
    return -1;

  // Rehash all entries
  for (size_t i = 0; i < old_capacity; i++) {
    if (old_entries[i].key && !old_entries[i].is_tombstone) {
      uint32_t hash = hash_value(old_entries[i].key);
      size_t index = hash % new_capacity;

      // Find empty slot
      for (size_t j = 0; j < new_capacity; j++) {
        size_t probe = (index + j) % new_capacity;
        if (!new_entries[probe].key) {
          new_entries[probe].key = old_entries[i].key;
          new_entries[probe].value = old_entries[i].value;
          new_entries[probe].is_tombstone = false;
          break;
        }
      }
    }
  }

  free(old_entries);
  map->as.map.entries = (void *)new_entries;
  map->as.map.capacity = new_capacity;
  return 0;
}

/**
 * @brief Get value from map by key
 *
 * DESIGN DECISION: Returns NULL for missing keys (vs sentinel), consistent with
 * common map APIs. Allows distinguishing "not found" from "null value".
 *
 * EDGE CASES: NULL key returns NULL, wrong type returns NULL, caller must
 * retain if keeping reference beyond map lifetime.
 *
 * @param map Map to get from (must be VAL_MAP type)
 * @param key Key to look up (must not be NULL)
 * @return Value if found, NULL otherwise (caller must retain if keeping)
 */
KronosValue *map_get(KronosValue *map, KronosValue *key) {
  if (map->type != VAL_MAP || !key)
    return NULL;

  size_t index;
  if (!map_find_entry(map, key, &index))
    return NULL;

  MapEntry *entries = (MapEntry *)map->as.map.entries;

  return entries[index].value;
}

/**
 * @brief Set value in map by key
 *
 * DESIGN DECISION: Grow when load factor > 0.75 (count * 4 >= capacity * 3,
 * integer arithmetic to avoid floating-point). Balances memory and performance.
 *
 * EDGE CASES: Existing key updates value, new key inserts, tombstone reuse,
 * allocation failure returns -1, NULL key returns -1, both key/value retained.
 *
 * @param map Map to set in (must be VAL_MAP type)
 * @param key Key to set (must not be NULL)
 * @param value Value to set (can be NULL, though not currently used)
 * @return 0 on success, -1 on failure (allocation error or invalid input)
 */
int map_set(KronosValue *map, KronosValue *key, KronosValue *value) {
  if (map->type != VAL_MAP || !key)
    return -1;

  // WHY: Grow if load factor > 0.75 to maintain good performance
  // Using integer arithmetic: count * 4 >= capacity * 3 is equivalent to
  // count / capacity >= 0.75, but avoids floating-point
  if (map->as.map.count * 4 >= map->as.map.capacity * 3) {
    if (map_grow(map) != 0)
      return -1;
  }

  size_t index;
  bool found = map_find_entry(map, key, &index);

  MapEntry *entries = (MapEntry *)map->as.map.entries;

  if (found) {
    // Update existing entry
    value_release(entries[index].value);
    entries[index].value = value;
    value_retain(value);
    // Key is already retained from initial insertion, no need to retain again
  } else {
    // Insert new entry
    if (!entries[index].key) {
      map->as.map.count++;
    }
    entries[index].key = key;
    entries[index].value = value;
    entries[index].is_tombstone = false;
    value_retain(key);
    value_retain(value);
  }

  return 0;
}

/**
 * @brief Delete key from map
 *
 * DESIGN DECISION: Tombstone marking (lazy deletion) avoids breaking linear
 * probing chains. Immediate removal would break chains and lose access to
 * entries inserted after deleted one. Tombstones cleaned up during map_grow().
 *
 * EDGE CASES: Key not found returns false, idempotent, NULL key returns false,
 * releases key/value refs, tombstones cleaned up on next grow.
 *
 * @param map Map to delete from (must be VAL_MAP type)
 * @param key Key to delete (must not be NULL)
 * @return true if key was found and deleted, false otherwise
 */
bool map_delete(KronosValue *map, KronosValue *key) {
  if (map->type != VAL_MAP || !key)
    return false;

  size_t index;
  if (!map_find_entry(map, key, &index))
    return false;

  MapEntry *entries = (MapEntry *)map->as.map.entries;

  if (entries[index].is_tombstone)
    return false;

  // Release references to key and value
  value_release(entries[index].key);
  value_release(entries[index].value);

  // Mark as tombstone (key/value set to NULL for safety and to allow reuse)
  // The NULL check in map_set distinguishes new slots from reused tombstones
  entries[index].key = NULL;
  entries[index].value = NULL;
  entries[index].is_tombstone = true;
  map->as.map.count--;

  return true;
}

/**
 * @brief Intern a string (deduplicate identical strings)
 *
 * DESIGN DECISION: Fixed-size hash table (1024) with linear probing, shared
 * across VMs. Falls back to non-interned string if table full.
 *
 * EDGE CASES: Collisions via linear probing, table full falls back, thread-safe
 * via intern_mutex, NULL treated as empty, caller must retain if keeping beyond
 * runtime_cleanup().
 *
 * @param str String to intern
 * @param len Length of the string
 * @return Interned string value (may be existing or newly created)
 */
KronosValue *string_intern(const char *str, size_t len) {
  uint32_t hash = hash_string(str, len);
  size_t index = hash % INTERN_TABLE_SIZE;

  pthread_mutex_lock(&intern_mutex);

  // Linear probing
  for (size_t i = 0; i < INTERN_TABLE_SIZE; i++) {
    size_t probe = (index + i) % INTERN_TABLE_SIZE;
    KronosValue *entry = intern_table[probe];

    if (entry == NULL) {
      // Not found, create new interned string
      KronosValue *val = value_new_string(str, len);
      if (val) {
        intern_table[probe] = val;
        // Keep one strong reference owned by intern table.
        // The initial ref from value_new_string() is returned to caller.
        value_retain(val);
      }
      pthread_mutex_unlock(&intern_mutex);
      return val;
    }

    if (entry->type == VAL_STRING && entry->as.string.hash == hash &&
        entry->as.string.length == len &&
        memcmp(entry->as.string.data, str, len) == 0) {
      // Found existing interned string
      // Retain before returning so caller gets refcount 1 (consistent with new
      // strings)
      value_retain(entry);
      pthread_mutex_unlock(&intern_mutex);
      return entry;
    }
  }

  // Table full, fallback to non-interned string
  pthread_mutex_unlock(&intern_mutex);
  fprintf(stderr,
          "Warning: String intern table full (size %d), falling back to "
          "non-interned string\n",
          INTERN_TABLE_SIZE);
  return value_new_string(str, len);
}

#define TYPE_MATCH_MAX_DEPTH 64

static char *type_trim_dup(const char *s, size_t len) {
  if (!s) {
    return NULL;
  }

  size_t start = 0;
  while (start < len && isspace((unsigned char)s[start])) {
    start++;
  }

  size_t end = len;
  while (end > start && isspace((unsigned char)s[end - 1])) {
    end--;
  }

  size_t out_len = end - start;
  char *out = malloc(out_len + 1);
  if (!out) {
    return NULL;
  }
  memcpy(out, s + start, out_len);
  out[out_len] = '\0';
  return out;
}

static bool parse_generic_inner(const char *type_name, const char *base,
                                char **inner_out) {
  if (!type_name || !base || !inner_out) {
    return false;
  }
  *inner_out = NULL;

  size_t base_len = strlen(base);
  size_t type_len = strlen(type_name);
  if (type_len <= base_len + 2) {
    return false;
  }
  if (strncmp(type_name, base, base_len) != 0 || type_name[base_len] != '<') {
    return false;
  }

  int depth = 1;
  size_t close_idx = SIZE_MAX;
  for (size_t i = base_len + 1; i < type_len; i++) {
    char c = type_name[i];
    if (c == '<') {
      depth++;
    } else if (c == '>') {
      depth--;
      if (depth == 0) {
        close_idx = i;
        break;
      }
      if (depth < 0) {
        return false;
      }
    }
  }

  if (close_idx == SIZE_MAX || close_idx != type_len - 1) {
    return false;
  }

  *inner_out =
      type_trim_dup(type_name + base_len + 1, close_idx - (base_len + 1));
  return *inner_out != NULL;
}

static bool find_top_level_char(const char *s, char target, size_t *idx_out) {
  if (!s) {
    return false;
  }

  int angle_depth = 0;
  int brace_depth = 0;
  size_t len = strlen(s);
  for (size_t i = 0; i < len; i++) {
    char c = s[i];
    if (c == '<') {
      angle_depth++;
      continue;
    }
    if (c == '>') {
      angle_depth--;
      continue;
    }
    if (c == '{') {
      brace_depth++;
      continue;
    }
    if (c == '}') {
      brace_depth--;
      continue;
    }
    if (c == target && angle_depth == 0 && brace_depth == 0) {
      if (idx_out) {
        *idx_out = i;
      }
      return true;
    }
  }
  return false;
}

static bool value_is_base_type(KronosValue *val, const char *type_name) {
  if (!val || !type_name) {
    return false;
  }

  if (strcmp(type_name, "number") == 0) {
    return val->type == VAL_NUMBER;
  }
  if (strcmp(type_name, "string") == 0) {
    return val->type == VAL_STRING;
  }
  if (strcmp(type_name, "boolean") == 0 || strcmp(type_name, "bool") == 0) {
    return val->type == VAL_BOOL;
  }
  if (strcmp(type_name, "null") == 0) {
    return val->type == VAL_NIL;
  }
  if (strcmp(type_name, "list") == 0) {
    return val->type == VAL_LIST;
  }
  if (strcmp(type_name, "map") == 0) {
    return val->type == VAL_MAP;
  }
  if (strcmp(type_name, "range") == 0) {
    return val->type == VAL_RANGE;
  }
  if (strcmp(type_name, "tuple") == 0) {
    return val->type == VAL_TUPLE;
  }
  if (strcmp(type_name, "function") == 0) {
    return val->type == VAL_FUNCTION;
  }
  if (strcmp(type_name, "channel") == 0) {
    return val->type == VAL_CHANNEL;
  }
  return false;
}

static bool value_is_type_expr(KronosValue *val, const char *type_name,
                               int depth) {
  if (!val || !type_name || depth > TYPE_MATCH_MAX_DEPTH) {
    return false;
  }

  char *trimmed = type_trim_dup(type_name, strlen(type_name));
  if (!trimmed) {
    return false;
  }
  if (trimmed[0] == '\0') {
    free(trimmed);
    return false;
  }

  // Top-level union support: "number or string".
  int angle_depth = 0;
  int brace_depth = 0;
  size_t segment_start = 0;
  bool saw_union = false;
  size_t len = strlen(trimmed);
  for (size_t i = 0; i < len; i++) {
    char c = trimmed[i];
    if (c == '<') {
      angle_depth++;
      continue;
    }
    if (c == '>') {
      angle_depth--;
      continue;
    }
    if (c == '{') {
      brace_depth++;
      continue;
    }
    if (c == '}') {
      brace_depth--;
      continue;
    }

    if (angle_depth == 0 && brace_depth == 0 && c == 'o' && i + 1 < len &&
        trimmed[i + 1] == 'r' &&
        (i == 0 || isspace((unsigned char)trimmed[i - 1])) &&
        (i + 2 >= len || isspace((unsigned char)trimmed[i + 2]))) {
      saw_union = true;
      char *segment = type_trim_dup(trimmed + segment_start, i - segment_start);
      if (!segment) {
        free(trimmed);
        return false;
      }
      bool matches = value_is_type_expr(val, segment, depth + 1);
      free(segment);
      if (matches) {
        free(trimmed);
        return true;
      }

      // Skip to next non-whitespace after "or"
      i += 2;
      while (i < len && isspace((unsigned char)trimmed[i])) {
        i++;
      }
      if (i > 0) {
        segment_start = i;
        i--;
      } else {
        segment_start = i;
      }
    }
  }

  if (saw_union) {
    char *segment =
        type_trim_dup(trimmed + segment_start, len - segment_start);
    if (!segment) {
      free(trimmed);
      return false;
    }
    bool matches = value_is_type_expr(val, segment, depth + 1);
    free(segment);
    free(trimmed);
    return matches;
  }

  // Generic list<T>.
  char *inner = NULL;
  if (parse_generic_inner(trimmed, "list", &inner)) {
    bool ok = true;
    if (val->type != VAL_LIST) {
      ok = false;
    } else if (find_top_level_char(inner, ',', NULL)) {
      ok = false;
    } else {
      for (size_t i = 0; i < val->as.list.count; i++) {
        if (!value_is_type_expr(val->as.list.items[i], inner, depth + 1)) {
          ok = false;
          break;
        }
      }
    }
    free(inner);
    free(trimmed);
    return ok;
  }

  // Generic map<K, V>.
  if (parse_generic_inner(trimmed, "map", &inner)) {
    bool ok = true;
    if (val->type != VAL_MAP) {
      ok = false;
    } else {
      size_t comma_idx = 0;
      if (!find_top_level_char(inner, ',', &comma_idx)) {
        ok = false;
      } else {
        char *key_type = type_trim_dup(inner, comma_idx);
        char *value_type = type_trim_dup(inner + comma_idx + 1,
                                         strlen(inner) - comma_idx - 1);
        if (!key_type || !value_type || key_type[0] == '\0' ||
            value_type[0] == '\0') {
          ok = false;
        } else if (find_top_level_char(value_type, ',', NULL)) {
          // map<K,V> only supports two top-level arguments.
          ok = false;
        } else {
          MapEntry *entries = (MapEntry *)val->as.map.entries;
          for (size_t i = 0; i < val->as.map.capacity; i++) {
            if (!entries[i].key || entries[i].is_tombstone) {
              continue;
            }
            if (!value_is_type_expr(entries[i].key, key_type, depth + 1) ||
                !value_is_type_expr(entries[i].value, value_type, depth + 1)) {
              ok = false;
              break;
            }
          }
        }
        free(key_type);
        free(value_type);
      }
    }
    free(inner);
    free(trimmed);
    return ok;
  }

  // Structural map shape: map{x:number,y:number}.
  size_t trimmed_len = strlen(trimmed);
  if (trimmed_len >= 5 && strncmp(trimmed, "map{", 4) == 0 &&
      trimmed[trimmed_len - 1] == '}') {
    if (val->type != VAL_MAP) {
      free(trimmed);
      return false;
    }

    char *fields = type_trim_dup(trimmed + 4, trimmed_len - 5);
    if (!fields) {
      free(trimmed);
      return false;
    }
    if (fields[0] == '\0') {
      free(fields);
      free(trimmed);
      return true;
    }

    bool ok = true;
    size_t fields_len = strlen(fields);
    size_t field_start = 0;
    int inner_angle_depth = 0;
    int inner_brace_depth = 0;

    for (size_t i = 0; i <= fields_len; i++) {
      char c = (i < fields_len) ? fields[i] : ',';
      if (i < fields_len) {
        if (c == '<') {
          inner_angle_depth++;
          continue;
        }
        if (c == '>') {
          inner_angle_depth--;
          continue;
        }
        if (c == '{') {
          inner_brace_depth++;
          continue;
        }
        if (c == '}') {
          inner_brace_depth--;
          continue;
        }
      }

      if (c == ',' && inner_angle_depth == 0 && inner_brace_depth == 0) {
        char *field =
            type_trim_dup(fields + field_start, i - field_start);
        if (!field || field[0] == '\0') {
          free(field);
          ok = false;
          break;
        }

        size_t colon_idx = 0;
        if (!find_top_level_char(field, ':', &colon_idx)) {
          free(field);
          ok = false;
          break;
        }

        char *field_name = type_trim_dup(field, colon_idx);
        char *field_type = type_trim_dup(field + colon_idx + 1,
                                         strlen(field) - colon_idx - 1);
        free(field);

        if (!field_name || !field_type || field_name[0] == '\0' ||
            field_type[0] == '\0') {
          free(field_name);
          free(field_type);
          ok = false;
          break;
        }

        KronosValue *key = value_new_string(field_name, strlen(field_name));
        if (!key) {
          free(field_name);
          free(field_type);
          ok = false;
          break;
        }
        KronosValue *field_value = map_get(val, key);
        value_release(key);

        if (!field_value ||
            !value_is_type_expr(field_value, field_type, depth + 1)) {
          free(field_name);
          free(field_type);
          ok = false;
          break;
        }

        free(field_name);
        free(field_type);
        field_start = i + 1;
      }
    }

    free(fields);
    free(trimmed);
    return ok;
  }

  bool result = value_is_base_type(val, trimmed);
  free(trimmed);
  return result;
}

/**
 * @brief Check if a value matches a type expression.
 *
 * Supports primitive names (`number`, `string`, ...), unions
 * (`number or string`), generics (`list<number>`, `map<string, number>`), and
 * structural map shapes (`map{x:number,y:number}`).
 */
bool value_is_type(KronosValue *val, const char *type_name) {
  return value_is_type_expr(val, type_name, 0);
}
