#ifndef KRONOS_VM_BUILTINS_H
#define KRONOS_VM_BUILTINS_H

#include "vm.h"
#include <stdint.h>

// Built-in function handler signature.
typedef int (*BuiltinHandler)(KronosVM *vm, uint8_t arg_count);

// Internal lambda/function-value invocation helper shared with builtins.
int vm_call_function_value(KronosVM *vm, KronosValue *func_val,
                           const char *func_name, uint8_t arg_count);

#endif
