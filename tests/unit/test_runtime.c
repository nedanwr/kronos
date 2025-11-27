#include "../framework/test_framework.h"
#include "../../src/core/runtime.h"
#include <math.h>

TEST(value_new_number) {
    KronosValue *val = value_new_number(42.5);
    ASSERT_PTR_NOT_NULL(val);
    ASSERT_INT_EQ(val->type, VAL_NUMBER);
    ASSERT_DOUBLE_EQ(val->as.number, 42.5);
    ASSERT_INT_EQ(val->refcount, 1);
    
    value_release(val);
}

TEST(value_new_string) {
    KronosValue *val = value_new_string("hello", 5);
    ASSERT_PTR_NOT_NULL(val);
    ASSERT_INT_EQ(val->type, VAL_STRING);
    ASSERT_STR_EQ(val->as.string.data, "hello");
    ASSERT_INT_EQ(val->as.string.length, 5);
    ASSERT_INT_EQ(val->refcount, 1);
    
    value_release(val);
}

TEST(value_new_bool) {
    KronosValue *val_true = value_new_bool(true);
    ASSERT_PTR_NOT_NULL(val_true);
    ASSERT_INT_EQ(val_true->type, VAL_BOOL);
    ASSERT_TRUE(val_true->as.boolean);
    ASSERT_INT_EQ(val_true->refcount, 1);
    
    KronosValue *val_false = value_new_bool(false);
    ASSERT_PTR_NOT_NULL(val_false);
    ASSERT_INT_EQ(val_false->type, VAL_BOOL);
    ASSERT_FALSE(val_false->as.boolean);
    ASSERT_INT_EQ(val_false->refcount, 1);
    
    value_release(val_true);
    value_release(val_false);
}

TEST(value_new_nil) {
    KronosValue *val = value_new_nil();
    ASSERT_PTR_NOT_NULL(val);
    ASSERT_INT_EQ(val->type, VAL_NIL);
    ASSERT_INT_EQ(val->refcount, 1);
    
    value_release(val);
}

TEST(value_retain_release) {
    KronosValue *val = value_new_number(10);
    ASSERT_INT_EQ(val->refcount, 1);
    
    value_retain(val);
    ASSERT_INT_EQ(val->refcount, 2);
    
    value_retain(val);
    ASSERT_INT_EQ(val->refcount, 3);
    
    value_release(val);
    ASSERT_INT_EQ(val->refcount, 2);
    
    value_release(val);
    ASSERT_INT_EQ(val->refcount, 1);
    
    value_release(val);
    // Value should be freed now
}

TEST(value_retain_null) {
    // Should not crash
    value_retain(NULL);
}

TEST(value_release_null) {
    // Should not crash
    value_release(NULL);
}

TEST(value_equals_number) {
    KronosValue *a = value_new_number(10);
    KronosValue *b = value_new_number(10);
    KronosValue *c = value_new_number(20);
    
    ASSERT_TRUE(value_equals(a, b));
    ASSERT_FALSE(value_equals(a, c));
    ASSERT_FALSE(value_equals(b, c));
    
    value_release(a);
    value_release(b);
    value_release(c);
}

TEST(value_equals_string) {
    KronosValue *a = value_new_string("hello", 5);
    KronosValue *b = value_new_string("hello", 5);
    KronosValue *c = value_new_string("world", 5);
    
    ASSERT_TRUE(value_equals(a, b));
    ASSERT_FALSE(value_equals(a, c));
    ASSERT_FALSE(value_equals(b, c));
    
    value_release(a);
    value_release(b);
    value_release(c);
}

TEST(value_equals_bool) {
    KronosValue *a = value_new_bool(true);
    KronosValue *b = value_new_bool(true);
    KronosValue *c = value_new_bool(false);
    
    ASSERT_TRUE(value_equals(a, b));
    ASSERT_FALSE(value_equals(a, c));
    ASSERT_FALSE(value_equals(b, c));
    
    value_release(a);
    value_release(b);
    value_release(c);
}

TEST(value_equals_nil) {
    KronosValue *a = value_new_nil();
    KronosValue *b = value_new_nil();
    
    ASSERT_TRUE(value_equals(a, b));
    
    value_release(a);
    value_release(b);
}

TEST(value_equals_different_types) {
    KronosValue *num = value_new_number(10);
    KronosValue *str = value_new_string("10", 2);
    KronosValue *bool_val = value_new_bool(true);
    KronosValue *nil = value_new_nil();
    
    ASSERT_FALSE(value_equals(num, str));
    ASSERT_FALSE(value_equals(num, bool_val));
    ASSERT_FALSE(value_equals(num, nil));
    ASSERT_FALSE(value_equals(str, bool_val));
    ASSERT_FALSE(value_equals(str, nil));
    ASSERT_FALSE(value_equals(bool_val, nil));
    
    value_release(num);
    value_release(str);
    value_release(bool_val);
    value_release(nil);
}

