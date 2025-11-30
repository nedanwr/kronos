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

#include "runtime.h"
#include "gc.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Epsilon value for floating-point comparisons (handles rounding errors) */
#define VALUE_COMPARE_EPSILON (1e-9)

/** Size of the string interning hash table */
#define INTERN_TABLE_SIZE 1024

/** Hash table for string interning (reduces memory for duplicate strings) */
static KronosValue *intern_table[INTERN_TABLE_SIZE] = {0};

/**
 * @brief Hash function for strings (FNV-1a algorithm)
 *
 * Used for string interning to quickly find existing strings.
 *
 * @param str String to hash
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
 * Must be called before creating any values. Initializes the string
 * interning table and garbage collector.
 */
void runtime_init(void) {
  memset(intern_table, 0, sizeof(intern_table));
  gc_init();
}

/**
 * @brief Cleanup the runtime system
 *
 * Releases all interned strings and shuts down the garbage collector.
 * IMPORTANT: This must only be called after all external references to
 * interned strings have been released, otherwise values may be freed prematurely.
 */
void runtime_cleanup(void) {
  // Free interned strings
  for (size_t i = 0; i < INTERN_TABLE_SIZE; i++) {
    if (intern_table[i] != NULL) {
      value_release(intern_table[i]); // Release intern table's reference
      intern_table[i] = NULL;
    }
  }
  gc_cleanup();
}

/**
 * @brief Create a new number value
 *
 * Allocates a KronosValue representing a floating-point number.
 * The value is tracked by the garbage collector and uses reference counting.
 *
 * @param num The numeric value
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
 * is copied into the value.
 *
 * @param bytecode Function bytecode (will be copied)
 * @param length Length of bytecode in bytes
 * @param arity Number of parameters the function expects
 * @return New function value, or NULL on allocation failure
 */
KronosValue *value_new_function(uint8_t *bytecode, size_t length, int arity) {
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

  val->type = VAL_FUNCTION;
  val->refcount = 1;
  val->as.function.bytecode = buffer;
  val->as.function.length = length;
  val->as.function.arity = arity;

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
 * @param start Starting value (inclusive)
 * @param end Ending value (inclusive)
 * @param step Step size (defaults to 1.0 if 0.0)
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
  // Default step to 1.0 if 0.0 is provided
  val->as.range.step = (step == 0.0) ? 1.0 : step;

  gc_track(val);
  return val;
}

/**
 * @brief Hash function for map keys
 *
 * Computes a hash value for any KronosValue to use as a map key.
 * Supports: strings, numbers, booleans, null.
 *
 * @param key Key value to hash
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
  default:
    // For other types, use pointer hash
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

  struct {
    KronosValue *key;
    KronosValue *value;
    bool is_tombstone;
  } *entries = calloc(capacity, sizeof(*entries));
  
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
 * Call this when storing a value in a new location. Must be paired
 * with value_release() when the reference is no longer needed.
 *
 * @param val Value to retain (safe to pass NULL)
 */
void value_retain(KronosValue *val) {
  if (val) {
    if (val->refcount == UINT32_MAX) {
      fprintf(stderr, "KronosValue refcount overflow\n");
      abort();
    }
    val->refcount++;
  }
}

static void release_stack_push(KronosValue ***stack, size_t *count,
                               size_t *capacity, KronosValue *val) {
  if (*count == *capacity) {
    size_t new_capacity = (*capacity == 0) ? 8 : (*capacity * 2);
    KronosValue **new_stack =
        realloc(*stack, new_capacity * sizeof(KronosValue *));
    if (!new_stack) {
      fprintf(stderr, "Failed to grow release stack\n");
      abort();
    }
    *stack = new_stack;
    *capacity = new_capacity;
  }

  (*stack)[(*count)++] = val;
}

/**
 * @brief Decrement the reference count of a value
 *
 * Call this when removing a reference to a value. When the refcount
 * reaches zero, the value and its owned memory are automatically freed.
 * Uses iterative release to handle nested structures (lists containing lists).
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
  release_stack_push(&stack, &stack_count, &stack_capacity, val);

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
      break;
    case VAL_LIST:
      for (size_t i = 0; i < current->as.list.count; i++) {
        KronosValue *child = current->as.list.items[i];
        if (child)
          release_stack_push(&stack, &stack_count, &stack_capacity, child);
      }
      free(current->as.list.items);
      break;
    case VAL_MAP: {
      struct {
        KronosValue *key;
        KronosValue *value;
        bool is_tombstone;
      } *entries = (void *)current->as.map.entries;
      for (size_t i = 0; i < current->as.map.capacity; i++) {
        if (entries[i].key && !entries[i].is_tombstone) {
          release_stack_push(&stack, &stack_count, &stack_capacity, entries[i].key);
          if (entries[i].value)
            release_stack_push(&stack, &stack_count, &stack_capacity, entries[i].value);
        }
      }
      free(entries);
      break;
    }
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
 * @brief Print a value to a file stream
 *
 * Formats the value in a human-readable way:
 * - Numbers: printed as integers if whole, otherwise as floats
 * - Strings: printed as-is
 * - Booleans: "true" or "false"
 * - Nil: "null"
 * - Lists: [item1, item2, ...]
 *
 * @param out File stream to print to (defaults to stdout if NULL)
 * @param val Value to print (prints "null" if NULL)
 */
