#ifndef KRONOS_RUNTIME_H
#define KRONOS_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct Channel Channel;

// Value types in Kronos
typedef enum {
  VAL_NUMBER,
  VAL_STRING,
  VAL_BOOL,
  VAL_NIL,
  VAL_FUNCTION,
  VAL_LIST,
  VAL_CHANNEL,
  VAL_RANGE,
  VAL_MAP,
} ValueType;

// Reference-counted value
typedef struct KronosValue {
  ValueType type;
  uint32_t refcount;
  union {
    double number;
    struct {
      char *data;
      size_t length;
      uint32_t hash;
    } string;
    bool boolean;
    struct {
      uint8_t *bytecode;
      size_t length;
      int arity;
      char **param_names; // Parameter names for argument binding (may be NULL)
    } function;
    struct {
      struct KronosValue **items;
      size_t count;
      size_t capacity;
    } list;
    Channel *channel;
    struct {
      double start;
      double end;
      double step;
    } range;
    struct {
      // Hash table for key-value pairs
      struct {
        struct KronosValue *key;
        struct KronosValue *value;
        bool is_tombstone; // For deletion marking
      } *entries;
      size_t count;      // Number of active entries
      size_t capacity;   // Total capacity of hash table
    } map;
  } as;
} KronosValue;

// Factory/ownership rules:
// - Each factory returns a new KronosValue with refcount 1 owned by caller.
// - Callers must eventually release the value via value_release().
// - value_new_string copies the provided bytes (treats NULL as "") and owns the
//   resulting buffer; callers may free their original buffer immediately.
// - value_new_function copies the bytecode buffer (returns NULL when bytecode
// is
//   NULL or length == 0) and retains the copy internally.
// - value_new_list accepts initial_capacity == 0 and picks a default size.
// - value_new_channel adopts ownership of the Channel* (callers must not free
//   it after passing it in) and returns NULL on invalid inputs.
// Value creation functions
KronosValue *value_new_number(double num);
KronosValue *value_new_string(const char *str, size_t len);
KronosValue *value_new_bool(bool val);
KronosValue *value_new_nil(void);
KronosValue *value_new_function(uint8_t *bytecode, size_t length, int arity,
                                char **param_names);
KronosValue *value_new_list(size_t initial_capacity);
KronosValue *value_new_channel(Channel *channel);
KronosValue *value_new_range(double start, double end, double step);
KronosValue *value_new_map(size_t initial_capacity);

// Reference counting
// Both helpers treat NULL inputs as no-ops for convenience.
void value_retain(KronosValue *val);  // increments refcount if val != NULL
void value_release(KronosValue *val); // decrements refcount, frees at 0
void value_finalize(KronosValue *val); // finalizes object without releasing children (for gc_cleanup)

// Value operations
void value_fprint(FILE *out, KronosValue *val);
void value_print(KronosValue *val);
bool value_is_truthy(KronosValue *val);
bool value_equals(KronosValue *a, KronosValue *b);
bool value_is_type(KronosValue *val, const char *type_name);

// Map operations
KronosValue *map_get(KronosValue *map, KronosValue *key);
int map_set(KronosValue *map, KronosValue *key, KronosValue *value);
bool map_delete(KronosValue *map, KronosValue *key);

// String interning
KronosValue *string_intern(const char *str, size_t len);

// Cleanup
void runtime_init(void);
void runtime_cleanup(void);

#endif // KRONOS_RUNTIME_H
