#ifndef KRONOS_RUNTIME_H
#define KRONOS_RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Value types in Kronos
typedef enum {
    VAL_NUMBER,
    VAL_STRING,
    VAL_BOOL,
    VAL_NIL,
    VAL_FUNCTION,
    VAL_LIST,
    VAL_CHANNEL,
} ValueType;

// Reference-counted value
typedef struct KronosValue {
    ValueType type;
    uint32_t refcount;
    union {
        double number;
        struct {
            char* data;
            size_t length;
            uint32_t hash;
        } string;
        bool boolean;
        struct {
            uint8_t* bytecode;
            size_t length;
            int arity;
        } function;
        struct {
            struct KronosValue** items;
            size_t count;
            size_t capacity;
        } list;
    } as;
} KronosValue;

// Value creation functions
KronosValue* value_new_number(double num);
KronosValue* value_new_string(const char* str, size_t len);
KronosValue* value_new_bool(bool val);
KronosValue* value_new_nil(void);

// Reference counting
void value_retain(KronosValue* val);
void value_release(KronosValue* val);

// Value operations
void value_print(KronosValue* val);
bool value_is_truthy(KronosValue* val);
bool value_equals(KronosValue* a, KronosValue* b);
bool value_is_type(KronosValue* val, const char* type_name);

// String interning
KronosValue* string_intern(const char* str, size_t len);

// Cleanup
void runtime_init(void);
void runtime_cleanup(void);

#endif // KRONOS_RUNTIME_H