void value_fprint(FILE *out, KronosValue *val) {
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
    fprintf(out, "[");
    for (size_t i = 0; i < val->as.list.count; i++) {
      if (i > 0)
        fprintf(out, ", ");
      value_fprint(out, val->as.list.items[i]);
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
    struct {
      KronosValue *key;
      KronosValue *value;
      bool is_tombstone;
    } *entries = (void *)val->as.map.entries;
    fprintf(out, "{");
    bool first = true;
    for (size_t i = 0; i < val->as.map.capacity; i++) {
      if (entries[i].key && !entries[i].is_tombstone) {
        if (!first)
          fprintf(out, ", ");
        first = false;
        value_fprint(out, entries[i].key);
        fprintf(out, ": ");
        value_fprint(out, entries[i].value);
      }
    }
    fprintf(out, "}");
    break;
  }
  default:
    fprintf(out, "<unknown>");
    break;
  }
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
 * @brief Check if two values are equal
 *
 * Performs deep equality checking:
 * - Same pointer: always equal
 * - Different types: never equal
 * - Numbers: compared with epsilon tolerance for floating-point
 * - Strings: byte-by-byte comparison
 * - Lists: recursive element-by-element comparison
 * - Other types: pointer equality
 *
 * @param a First value
 * @param b Second value
 * @return true if equal, false otherwise
 */
