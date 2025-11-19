#include "runtime.h"
#include "gc.h"
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VALUE_COMPARE_EPSILON (1e-9)

// String interning table
#define INTERN_TABLE_SIZE 1024
static KronosValue *intern_table[INTERN_TABLE_SIZE] = {0};

// Hash function for strings
static uint32_t hash_string(const char *str, size_t len) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < len; i++) {
    hash ^= (uint8_t)str[i];
    hash *= 16777619;
  }
  return hash;
}

// Initialize runtime
void runtime_init(void) {
  memset(intern_table, 0, sizeof(intern_table));
  gc_init();
}

// Cleanup runtime.
// NOTE: This must only run after every external reference to interned strings
// has been released. We drop the intern table's own references via
// value_release(), which decrements refcounts and frees only when they reach 0.
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

// Create a new number value
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

// Create a new string value
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

// Create a new boolean value
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

// Create a new nil value
KronosValue *value_new_nil(void) {
  KronosValue *val = malloc(sizeof(KronosValue));
  if (!val)
    return NULL;

  val->type = VAL_NIL;
  val->refcount = 1;

  gc_track(val);
  return val;
}

// Create a new function value
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

// Create a new list value
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

// Create a new channel value
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

// Retain a value (increment refcount)
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

// Release a value (decrement refcount, free if 0)
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
    case VAL_CHANNEL:
      // Channels are currently managed externally.
      break;
    default:
      break;
    }

    free(current);
  }

  free(stack);
}

// Print a value to a stream
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
  default:
    fprintf(out, "<unknown>");
    break;
  }
}

void value_print(KronosValue *val) { value_fprint(stdout, val); }

// Check if a value is truthy
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

// Check if two values are equal
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
  default:
    return a == b; // Pointer equality for complex types
  }
}

// String interning
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

// Check if a value matches a type name
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
  }

  return false;
}