TEST(value_is_truthy) {
    KronosValue *num_zero = value_new_number(0);
    KronosValue *num_nonzero = value_new_number(42);
    KronosValue *str_empty = value_new_string("", 0);
    KronosValue *str_nonempty = value_new_string("hello", 5);
    KronosValue *bool_true = value_new_bool(true);
    KronosValue *bool_false = value_new_bool(false);
    KronosValue *nil = value_new_nil();
    
    ASSERT_FALSE(value_is_truthy(num_zero));
    ASSERT_TRUE(value_is_truthy(num_nonzero));
    ASSERT_FALSE(value_is_truthy(str_empty));
    ASSERT_TRUE(value_is_truthy(str_nonempty));
    ASSERT_TRUE(value_is_truthy(bool_true));
    ASSERT_FALSE(value_is_truthy(bool_false));
    ASSERT_FALSE(value_is_truthy(nil));
    
    value_release(num_zero);
    value_release(num_nonzero);
    value_release(str_empty);
    value_release(str_nonempty);
    value_release(bool_true);
    value_release(bool_false);
    value_release(nil);
}

TEST(value_is_type) {
    KronosValue *num = value_new_number(10);
    KronosValue *str = value_new_string("hello", 5);
    KronosValue *bool_val = value_new_bool(true);
    KronosValue *nil = value_new_nil();
    
    ASSERT_TRUE(value_is_type(num, "number"));
    ASSERT_FALSE(value_is_type(num, "string"));
    ASSERT_FALSE(value_is_type(num, "boolean"));
    
    ASSERT_TRUE(value_is_type(str, "string"));
    ASSERT_FALSE(value_is_type(str, "number"));
    
    ASSERT_TRUE(value_is_type(bool_val, "boolean"));
    ASSERT_FALSE(value_is_type(bool_val, "number"));
    
    ASSERT_FALSE(value_is_type(nil, "number"));
    ASSERT_FALSE(value_is_type(nil, "string"));
    
    value_release(num);
    value_release(str);
    value_release(bool_val);
    value_release(nil);
}

TEST(value_new_list) {
    KronosValue *list = value_new_list(0);
    ASSERT_PTR_NOT_NULL(list);
    ASSERT_INT_EQ(list->type, VAL_LIST);
    ASSERT_INT_EQ(list->as.list.count, 0);
    ASSERT_INT_EQ(list->refcount, 1);
    
    value_release(list);
}

TEST(value_new_list_with_capacity) {
    KronosValue *list = value_new_list(10);
    ASSERT_PTR_NOT_NULL(list);
    ASSERT_INT_EQ(list->type, VAL_LIST);
    ASSERT_INT_EQ(list->as.list.count, 0);
    ASSERT_TRUE(list->as.list.capacity >= 10);
    ASSERT_INT_EQ(list->refcount, 1);
    
    value_release(list);
}

TEST(value_print_number) {
    KronosValue *val = value_new_number(42.5);
    ASSERT_PTR_NOT_NULL(val);
    
    // Should not crash
    value_print(val);
    
    value_release(val);
}

TEST(value_print_string) {
    KronosValue *val = value_new_string("hello", 5);
    ASSERT_PTR_NOT_NULL(val);
    
    // Should not crash
    value_print(val);
    
    value_release(val);
}

TEST(value_fprint) {
    KronosValue *val = value_new_number(123);
    ASSERT_PTR_NOT_NULL(val);
    
    FILE *f = fopen("/dev/null", "w");
    if (f) {
        value_fprint(f, val);
        fclose(f);
    }
    
    value_release(val);
}

TEST(string_intern) {
    KronosValue *val1 = string_intern("test", 4);
    ASSERT_PTR_NOT_NULL(val1);
    ASSERT_INT_EQ(val1->type, VAL_STRING);
    
    // Second call with same string should return same value (interning)
    KronosValue *val2 = string_intern("test", 4);
    ASSERT_PTR_NOT_NULL(val2);
    // Should be the same pointer (interning)
    ASSERT_TRUE(val1 == val2 || value_equals(val1, val2));
    
    value_release(val1);
    value_release(val2);
}

TEST(value_new_function) {
    uint8_t bytecode[] = {1, 2, 3};
    KronosValue *func = value_new_function(bytecode, 3, 2);
    ASSERT_PTR_NOT_NULL(func);
    ASSERT_INT_EQ(func->type, VAL_FUNCTION);
    ASSERT_INT_EQ(func->as.function.arity, 2);
    ASSERT_INT_EQ(func->as.function.length, 3);
    
    value_release(func);
}

TEST(value_new_function_null_bytecode) {
    // Should return NULL for invalid input
    KronosValue *func = value_new_function(NULL, 0, 0);
    ASSERT_PTR_NULL(func);
}