bool value_equals(KronosValue *a, KronosValue *b) {
  if (a == b)
    return true;
  if (!a || !b)
    return false;
  if (a->type != b->type)
    return false;

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
      if (!value_equals(a->as.list.items[i], b->as.list.items[i]))
        return false;
    }
    return true;
  case VAL_RANGE:
    return fabs(a->as.range.start - b->as.range.start) < VALUE_COMPARE_EPSILON &&
           fabs(a->as.range.end - b->as.range.end) < VALUE_COMPARE_EPSILON &&
           fabs(a->as.range.step - b->as.range.step) < VALUE_COMPARE_EPSILON;
  case VAL_MAP: {
    if (a->as.map.count != b->as.map.count)
      return false;
    struct {
      KronosValue *key;
      KronosValue *value;
      bool is_tombstone;
    } *a_entries = (void *)a->as.map.entries;
    struct {
      KronosValue *key;
      KronosValue *value;
      bool is_tombstone;
    } *b_entries = (void *)b->as.map.entries;
    // For each key in a, check if it exists in b with same value
    for (size_t i = 0; i < a->as.map.capacity; i++) {
      if (a_entries[i].key && !a_entries[i].is_tombstone) {
        // Find matching key in b
        bool found = false;
        for (size_t j = 0; j < b->as.map.capacity; j++) {
          if (b_entries[j].key && !b_entries[j].is_tombstone &&
              value_equals(a_entries[i].key, b_entries[j].key)) {
            if (!value_equals(a_entries[i].value, b_entries[j].value))
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
  default:
    return a == b; // Pointer equality for complex types
  }
}

/**
 * @brief Find entry index in map for a given key
 *
 * Uses linear probing to find the entry for a key.
 *
 * @param map Map to search in
 * @param key Key to find
 * @param out_index Output parameter for the index (set even if not found)
 * @return true if key found, false otherwise
 */
static bool map_find_entry(KronosValue *map, KronosValue *key, size_t *out_index) {
  if (map->type != VAL_MAP || !key)
    return false;

  uint32_t hash = hash_value(key);
  size_t capacity = map->as.map.capacity;
  size_t index = hash % capacity;

  struct {
    KronosValue *key;
    KronosValue *value;
    bool is_tombstone;
  } *entries = (void *)map->as.map.entries;

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
  }

  *out_index = index; // Fallback
  return false;
}

/**
 * @brief Grow map capacity and rehash all entries
 *
 * @param map Map to grow
 * @return 0 on success, -1 on failure
 */
static int map_grow(KronosValue *map) {
  size_t old_capacity = map->as.map.capacity;
  size_t new_capacity = old_capacity * 2;

  struct {
    KronosValue *key;
    KronosValue *value;
    bool is_tombstone;
  } *old_entries = (void *)map->as.map.entries;
  struct {
    KronosValue *key;
    KronosValue *value;
    bool is_tombstone;
  } *new_entries = calloc(new_capacity, sizeof(*new_entries));

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
 * @param map Map to get from
 * @param key Key to look up
 * @return Value if found, NULL otherwise (caller must retain if keeping)
 */
KronosValue *map_get(KronosValue *map, KronosValue *key) {
  if (map->type != VAL_MAP || !key)
    return NULL;

  size_t index;
  if (!map_find_entry(map, key, &index))
    return NULL;

  struct {
    KronosValue *key;
    KronosValue *value;
    bool is_tombstone;
  } *entries = (void *)map->as.map.entries;

  return entries[index].value;
}

/**
 * @brief Set value in map by key
 *
 * @param map Map to set in
 * @param key Key to set
 * @param value Value to set
 * @return 0 on success, -1 on failure
 */
int map_set(KronosValue *map, KronosValue *key, KronosValue *value) {
  if (map->type != VAL_MAP || !key)
    return -1;

  // Grow if load factor > 0.75
  if (map->as.map.count * 4 >= map->as.map.capacity * 3) {
    if (map_grow(map) != 0)
      return -1;
  }

  size_t index;
  bool found = map_find_entry(map, key, &index);

  struct {
    KronosValue *key;
    KronosValue *value;
    bool is_tombstone;
  } *entries = (void *)map->as.map.entries;

  if (found) {
    // Update existing entry
    value_release(entries[index].value);
    entries[index].value = value;
    value_retain(value);
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
 * @param map Map to delete from
 * @param key Key to delete
 * @return true if key was found and deleted, false otherwise
 */
bool map_delete(KronosValue *map, KronosValue *key) {
  if (map->type != VAL_MAP || !key)
    return false;

  size_t index;
  if (!map_find_entry(map, key, &index))
    return false;

  struct {
    KronosValue *key;
    KronosValue *value;
    bool is_tombstone;
  } *entries = (void *)map->as.map.entries;

  if (entries[index].is_tombstone)
    return false;

  value_release(entries[index].key);
  value_release(entries[index].value);
  entries[index].key = NULL;
  entries[index].value = NULL;
  entries[index].is_tombstone = true;
  map->as.map.count--;

  return true;
}

/**
 * @brief Intern a string (deduplicate identical strings)
 *
 * Returns an existing string value if one with the same content exists,
 * otherwise creates a new one. This reduces memory usage when the same
 * string appears multiple times (e.g., variable names, keywords).
 *
 * Uses linear probing for collision resolution. Falls back to creating
 * a non-interned string if the table is full.
 *
 * @param str String to intern
 * @param len Length of the string
 * @return Interned string value (may be existing or newly created)
 */
KronosValue *string_intern(const char *str, size_t len) {
  uint32_t hash = hash_string(str, len);
  size_t index = hash % INTERN_TABLE_SIZE;

  // Linear probing
  for (size_t i = 0; i < INTERN_TABLE_SIZE; i++) {
    size_t probe = (index + i) % INTERN_TABLE_SIZE;
    KronosValue *entry = intern_table[probe];

    if (entry == NULL) {
      // Not found, create new interned string
      KronosValue *val = value_new_string(str, len);
      if (val) {
        intern_table[probe] = val;
        value_retain(val); // Extra ref for intern table
      }
      return val;
    }

    if (entry->type == VAL_STRING && entry->as.string.hash == hash &&
        entry->as.string.length == len &&
        memcmp(entry->as.string.data, str, len) == 0) {
      // Found existing interned string
      return entry;
    }
  }

  // Table full, fallback to non-interned string
  return value_new_string(str, len);
}

/**
 * @brief Check if a value matches a type name
 *
 * Used for type annotations and type checking. Supports:
 * - "number" for VAL_NUMBER
 * - "string" for VAL_STRING
 * - "boolean" for VAL_BOOL
 * - "null" for VAL_NIL
 *
 * @param val Value to check
 * @param type_name Type name string (e.g., "number", "string")
 * @return true if value matches the type, false otherwise
 */
bool value_is_type(KronosValue *val, const char *type_name) {
  if (!val || !type_name)
    return false;

  if (strcmp(type_name, "number") == 0) {
    return val->type == VAL_NUMBER;
  } else if (strcmp(type_name, "string") == 0) {
    return val->type == VAL_STRING;
  } else if (strcmp(type_name, "boolean") == 0) {
    return val->type == VAL_BOOL;
  } else if (strcmp(type_name, "null") == 0) {
    return val->type == VAL_NIL;
  } else if (strcmp(type_name, "range") == 0) {
    return val->type == VAL_RANGE;
  }

  return false;
}
