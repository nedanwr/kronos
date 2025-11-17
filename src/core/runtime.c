#include "runtime.h"
#include "gc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// String interning table
#define INTERN_TABLE_SIZE 1024
static KronosValue* intern_table[INTERN_TABLE_SIZE] = {0};

// Hash function for strings
static uint32_t hash_string(const char* str, size_t len) {
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

// Cleanup runtime
void runtime_cleanup(void) {
    // Free interned strings
    for (int i = 0; i < INTERN_TABLE_SIZE; i++) {
        if (intern_table[i] != NULL) {
            free(intern_table[i]->as.string.data);
            free(intern_table[i]);
            intern_table[i] = NULL;
        }
    }
    gc_cleanup();
}

// Create a new number value
KronosValue* value_new_number(double num) {
    KronosValue* val = malloc(sizeof(KronosValue));
    if (!val) return NULL;

    val->type = VAL_NUMBER;
    val->refcount = 1;
    val->as.number = num;

    gc_track(val);
    return val;
}

// Create a new string value
KronosValue* value_new_string(const char* str, size_t len) {
    KronosValue* val = malloc(sizeof(KronosValue));
    if (!val) return NULL;

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
KronosValue* value_new_bool(bool val) {
    KronosValue* v = malloc(sizeof(KronosValue));
    if (!v) return NULL;

    v->type = VAL_BOOL;
    v->refcount = 1;
    v->as.boolean = val;

    gc_track(v);
    return v;
}

// Create a new nil value
KronosValue* value_new_nil(void) {
    KronosValue* val = malloc(sizeof(KronosValue));
    if (!val) return NULL;

    val->type = VAL_NIL;
    val->refcount = 1;

    gc_track(val);
    return val;
}

// Retain a value (increment refcount)
void value_retain(KronosValue* val) {
    if (val) {
        val->refcount++;
    }
}

// Release a value (decrement refcount, free if 0)
void value_release(KronosValue* val) {
    if (!val) return;

    val->refcount--;
    if (val->refcount == 0) {
        gc_untrack(val);

        // Free any owned memory
        switch (val->type) {
            case VAL_STRING:
                free(val->as.string.data);
                break;
            case VAL_FUNCTION:
                free(val->as.function.bytecode);
                break;
            case VAL_LIST:
                for (size_t i = 0; i < val->as.list.count; i++) {
                    value_release(val->as.list.items[i]);
                }
                free(val->as.list.items);
                break;
            default:
                break;
        }

        free(val);
    }
}

// Print a value
void value_print(KronosValue* val) {
    if (!val) {
        printf("null");
        return;
    }

    switch (val->type) {
        case VAL_NUMBER:
            // Print integer if it's a whole number
            if (val->as.number == (long)val->as.number) {
                printf("%ld", (long)val->as.number);
            } else {
                printf("%g", val->as.number);
            }
            break;
        case VAL_STRING:
            printf("%s", val->as.string.data);
            break;
        case VAL_BOOL:
            printf("%s", val->as.boolean ? "true" : "false");
            break;
        case VAL_NIL:
            printf("null");
            break;
        case VAL_FUNCTION:
            printf("<function>");
            break;
        case VAL_LIST:
            printf("[");
            for (size_t i = 0; i < val->as.list.count; i++) {
                if (i > 0) printf(", ");
                value_print(val->as.list.items[i]);
            }
            printf("]");
            break;
        case VAL_CHANNEL:
            printf("<channel>");
            break;
    }
}

// Check if a value is truthy
bool value_is_truthy(KronosValue* val) {
    if (!val) return false;

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
bool value_equals(KronosValue* a, KronosValue* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->type != b->type) return false;

    switch (a->type) {
        case VAL_NUMBER:
            return a->as.number == b->as.number;
        case VAL_STRING:
            return a->as.string.length == b->as.string.length &&
                   memcmp(a->as.string.data, b->as.string.data, a->as.string.length) == 0;
        case VAL_BOOL:
            return a->as.boolean == b->as.boolean;
        case VAL_NIL:
            return true;
        default:
            return a == b; // Pointer equality for complex types
    }
}

// String interning
KronosValue* string_intern(const char* str, size_t len) {
    uint32_t hash = hash_string(str, len);
    size_t index = hash % INTERN_TABLE_SIZE;

    // Linear probing
    for (size_t i = 0; i < INTERN_TABLE_SIZE; i++) {
        size_t probe = (index + i) % INTERN_TABLE_SIZE;
        KronosValue* entry = intern_table[probe];

        if (entry == NULL) {
            // Not found, create new interned string
            KronosValue* val = value_new_string(str, len);
            if (val) {
                intern_table[probe] = val;
                value_retain(val); // Extra ref for intern table
            }
            return val;
        }

        if (entry->type == VAL_STRING &&
            entry->as.string.hash == hash &&
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
bool value_is_type(KronosValue* val, const char* type_name) {
    if (!val || !type_name) return false;

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

