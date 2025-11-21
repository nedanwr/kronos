#define _POSIX_C_SOURCE 200809L
#include "vm.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void vm_finalize_error(KronosVM *vm, KronosErrorCode code,
                              char *owned_message, const char *fallback_msg) {
  if (!vm)
    return;

  free(vm->last_error_message);
  vm->last_error_message = owned_message;
  vm->last_error_code = code;

  if (vm->error_callback && code != KRONOS_OK) {
    const char *callback_msg = vm->last_error_message
                                   ? vm->last_error_message
                                   : (fallback_msg ? fallback_msg : "");
    vm->error_callback(vm, code, callback_msg);
  }
}

static char *vm_format_message(const char *fmt, va_list args) {
  if (!fmt)
    return NULL;

  va_list copy;
  va_copy(copy, args);
  int needed = vsnprintf(NULL, 0, fmt, copy);
  va_end(copy);

  if (needed < 0)
    return NULL;

  size_t size = (size_t)needed + 1;
  char *buffer = malloc(size);
  if (!buffer)
    return NULL;

  if (vsnprintf(buffer, size, fmt, args) < 0) {
    free(buffer);
    return NULL;
  }

  return buffer;
}

void vm_clear_error(KronosVM *vm) {
  vm_finalize_error(vm, KRONOS_OK, NULL, NULL);
}

void vm_set_error(KronosVM *vm, KronosErrorCode code, const char *message) {
  char *copy = NULL;
  if (message) {
    copy = strdup(message);
  }
  vm_finalize_error(vm, code, copy, message);
}

void vm_set_errorf(KronosVM *vm, KronosErrorCode code, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  char *message = vm_format_message(fmt, args);
  va_end(args);
  vm_finalize_error(vm, code, message, fmt);
}

int vm_error(KronosVM *vm, KronosErrorCode code, const char *message) {
  vm_set_error(vm, code, message);
  return code == KRONOS_OK ? 0 : -(int)code;
}

int vm_errorf(KronosVM *vm, KronosErrorCode code, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  char *message = vm_format_message(fmt, args);
  va_end(args);
  vm_finalize_error(vm, code, message, fmt);
  return code == KRONOS_OK ? 0 : -(int)code;
}

static int vm_propagate_error(KronosVM *vm, KronosErrorCode fallback) {
  KronosErrorCode code =
      (vm && vm->last_error_code != KRONOS_OK) ? vm->last_error_code : fallback;
  return code == KRONOS_OK ? -(int)fallback : -(int)code;
}

// Create new VM
KronosVM *vm_new(void) {
  KronosVM *vm = malloc(sizeof(KronosVM));
  if (!vm)
    return NULL;

  vm->stack_top = vm->stack;
  vm->global_count = 0;
  vm->function_count = 0;
  vm->call_stack_size = 0;
  vm->current_frame = NULL;
  vm->ip = NULL;
  vm->bytecode = NULL;

  vm->last_error_message = NULL;
  vm->last_error_code = KRONOS_OK;
  vm->error_callback = NULL;

  // Initialize Pi constant - immutable
  // Note: double precision provides ~15-17 decimal digits of precision
  KronosValue *pi_value = value_new_number(3.1415926535897932);
  if (!pi_value) {
    free(vm);
    return NULL;
  }

  // Manually add Pi as immutable global
  if (vm->global_count < GLOBALS_MAX) {
    // Allocate into temporary pointers first
    char *name_copy = strdup("Pi");
    if (!name_copy) {
      value_release(pi_value);
      free(vm);
      return NULL;
    }

    char *type_copy = strdup("number");
    if (!type_copy) {
      free(name_copy);
      value_release(pi_value);
      free(vm);
      return NULL;
    }

    // Only assign to vm->globals after both allocations succeed
    vm->globals[vm->global_count].name = name_copy;
    vm->globals[vm->global_count].value = pi_value;
    vm->globals[vm->global_count].is_mutable = false; // Immutable!
    vm->globals[vm->global_count].type_name = type_copy;
    // No value_retain needed - globals array owns the single reference
    vm->global_count++;
  }

  return vm;
}

// Free VM
void vm_free(KronosVM *vm) {
  if (!vm)
    return;

  // Release all values on stack
  while (vm->stack_top > vm->stack) {
    vm->stack_top--;
    value_release(*vm->stack_top);
  }

  // Release call frames
  for (size_t i = 0; i < vm->call_stack_size; i++) {
    CallFrame *frame = &vm->call_stack[i];
    for (size_t j = 0; j < frame->local_count; j++) {
      free(frame->locals[j].name);
      value_release(frame->locals[j].value);
      free(frame->locals[j].type_name);
    }
  }

  // Release global variables
  for (size_t i = 0; i < vm->global_count; i++) {
    free(vm->globals[i].name);
    value_release(vm->globals[i].value);
    free(vm->globals[i].type_name);
  }

  // Release functions
  for (size_t i = 0; i < vm->function_count; i++) {
    function_free(vm->functions[i]);
  }

  free(vm->last_error_message);
  free(vm);
}

// Free a function
void function_free(Function *func) {
  if (!func)
    return;

  free(func->name);
  for (size_t i = 0; i < func->param_count; i++) {
    free(func->params[i]);
  }
  free(func->params);

  // Free bytecode structure
  free(func->bytecode.code);
  for (size_t i = 0; i < func->bytecode.const_count; i++) {
    value_release(func->bytecode.constants[i]);
  }
  free(func->bytecode.constants);

  free(func);
}

// Define a function
int vm_define_function(KronosVM *vm, Function *func) {
  if (!vm || !func) {
    return vm_error(vm, KRONOS_ERR_INVALID_ARGUMENT,
                    "vm_define_function requires non-null inputs");
  }

  if (vm->function_count >= FUNCTIONS_MAX) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Maximum number of functions exceeded (%d allowed)",
                     FUNCTIONS_MAX);
  }

  vm->functions[vm->function_count++] = func;
  return 0;
}

// Get a function by name
Function *vm_get_function(KronosVM *vm, const char *name) {
  for (size_t i = 0; i < vm->function_count; i++) {
    if (strcmp(vm->functions[i]->name, name) == 0) {
      return vm->functions[i];
    }
  }
  return NULL;
}

// Stack operations
static void push(KronosVM *vm, KronosValue *value) {
  if (vm->stack_top >= vm->stack + STACK_MAX) {
    vm_set_errorf(vm, KRONOS_ERR_RUNTIME,
                  "Stack overflow (too many nested operations or calls)");
    return;
  }
  *vm->stack_top = value;
  vm->stack_top++;
  value_retain(value); // Retain while on stack
}

static KronosValue *pop(KronosVM *vm) {
  if (vm->stack_top <= vm->stack) {
    vm_set_error(vm, KRONOS_ERR_RUNTIME,
                 "Stack underflow (internal error - please report this bug)");
    return NULL;
  }
  vm->stack_top--;
  KronosValue *val = *vm->stack_top;
  return val;
}

static KronosValue *peek(KronosVM *vm, int distance) {
  // Bounds checking: ensure distance is valid
  // Guard: distance must be >= 0 and < stack size
  if (distance < 0) {
    fprintf(stderr,
            "Fatal error: peek: distance must be non-negative (got %d)\n",
            distance);
    fflush(stderr);
    abort();
  }

  // Compute current stack size
  size_t stack_size = vm->stack_top - vm->stack;

  // Guard: distance must be < stack size to access valid memory
  if ((size_t)distance >= stack_size) {
    fprintf(stderr, "Fatal error: peek: distance %d exceeds stack size %zu\n",
            distance, stack_size);
    fflush(stderr);
    abort();
  }

  return vm->stack_top[-1 - distance];
}

// Global variable management
int vm_set_global(KronosVM *vm, const char *name, KronosValue *value,
                  bool is_mutable, const char *type_name) {
  if (!vm || !name || !value) {
    return vm_error(vm, KRONOS_ERR_INVALID_ARGUMENT,
                    "vm_set_global requires non-null inputs");
  }

  // Check if variable already exists
  for (size_t i = 0; i < vm->global_count; i++) {
    if (strcmp(vm->globals[i].name, name) == 0) {
      // Check if it's immutable
      if (!vm->globals[i].is_mutable) {
        return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                         "Cannot reassign immutable variable '%s'", name);
      }

      // Check type if specified
      if (vm->globals[i].type_name != NULL &&
          !value_is_type(value, vm->globals[i].type_name)) {
        return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                         "Type mismatch for variable '%s': expected '%s'", name,
                         vm->globals[i].type_name);
      }

      value_release(vm->globals[i].value);
      vm->globals[i].value = value;
      value_retain(value);
      return 0;
    }
  }

  // Add new global
  if (vm->global_count >= GLOBALS_MAX) {
    fprintf(stderr,
            "Fatal error: Maximum number of global variables exceeded (%d "
            "allowed)\n",
            GLOBALS_MAX);
    fflush(stderr);
    exit(1);
  }

  // Allocate into temporary pointers first, check each for NULL
  char *name_copy = strdup(name);
  if (!name_copy) {
    return vm_error(vm, KRONOS_ERR_INTERNAL,
                    "Failed to allocate memory for variable name");
  }

  char *type_copy = NULL;
  if (type_name) {
    type_copy = strdup(type_name);
    if (!type_copy) {
      // Free already-allocated name_copy on failure
      free(name_copy);
      return vm_error(vm, KRONOS_ERR_INTERNAL,
                      "Failed to allocate memory for type name");
    }
  }

  // Call value_retain before modifying vm->globals
  value_retain(value);

  // Only assign to vm->globals after all allocations succeed
  vm->globals[vm->global_count].name = name_copy;
  vm->globals[vm->global_count].value = value;
  vm->globals[vm->global_count].is_mutable = is_mutable;
  vm->globals[vm->global_count].type_name = type_copy;

  // Only increment global_count after everything succeeds
  vm->global_count++;
  return 0;
}

KronosValue *vm_get_global(KronosVM *vm, const char *name) {
  for (size_t i = 0; i < vm->global_count; i++) {
    if (strcmp(vm->globals[i].name, name) == 0) {
      return vm->globals[i].value;
    }
  }
  return NULL;
}

// Set local variable in current frame
int vm_set_local(KronosVM *vm, CallFrame *frame, const char *name,
                 KronosValue *value, bool is_mutable, const char *type_name) {
  if (!vm || !frame || !name || !value)
    return vm_error(vm, KRONOS_ERR_INVALID_ARGUMENT,
                    "vm_set_local requires non-null inputs");

  // Check if variable already exists
  for (size_t i = 0; i < frame->local_count; i++) {
    if (strcmp(frame->locals[i].name, name) == 0) {
      // Check if it's immutable
      if (!frame->locals[i].is_mutable) {
        return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                         "Cannot reassign immutable local variable '%s'", name);
      }

      // Check type if specified
      if (frame->locals[i].type_name != NULL &&
          !value_is_type(value, frame->locals[i].type_name)) {
        return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                         "Type mismatch for local variable '%s': expected '%s'",
                         name, frame->locals[i].type_name);
      }

      value_release(frame->locals[i].value);
      frame->locals[i].value = value;
      value_retain(value);
      return 0;
    }
  }

  // Add new local variable
  if (frame->local_count >= LOCALS_MAX) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Maximum number of local variables exceeded (%d allowed)",
                     LOCALS_MAX);
  }

  // Allocate into temporary pointers first, check each for NULL
  char *name_copy = strdup(name);
  if (!name_copy) {
    // Allocation failure: release value and return error without modifying
    // frame state
    value_release(value);
    return vm_error(vm, KRONOS_ERR_INTERNAL,
                    "Failed to allocate memory for local name");
  }

  char *type_copy = NULL;
  if (type_name) {
    type_copy = strdup(type_name);
    if (!type_copy) {
      // Allocation failure: free already-allocated name_copy, release value,
      // and return error
      free(name_copy);
      value_release(value);
      return vm_error(vm, KRONOS_ERR_INTERNAL,
                      "Failed to allocate memory for local type");
    }
  }

  // Only assign to frame->locals[...] after all allocations succeed
  frame->locals[frame->local_count].name = name_copy;
  frame->locals[frame->local_count].value = value;
  frame->locals[frame->local_count].is_mutable = is_mutable;
  frame->locals[frame->local_count].type_name = type_copy;

  // Only call value_retain after all allocations and assignments succeed
  value_retain(value);
  // Only increment frame->local_count after everything succeeds
  frame->local_count++;
  return 0;
}

// Get local variable from current frame
KronosValue *vm_get_local(CallFrame *frame, const char *name) {
  if (!frame)
    return NULL;

  for (size_t i = 0; i < frame->local_count; i++) {
    if (strcmp(frame->locals[i].name, name) == 0) {
      return frame->locals[i].value;
    }
  }

  return NULL;
}

// Get variable (try local first, then global)
KronosValue *vm_get_variable(KronosVM *vm, const char *name) {
  // Try local variables if in function
  if (vm->current_frame) {
    KronosValue *local = vm_get_local(vm->current_frame, name);
    if (local)
      return local;
  }

  // Try global variables
  KronosValue *global = vm_get_global(vm, name);
  if (global)
    return global;

  vm_set_errorf(vm, KRONOS_ERR_NOT_FOUND, "Undefined variable '%s'", name);
  return NULL;
}

// Read byte from bytecode
static uint8_t read_byte(KronosVM *vm) {
  // Compute current offset and compare against bytecode count
  size_t offset = vm->ip - vm->bytecode->code;
  if (offset >= vm->bytecode->count) {
    // Out of bounds: set error state and return sentinel value
    // Do not increment vm->ip when out of range
    vm_set_error(
        vm, KRONOS_ERR_RUNTIME,
        "Bytecode read out of bounds (truncated or malformed bytecode)");
    // Return OP_HALT to stop execution gracefully
    return OP_HALT;
  }
  // Safe to dereference and increment
  return *vm->ip++;
}

// Read 16-bit value (big-endian)
static uint16_t read_uint16(KronosVM *vm) {
  uint16_t high = read_byte(vm);
  uint16_t low = read_byte(vm);
  return (high << 8) | low;
}

// Read constant from pool
static KronosValue *read_constant(KronosVM *vm) {
  uint16_t idx = read_uint16(vm);
  // Validate index is within bounds of constants array
  if (idx >= vm->bytecode->const_count) {
    vm_set_errorf(vm, KRONOS_ERR_RUNTIME,
                  "Constant index out of bounds: %u (valid range: 0-%zu)", idx,
                  vm->bytecode->const_count > 0 ? vm->bytecode->const_count - 1
                                                : 0);
    return NULL;
  }
  return vm->bytecode->constants[idx];
}

// Execute bytecode
int vm_execute(KronosVM *vm, Bytecode *bytecode) {
  if (!vm) {
    return -(int)KRONOS_ERR_INVALID_ARGUMENT;
  }
  if (!bytecode) {
    return vm_error(vm, KRONOS_ERR_INVALID_ARGUMENT,
                    "vm_execute: bytecode must not be NULL");
  }

  vm->bytecode = bytecode;
  vm->ip = bytecode->code;

  while (1) {
    uint8_t instruction = read_byte(vm);

    switch (instruction) {
    case OP_LOAD_CONST: {
      KronosValue *constant = read_constant(vm);
      if (!constant) {
        return vm_propagate_error(vm, KRONOS_ERR_INTERNAL);
      }
      push(vm, constant);
      break;
    }

    case OP_LOAD_VAR: {
      KronosValue *name_val = read_constant(vm);
      if (!name_val) {
        return vm_propagate_error(vm, KRONOS_ERR_INTERNAL);
      }
      if (name_val->type != VAL_STRING) {
        return vm_error(vm, KRONOS_ERR_INTERNAL,
                        "Variable name constant is not a string");
      }
      KronosValue *value = vm_get_variable(vm, name_val->as.string.data);
      if (!value) {
        // Debug: check what error was set
        if (vm->last_error_code == KRONOS_ERR_NOT_FOUND) {
          fprintf(stderr, "DEBUG: OP_LOAD_VAR failed to find variable '%s'\n",
                  name_val->as.string.data);
        }
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
      push(vm, value);
      break;
    }

    case OP_STORE_VAR: {
      KronosValue *name_val = read_constant(vm);
      if (!name_val) {
        return vm_propagate_error(vm, KRONOS_ERR_INTERNAL);
      }
      if (name_val->type != VAL_STRING) {
        return vm_error(vm, KRONOS_ERR_INTERNAL,
                        "Variable name constant is not a string");
      }
      KronosValue *value = pop(vm);
      if (!value) {
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }

      // Read mutability flag
      uint8_t is_mutable_byte = read_byte(vm);
      bool is_mutable = (is_mutable_byte == 1);

      // Read type name (if specified)
      uint8_t has_type = read_byte(vm);
      const char *type_name = NULL;
      if (has_type) {
        KronosValue *type_val = read_constant(vm);
        if (!type_val) {
          value_release(value);
          return vm_propagate_error(vm, KRONOS_ERR_INTERNAL);
        }
        if (type_val->type != VAL_STRING) {
          value_release(value);
          return vm_error(vm, KRONOS_ERR_INTERNAL,
                          "Type name constant is not a string");
        }
        type_name = type_val->as.string.data;
      }

      // If in function, set as local variable; otherwise, set as global
      int store_status;
      if (vm->current_frame) {
        store_status =
            vm_set_local(vm, vm->current_frame, name_val->as.string.data, value,
                         is_mutable, type_name);
      } else {
        store_status = vm_set_global(vm, name_val->as.string.data, value,
                                     is_mutable, type_name);
      }

      value_release(value); // Release our reference
      if (store_status != 0) {
        return store_status;
      }
      break;
    }

    case OP_PRINT: {
      KronosValue *value = pop(vm);
      if (!value) {
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
      value_fprint(stdout, value);
      printf("\n");
      value_release(value);
      break;
    }

    case OP_ADD: {
      KronosValue *b = pop(vm);
      if (!b) {
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
      KronosValue *a = pop(vm);
      if (!a) {
        value_release(b);
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }

      if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
        KronosValue *result = value_new_number(a->as.number + b->as.number);
        push(vm, result);
        value_release(result); // Push retains it
      } else if (a->type == VAL_STRING && b->type == VAL_STRING) {
        // String concatenation
        size_t len_a = a->as.string.length;
        size_t len_b = b->as.string.length;
        size_t total_len = len_a + len_b;

        char *concat = malloc(total_len + 1);
        if (!concat) {
          value_release(a);
          value_release(b);
          return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate memory for string concatenation");
        }

        memcpy(concat, a->as.string.data, len_a);
        memcpy(concat + len_a, b->as.string.data, len_b);
        concat[total_len] = '\0';

        KronosValue *result = value_new_string(concat, total_len);
        free(concat);
        if (!result) {
          value_release(a);
          value_release(b);
          return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string value");
        }

        push(vm, result);
        value_release(result);
      } else {
        int err = vm_error(vm, KRONOS_ERR_RUNTIME,
                           "Cannot add - both values must be numbers or both must be strings");
        value_release(a);
        value_release(b);
        return err;
      }

      value_release(a);
      value_release(b);
      break;
    }

    case OP_SUB: {
      KronosValue *b = pop(vm);
      if (!b) {
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
      KronosValue *a = pop(vm);
      if (!a) {
        value_release(b);
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }

      if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
        KronosValue *result = value_new_number(a->as.number - b->as.number);
        push(vm, result);
        value_release(result);
      } else {
        int err = vm_error(vm, KRONOS_ERR_RUNTIME,
                           "Cannot subtract - both values must be numbers");
        value_release(a);
        value_release(b);
        return err;
      }

      value_release(a);
      value_release(b);
      break;
    }

    case OP_MUL: {
      KronosValue *b = pop(vm);
      if (!b) {
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
      KronosValue *a = pop(vm);
      if (!a) {
        value_release(b);
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }

      if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
        KronosValue *result = value_new_number(a->as.number * b->as.number);
        push(vm, result);
        value_release(result);
      } else {
        int err = vm_error(vm, KRONOS_ERR_RUNTIME,
                           "Cannot multiply - both values must be numbers");
        value_release(a);
        value_release(b);
        return err;
      }

      value_release(a);
      value_release(b);
      break;
    }

    case OP_DIV: {
      KronosValue *b = pop(vm);
      if (!b) {
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
      KronosValue *a = pop(vm);
      if (!a) {
        value_release(b);
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }

      if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
        if (b->as.number == 0) {
          int err = vm_error(vm, KRONOS_ERR_RUNTIME, "Cannot divide by zero");
          value_release(a);
          value_release(b);
          return err;
        }
        KronosValue *result = value_new_number(a->as.number / b->as.number);
        push(vm, result);
        value_release(result);
      } else {
        int err = vm_error(vm, KRONOS_ERR_RUNTIME,
                           "Cannot divide - both values must be numbers");
        value_release(a);
        value_release(b);
        return err;
      }

      value_release(a);
      value_release(b);
      break;
    }

    case OP_EQ: {
      KronosValue *b = pop(vm);
      if (!b) {
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
      KronosValue *a = pop(vm);
      if (!a) {
        value_release(b);
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
      bool result = value_equals(a, b);
      KronosValue *res = value_new_bool(result);
      push(vm, res);
      value_release(res);
      value_release(a);
      value_release(b);
      break;
    }

    case OP_NEQ: {
      KronosValue *b = pop(vm);
      if (!b) {
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
      KronosValue *a = pop(vm);
      if (!a) {
        value_release(b);
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
      bool result = !value_equals(a, b);
      KronosValue *res = value_new_bool(result);
      push(vm, res);
      value_release(res);
      value_release(a);
      value_release(b);
      break;
    }

    case OP_GT: {
      KronosValue *b = pop(vm);
      if (!b) {
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
      KronosValue *a = pop(vm);
      if (!a) {
        value_release(b);
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }

      if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
        bool result = a->as.number > b->as.number;
        KronosValue *res = value_new_bool(result);
        push(vm, res);
        value_release(res);
      } else {
        int err = vm_error(vm, KRONOS_ERR_RUNTIME,
                           "Cannot perform '>' - both values must be numbers");
        value_release(a);
        value_release(b);
        return err;
      }

      value_release(a);
      value_release(b);
      break;
    }

    case OP_LT: {
      KronosValue *b = pop(vm);
      if (!b) {
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
      KronosValue *a = pop(vm);
      if (!a) {
        value_release(b);
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }

      if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
        bool result = a->as.number < b->as.number;
        KronosValue *res = value_new_bool(result);
        push(vm, res);
        value_release(res);
      } else {
        int err = vm_error(vm, KRONOS_ERR_RUNTIME,
                           "Cannot perform '<' - both values must be numbers");
        value_release(a);
        value_release(b);
        return err;
      }

      value_release(a);
      value_release(b);
      break;
    }

    case OP_GTE: {
      KronosValue *b = pop(vm);
      if (!b) {
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
      KronosValue *a = pop(vm);
      if (!a) {
        value_release(b);
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }

      if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
        bool result = a->as.number >= b->as.number;
        KronosValue *res = value_new_bool(result);
        push(vm, res);
        value_release(res);
      } else {
        int err = vm_error(vm, KRONOS_ERR_RUNTIME,
                           "Cannot perform '>=' - both values must be numbers");
        value_release(a);
        value_release(b);
        return err;
      }

      value_release(a);
      value_release(b);
      break;
    }

    case OP_LTE: {
      KronosValue *b = pop(vm);
      if (!b) {
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
      KronosValue *a = pop(vm);
      if (!a) {
        value_release(b);
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }

      if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
        bool result = a->as.number <= b->as.number;
        KronosValue *res = value_new_bool(result);
        push(vm, res);
        value_release(res);
      } else {
        int err = vm_error(vm, KRONOS_ERR_RUNTIME,
                           "Cannot perform '<=' - both values must be numbers");
        value_release(a);
        value_release(b);
        return err;
      }

      value_release(a);
      value_release(b);
      break;
    }

    case OP_AND: {
      KronosValue *b = pop(vm);
      if (!b) {
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
      KronosValue *a = pop(vm);
      if (!a) {
        value_release(b);
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }

      // Both operands must be truthy for AND to be true
      bool a_truthy = value_is_truthy(a);
      bool b_truthy = value_is_truthy(b);
      bool result = a_truthy && b_truthy;
      KronosValue *res = value_new_bool(result);
      push(vm, res);
      value_release(res);
      value_release(a);
      value_release(b);
      break;
    }

    case OP_OR: {
      KronosValue *b = pop(vm);
      if (!b) {
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
      KronosValue *a = pop(vm);
      if (!a) {
        value_release(b);
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }

      // At least one operand must be truthy for OR to be true
      bool a_truthy = value_is_truthy(a);
      bool b_truthy = value_is_truthy(b);
      bool result = a_truthy || b_truthy;
      KronosValue *res = value_new_bool(result);
      push(vm, res);
      value_release(res);
      value_release(a);
      value_release(b);
      break;
    }

    case OP_NOT: {
      KronosValue *a = pop(vm);
      if (!a) {
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }

      // NOT returns the opposite of the truthiness
      bool a_truthy = value_is_truthy(a);
      bool result = !a_truthy;
      KronosValue *res = value_new_bool(result);
      push(vm, res);
      value_release(res);
      value_release(a);
      break;
    }

    case OP_JUMP: {
      int8_t offset = (int8_t)read_byte(vm);
      uint8_t *new_ip = vm->ip + offset;
      // Bounds check: ensure jump target is within valid bytecode range
      if (new_ip < vm->bytecode->code ||
          new_ip >= vm->bytecode->code + vm->bytecode->count) {
        return vm_errorf(
            vm, KRONOS_ERR_RUNTIME,
            "Jump target out of bounds (offset: %d, bytecode size: %zu)",
            offset, vm->bytecode->count);
      }
      vm->ip = new_ip;
      break;
    }

    case OP_JUMP_IF_FALSE: {
      uint8_t offset = read_byte(vm);
      KronosValue *condition = peek(vm, 0);
      if (!value_is_truthy(condition)) {
        uint8_t *new_ip = vm->ip + offset;
        // Bounds check: ensure jump target is within valid bytecode range
        if (new_ip < vm->bytecode->code ||
            new_ip >= vm->bytecode->code + vm->bytecode->count) {
          // Pop condition before returning error
          KronosValue *condition_val = pop(vm);
          if (condition_val) {
            value_release(condition_val);
          }
          return vm_errorf(
              vm, KRONOS_ERR_RUNTIME,
              "Jump target out of bounds (offset: %u, bytecode size: %zu)",
              offset, vm->bytecode->count);
        }
        vm->ip = new_ip;
      }
      KronosValue *condition_val = pop(vm);
      if (!condition_val) {
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
      value_release(condition_val); // Pop condition
      break;
    }

    case OP_DEFINE_FUNC: {
      // Read function name
      KronosValue *name_val = read_constant(vm);
      if (!name_val) {
        return vm_propagate_error(vm, KRONOS_ERR_INTERNAL);
      }
      if (name_val->type != VAL_STRING) {
        return vm_error(vm, KRONOS_ERR_INTERNAL,
                        "Function name constant is not a string");
      }
      uint8_t param_count = read_byte(vm);

      // Create function
      Function *func = malloc(sizeof(Function));
      if (!func) {
        return vm_error(vm, KRONOS_ERR_INTERNAL,
                        "Failed to allocate function structure");
      }

      // Allocate function name - check for NULL immediately after strdup
      func->name = strdup(name_val->as.string.data);
      if (!func->name) {
        // Allocation failure: free func and return error
        free(func);
        return vm_error(vm, KRONOS_ERR_INTERNAL,
                        "Failed to copy function name");
      }

      func->param_count = param_count;
      func->params =
          param_count > 0 ? malloc(sizeof(char *) * param_count) : NULL;
      if (param_count > 0 && !func->params) {
        // Allocation failure: free func->name and func, then return error
        free(func->name);
        free(func);
        return vm_error(vm, KRONOS_ERR_INTERNAL,
                        "Failed to allocate parameter array");
      }

      // Read parameter names
      int param_error = 0;
      size_t filled_params = 0;
      for (size_t i = 0; i < param_count; i++) {
        KronosValue *param_val = read_constant(vm);
        if (!param_val) {
          param_error = vm_propagate_error(vm, KRONOS_ERR_INTERNAL);
          break;
        }
        if (param_val->type != VAL_STRING) {
          param_error = vm_error(vm, KRONOS_ERR_INTERNAL,
                                 "Parameter name constant is not a string");
          break;
        }

        // Check if parameter name is a reserved constant
        // Note: This check happens before strdup, so no cleanup needed here
        if (strcmp(param_val->as.string.data, "Pi") == 0) {
          param_error =
              vm_error(vm, KRONOS_ERR_RUNTIME,
                       "Cannot use 'Pi' as a parameter name (reserved)");
          break;
        }

        // Allocate parameter name - check for NULL immediately after strdup
        func->params[i] = strdup(param_val->as.string.data);
        if (!func->params[i]) {
          // Allocation failure: set error and break (cleanup happens below)
          param_error = vm_error(vm, KRONOS_ERR_INTERNAL,
                                 "Failed to copy parameter name");
          break;
        }
        // Only increment filled_params after successful allocation
        filled_params++;
      }

      // Cleanup on any error: free all allocated resources
      if (param_error != 0) {
        // Free all successfully allocated parameter names (0..filled_params-1)
        for (size_t j = 0; j < filled_params; j++) {
          free(func->params[j]);
        }
        // Free parameter array if allocated
        free(func->params);
        // Free function name
        free(func->name);
        // Free function structure
        free(func);
        return param_error;
      }

      // Consume function body start position (2 bytes) - part of bytecode
      // format but not used at runtime; we just need to advance the instruction
      // pointer Format:
      // [OP_DEFINE_FUNC][name_idx:2][param_count:1][params:2*N][body_start:2][OP_JUMP][skip_offset:1]
      read_byte(vm); // body_start high byte
      read_byte(vm); // body_start low byte

      // Consume OP_JUMP instruction byte (part of bytecode format)
      read_byte(vm);

      // Read jump offset to skip function body
      uint8_t skip_offset = read_byte(vm);

      // Calculate body end (before the jump we just read)
      uint8_t *body_end_ptr = vm->ip + skip_offset;

      // Validate that body_end_ptr is within valid bytecode bounds
      if (body_end_ptr < vm->bytecode->code ||
          body_end_ptr > vm->bytecode->code + vm->bytecode->count) {
        function_free(func);
        return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                         "Function body extends beyond bytecode bounds "
                         "(offset: %u, bytecode size: %zu)",
                         skip_offset, vm->bytecode->count);
      }

      // Copy function body bytecode
      // Validate that body_end_ptr >= vm->ip to prevent wrap-around
      if (body_end_ptr < vm->ip) {
        function_free(func);
        return vm_errorf(
            vm, KRONOS_ERR_RUNTIME,
            "Invalid function body: backward jump detected (offset: %u)",
            skip_offset);
      }

      size_t bytecode_size = body_end_ptr - vm->ip;
      func->bytecode.code = malloc(bytecode_size);
      if (!func->bytecode.code) {
        // Allocation failure: clean up func (name, params, etc.) and return
        // error
        function_free(func);
        return vm_error(vm, KRONOS_ERR_INTERNAL,
                        "Failed to allocate memory for function bytecode");
      }
      func->bytecode.count = bytecode_size;
      func->bytecode.capacity = bytecode_size;
      memcpy(func->bytecode.code, vm->ip, bytecode_size);

      // Copy constants (retain references)
      func->bytecode.const_count = vm->bytecode->const_count;
      func->bytecode.const_capacity = vm->bytecode->const_count;
      func->bytecode.constants =
          malloc(sizeof(KronosValue *) * func->bytecode.const_count);
      if (!func->bytecode.constants) {
        // Allocation failure: free func->bytecode.code, then clean up func and
        // return error
        free(func->bytecode.code);
        func->bytecode.code = NULL;
        func->bytecode.count = 0;
        func->bytecode.capacity = 0;
        function_free(func);
        return vm_error(vm, KRONOS_ERR_INTERNAL,
                        "Failed to allocate memory for function constants");
      }
      for (size_t i = 0; i < func->bytecode.const_count; i++) {
        func->bytecode.constants[i] = vm->bytecode->constants[i];
        value_retain(func->bytecode.constants[i]);
      }

      // Store function
      int define_status = vm_define_function(vm, func);
      if (define_status != 0) {
        function_free(func);
        return define_status;
      }

      // Skip over function body
      vm->ip = body_end_ptr;
      break;
    }

    case OP_CALL_FUNC: {
      KronosValue *name_val = read_constant(vm);
      if (!name_val) {
        return vm_propagate_error(vm, KRONOS_ERR_INTERNAL);
      }
      if (name_val->type != VAL_STRING) {
        return vm_error(vm, KRONOS_ERR_INTERNAL,
                        "Function name constant is not a string");
      }
      uint8_t arg_count = read_byte(vm);

      // Check for built-in functions first
      const char *func_name = name_val->as.string.data;

      // Built-in: add(a, b)
      if (strcmp(func_name, "add") == 0) {
        if (arg_count != 2) {
          return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                           "Function 'add' expects 2 arguments, got %d",
                           arg_count);
        }
        KronosValue *b = pop(vm);
        if (!b) {
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        KronosValue *a = pop(vm);
        if (!a) {
          value_release(b);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
          KronosValue *result = value_new_number(a->as.number + b->as.number);
          push(vm, result);
          value_release(result);
        } else {
          int err =
              vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'add' requires both arguments to be numbers");
          value_release(a);
          value_release(b);
          return err;
        }
        value_release(a);
        value_release(b);
        break;
      }

      // Built-in: subtract(a, b)
      if (strcmp(func_name, "subtract") == 0) {
        if (arg_count != 2) {
          return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                           "Function 'subtract' expects 2 arguments, got %d",
                           arg_count);
        }
        KronosValue *b = pop(vm);
        if (!b) {
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        KronosValue *a = pop(vm);
        if (!a) {
          value_release(b);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
          KronosValue *result = value_new_number(a->as.number - b->as.number);
          push(vm, result);
          value_release(result);
        } else {
          int err = vm_errorf(
              vm, KRONOS_ERR_RUNTIME,
              "Function 'subtract' requires both arguments to be numbers");
          value_release(a);
          value_release(b);
          return err;
        }
        value_release(a);
        value_release(b);
        break;
      }

      // Built-in: multiply(a, b)
      if (strcmp(func_name, "multiply") == 0) {
        if (arg_count != 2) {
          return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                           "Function 'multiply' expects 2 arguments, got %d",
                           arg_count);
        }
        KronosValue *b = pop(vm);
        if (!b) {
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        KronosValue *a = pop(vm);
        if (!a) {
          value_release(b);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
          KronosValue *result = value_new_number(a->as.number * b->as.number);
          push(vm, result);
          value_release(result);
        } else {
          int err = vm_errorf(
              vm, KRONOS_ERR_RUNTIME,
              "Function 'multiply' requires both arguments to be numbers");
          value_release(a);
          value_release(b);
          return err;
        }
        value_release(a);
        value_release(b);
        break;
      }

      // Built-in: divide(a, b)
      if (strcmp(func_name, "divide") == 0) {
        if (arg_count != 2) {
          return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                           "Function 'divide' expects 2 arguments, got %d",
                           arg_count);
        }
        KronosValue *b = pop(vm);
        if (!b) {
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        KronosValue *a = pop(vm);
        if (!a) {
          value_release(b);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
          if (b->as.number == 0) {
            int err = vm_error(vm, KRONOS_ERR_RUNTIME,
                               "Function 'divide' cannot divide by zero");
            value_release(a);
            value_release(b);
            return err;
          } else {
            KronosValue *result = value_new_number(a->as.number / b->as.number);
            push(vm, result);
            value_release(result);
          }
        } else {
          int err = vm_errorf(
              vm, KRONOS_ERR_RUNTIME,
              "Function 'divide' requires both arguments to be numbers");
          value_release(a);
          value_release(b);
          return err;
        }
        value_release(a);
        value_release(b);
        break;
      }

      // Built-in: len(list/string)
      if (strcmp(func_name, "len") == 0) {
        if (arg_count != 1) {
          return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                           "Function 'len' expects 1 argument, got %d",
                           arg_count);
        }
        KronosValue *arg = pop(vm);
        if (!arg) {
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        if (arg->type == VAL_LIST) {
          KronosValue *result = value_new_number((double)arg->as.list.count);
          push(vm, result);
          value_release(result);
        } else if (arg->type == VAL_STRING) {
          KronosValue *result = value_new_number((double)arg->as.string.length);
          push(vm, result);
          value_release(result);
        } else {
          int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                              "Function 'len' requires a list or string argument");
          value_release(arg);
          return err;
        }
        value_release(arg);
        break;
      }

      // Built-in: uppercase(string)
      if (strcmp(func_name, "uppercase") == 0) {
        if (arg_count != 1) {
          return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                           "Function 'uppercase' expects 1 argument, got %d",
                           arg_count);
        }
        KronosValue *arg = pop(vm);
        if (!arg) {
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        if (arg->type != VAL_STRING) {
          int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                              "Function 'uppercase' requires a string argument");
          value_release(arg);
          return err;
        }

        char *upper = malloc(arg->as.string.length + 1);
        if (!upper) {
          value_release(arg);
          return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate memory");
        }
        for (size_t i = 0; i < arg->as.string.length; i++) {
          upper[i] = (char)toupper((unsigned char)arg->as.string.data[i]);
        }
        upper[arg->as.string.length] = '\0';

        KronosValue *result = value_new_string(upper, arg->as.string.length);
        free(upper);
        if (!result) {
          value_release(arg);
          return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string value");
        }
        push(vm, result);
        value_release(result);
        value_release(arg);
        break;
      }

      // Built-in: lowercase(string)
      if (strcmp(func_name, "lowercase") == 0) {
        if (arg_count != 1) {
          return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                           "Function 'lowercase' expects 1 argument, got %d",
                           arg_count);
        }
        KronosValue *arg = pop(vm);
        if (!arg) {
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        if (arg->type != VAL_STRING) {
          int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                              "Function 'lowercase' requires a string argument");
          value_release(arg);
          return err;
        }

        char *lower = malloc(arg->as.string.length + 1);
        if (!lower) {
          value_release(arg);
          return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate memory");
        }
        for (size_t i = 0; i < arg->as.string.length; i++) {
          lower[i] = (char)tolower((unsigned char)arg->as.string.data[i]);
        }
        lower[arg->as.string.length] = '\0';

        KronosValue *result = value_new_string(lower, arg->as.string.length);
        free(lower);
        if (!result) {
          value_release(arg);
          return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string value");
        }
        push(vm, result);
        value_release(result);
        value_release(arg);
        break;
      }

      // Built-in: trim(string)
      if (strcmp(func_name, "trim") == 0) {
        if (arg_count != 1) {
          return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                           "Function 'trim' expects 1 argument, got %d",
                           arg_count);
        }
        KronosValue *arg = pop(vm);
        if (!arg) {
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        if (arg->type != VAL_STRING) {
          int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                              "Function 'trim' requires a string argument");
          value_release(arg);
          return err;
        }

        // Find start (skip leading whitespace)
        size_t start = 0;
        while (start < arg->as.string.length && isspace((unsigned char)arg->as.string.data[start])) {
          start++;
        }

        // Find end (skip trailing whitespace)
        size_t end = arg->as.string.length;
        while (end > start && isspace((unsigned char)arg->as.string.data[end - 1])) {
          end--;
        }

        size_t trimmed_len = end - start;
        char *trimmed = malloc(trimmed_len + 1);
        if (!trimmed) {
          value_release(arg);
          return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate memory");
        }
        memcpy(trimmed, arg->as.string.data + start, trimmed_len);
        trimmed[trimmed_len] = '\0';

        KronosValue *result = value_new_string(trimmed, trimmed_len);
        free(trimmed);
        if (!result) {
          value_release(arg);
          return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string value");
        }
        push(vm, result);
        value_release(result);
        value_release(arg);
        break;
      }

      // Built-in: split(string, delimiter)
      if (strcmp(func_name, "split") == 0) {
        if (arg_count != 2) {
          return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                           "Function 'split' expects 2 arguments, got %d",
                           arg_count);
        }
        KronosValue *delim = pop(vm);
        if (!delim) {
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        KronosValue *str = pop(vm);
        if (!str) {
          value_release(delim);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        if (str->type != VAL_STRING || delim->type != VAL_STRING) {
          int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                              "Function 'split' requires two string arguments");
          value_release(str);
          value_release(delim);
          return err;
        }

        // Create result list
        KronosValue *result = value_new_list(4);
        if (!result) {
          value_release(str);
          value_release(delim);
          return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create list");
        }

        // Split string by delimiter
        const char *str_data = str->as.string.data;
        const char *delim_data = delim->as.string.data;
        size_t str_len = str->as.string.length;
        size_t delim_len = delim->as.string.length;

        if (delim_len == 0) {
          // Empty delimiter: split into individual characters
          for (size_t i = 0; i < str_len; i++) {
            char ch_str[2] = {str_data[i], '\0'};
            KronosValue *ch_val = value_new_string(ch_str, 1);
            if (!ch_val) {
              value_release(result);
              value_release(str);
              value_release(delim);
              return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string value");
            }

            // Grow list if needed
            if (result->as.list.count >= result->as.list.capacity) {
              size_t new_cap = result->as.list.capacity * 2;
              KronosValue **new_items = realloc(result->as.list.items, sizeof(KronosValue *) * new_cap);
              if (!new_items) {
                value_release(ch_val);
                value_release(result);
                value_release(str);
                value_release(delim);
                return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to grow list");
              }
              result->as.list.items = new_items;
              result->as.list.capacity = new_cap;
            }

            value_retain(ch_val);
            result->as.list.items[result->as.list.count++] = ch_val;
            value_release(ch_val);
          }
        } else {
          // Split by delimiter
          size_t start = 0;
          while (start < str_len) {
            // Find next delimiter
            size_t pos = start;
            bool found = false;
            while (pos + delim_len <= str_len) {
              if (memcmp(str_data + pos, delim_data, delim_len) == 0) {
                found = true;
                break;
              }
              pos++;
            }

            size_t end = found ? pos : str_len;
            size_t part_len = end - start;

            // Create substring
            char *part = malloc(part_len + 1);
            if (!part) {
              value_release(result);
              value_release(str);
              value_release(delim);
              return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate memory");
            }
            memcpy(part, str_data + start, part_len);
            part[part_len] = '\0';

            KronosValue *part_val = value_new_string(part, part_len);
            free(part);
            if (!part_val) {
              value_release(result);
              value_release(str);
              value_release(delim);
              return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string value");
            }

            // Grow list if needed
            if (result->as.list.count >= result->as.list.capacity) {
              size_t new_cap = result->as.list.capacity * 2;
              KronosValue **new_items = realloc(result->as.list.items, sizeof(KronosValue *) * new_cap);
              if (!new_items) {
                value_release(part_val);
                value_release(result);
                value_release(str);
                value_release(delim);
                return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to grow list");
              }
              result->as.list.items = new_items;
              result->as.list.capacity = new_cap;
            }

            value_retain(part_val);
            result->as.list.items[result->as.list.count++] = part_val;
            value_release(part_val);

            start = found ? pos + delim_len : str_len;
          }
        }

        push(vm, result);
        value_release(result);
        value_release(str);
        value_release(delim);
        break;
      }

      // Built-in: join(list, delimiter)
      if (strcmp(func_name, "join") == 0) {
        if (arg_count != 2) {
          return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                           "Function 'join' expects 2 arguments, got %d",
                           arg_count);
        }
        KronosValue *delim = pop(vm);
        if (!delim) {
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        KronosValue *list = pop(vm);
        if (!list) {
          value_release(delim);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        if (list->type != VAL_LIST || delim->type != VAL_STRING) {
          int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                              "Function 'join' requires a list and a string delimiter");
          value_release(list);
          value_release(delim);
          return err;
        }

        // Calculate total length
        size_t total_len = 0;
        for (size_t i = 0; i < list->as.list.count; i++) {
          KronosValue *item = list->as.list.items[i];
          if (item->type != VAL_STRING) {
            value_release(list);
            value_release(delim);
            return vm_error(vm, KRONOS_ERR_RUNTIME, "All list items must be strings for join");
          }
          total_len += item->as.string.length;
          if (i > 0) {
            total_len += delim->as.string.length;
          }
        }

        // Build joined string
        char *joined = malloc(total_len + 1);
        if (!joined) {
          value_release(list);
          value_release(delim);
          return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate memory");
        }

        size_t offset = 0;
        for (size_t i = 0; i < list->as.list.count; i++) {
          if (i > 0) {
            memcpy(joined + offset, delim->as.string.data, delim->as.string.length);
            offset += delim->as.string.length;
          }
          KronosValue *item = list->as.list.items[i];
          memcpy(joined + offset, item->as.string.data, item->as.string.length);
          offset += item->as.string.length;
        }
        joined[total_len] = '\0';

        KronosValue *result = value_new_string(joined, total_len);
        free(joined);
        if (!result) {
          value_release(list);
          value_release(delim);
          return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string value");
        }
        push(vm, result);
        value_release(result);
        value_release(list);
        value_release(delim);
        break;
      }

      // Built-in: to_string(value)
      if (strcmp(func_name, "to_string") == 0) {
        if (arg_count != 1) {
          return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                           "Function 'to_string' expects 1 argument, got %d",
                           arg_count);
        }
        KronosValue *arg = pop(vm);
        if (!arg) {
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }

        char *str_buf = NULL;
        size_t str_len = 0;

        if (arg->type == VAL_STRING) {
          // Already a string, just return it
          value_retain(arg);
          push(vm, arg);
          value_release(arg);
          break;
        } else if (arg->type == VAL_NUMBER) {
          // Convert number to string
          // Use a reasonable buffer size
          str_buf = malloc(64);
          if (!str_buf) {
            value_release(arg);
            return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate memory");
          }

          // Check if it's a whole number
          double intpart;
          double frac = modf(arg->as.number, &intpart);
          if (frac == 0.0) {
            str_len = (size_t)snprintf(str_buf, 64, "%.0f", arg->as.number);
          } else {
            str_len = (size_t)snprintf(str_buf, 64, "%g", arg->as.number);
          }
        } else if (arg->type == VAL_BOOL) {
          if (arg->as.boolean) {
            str_buf = strdup("true");
            str_len = 4;
          } else {
            str_buf = strdup("false");
            str_len = 5;
          }
          if (!str_buf) {
            value_release(arg);
            return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate memory");
          }
        } else if (arg->type == VAL_NIL) {
          str_buf = strdup("null");
          str_len = 4;
          if (!str_buf) {
            value_release(arg);
            return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate memory");
          }
        } else {
          value_release(arg);
          return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                          "Cannot convert type to string");
        }

        KronosValue *result = value_new_string(str_buf, str_len);
        free(str_buf); // Always free our buffer (value_new_string copies it)
        if (!result) {
          value_release(arg);
          return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string value");
        }
        push(vm, result);
        value_release(result);
        value_release(arg);
        break;
      }

      // Built-in: contains(string, substring)
      if (strcmp(func_name, "contains") == 0) {
        if (arg_count != 2) {
          return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                           "Function 'contains' expects 2 arguments, got %d",
                           arg_count);
        }
        KronosValue *substring = pop(vm);
        if (!substring) {
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        KronosValue *str = pop(vm);
        if (!str) {
          value_release(substring);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        if (str->type != VAL_STRING || substring->type != VAL_STRING) {
          int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                              "Function 'contains' requires two string arguments");
          value_release(str);
          value_release(substring);
          return err;
        }

        // Use strstr to check if substring exists
        bool found = (strstr(str->as.string.data, substring->as.string.data) != NULL);
        KronosValue *result = value_new_bool(found);
        push(vm, result);
        value_release(result);
        value_release(str);
        value_release(substring);
        break;
      }

      // Built-in: starts_with(string, prefix)
      if (strcmp(func_name, "starts_with") == 0) {
        if (arg_count != 2) {
          return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                           "Function 'starts_with' expects 2 arguments, got %d",
                           arg_count);
        }
        KronosValue *prefix = pop(vm);
        if (!prefix) {
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        KronosValue *str = pop(vm);
        if (!str) {
          value_release(prefix);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        if (str->type != VAL_STRING || prefix->type != VAL_STRING) {
          int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                              "Function 'starts_with' requires two string arguments");
          value_release(str);
          value_release(prefix);
          return err;
        }

        bool starts = false;
        if (prefix->as.string.length <= str->as.string.length) {
          starts = (memcmp(str->as.string.data, prefix->as.string.data, prefix->as.string.length) == 0);
        }
        KronosValue *result = value_new_bool(starts);
        push(vm, result);
        value_release(result);
        value_release(str);
        value_release(prefix);
        break;
      }

      // Built-in: ends_with(string, suffix)
      if (strcmp(func_name, "ends_with") == 0) {
        if (arg_count != 2) {
          return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                           "Function 'ends_with' expects 2 arguments, got %d",
                           arg_count);
        }
        KronosValue *suffix = pop(vm);
        if (!suffix) {
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        KronosValue *str = pop(vm);
        if (!str) {
          value_release(suffix);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        if (str->type != VAL_STRING || suffix->type != VAL_STRING) {
          int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                              "Function 'ends_with' requires two string arguments");
          value_release(str);
          value_release(suffix);
          return err;
        }

        bool ends = false;
        if (suffix->as.string.length <= str->as.string.length) {
          size_t start_pos = str->as.string.length - suffix->as.string.length;
          ends = (memcmp(str->as.string.data + start_pos, suffix->as.string.data, suffix->as.string.length) == 0);
        }
        KronosValue *result = value_new_bool(ends);
        push(vm, result);
        value_release(result);
        value_release(str);
        value_release(suffix);
        break;
      }

      // Built-in: replace(string, old, new)
      if (strcmp(func_name, "replace") == 0) {
        if (arg_count != 3) {
          return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                           "Function 'replace' expects 3 arguments, got %d",
                           arg_count);
        }
        KronosValue *new_str = pop(vm);
        if (!new_str) {
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        KronosValue *old_str = pop(vm);
        if (!old_str) {
          value_release(new_str);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        KronosValue *str = pop(vm);
        if (!str) {
          value_release(old_str);
          value_release(new_str);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        if (str->type != VAL_STRING || old_str->type != VAL_STRING || new_str->type != VAL_STRING) {
          int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                              "Function 'replace' requires three string arguments");
          value_release(str);
          value_release(old_str);
          value_release(new_str);
          return err;
        }

        // Handle empty old string (return original string)
        if (old_str->as.string.length == 0) {
          value_retain(str);
          push(vm, str);
          value_release(str);
          value_release(old_str);
          value_release(new_str);
          break;
        }

        // Calculate maximum possible result size
        size_t max_result_len = str->as.string.length;
        if (new_str->as.string.length > old_str->as.string.length) {
          // Worst case: every character is replaced
          max_result_len = (str->as.string.length / old_str->as.string.length) * new_str->as.string.length +
                          (str->as.string.length % old_str->as.string.length);
        }

        char *result_buf = malloc(max_result_len + 1);
        if (!result_buf) {
          value_release(str);
          value_release(old_str);
          value_release(new_str);
          return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate memory");
        }

        size_t result_len = 0;
        const char *search_start = str->as.string.data;
        const char *search_end = str->as.string.data + str->as.string.length;

        while (search_start < search_end) {
          const char *found = strstr(search_start, old_str->as.string.data);
          if (!found || found >= search_end) {
            // No more occurrences, copy rest of string
            size_t remaining = search_end - search_start;
            memcpy(result_buf + result_len, search_start, remaining);
            result_len += remaining;
            break;
          }

          // Copy part before match
          size_t before_len = found - search_start;
          memcpy(result_buf + result_len, search_start, before_len);
          result_len += before_len;

          // Copy replacement
          memcpy(result_buf + result_len, new_str->as.string.data, new_str->as.string.length);
          result_len += new_str->as.string.length;

          // Move past the old substring
          search_start = found + old_str->as.string.length;
        }

        result_buf[result_len] = '\0';

        KronosValue *result = value_new_string(result_buf, result_len);
        free(result_buf);
        if (!result) {
          value_release(str);
          value_release(old_str);
          value_release(new_str);
          return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string value");
        }
        push(vm, result);
        value_release(result);
        value_release(str);
        value_release(old_str);
        value_release(new_str);
        break;
      }

      // Get user-defined function
      Function *func = vm_get_function(vm, func_name);
      if (!func) {
        return vm_errorf(vm, KRONOS_ERR_NOT_FOUND, "Undefined function '%s'",
                         func_name);
      }

      if (arg_count != func->param_count) {
        return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                         "Function '%s' expects %zu argument%s, but got %d",
                         func->name, func->param_count,
                         func->param_count == 1 ? "" : "s", arg_count);
      }

      // Check call stack size
      if (vm->call_stack_size >= CALL_STACK_MAX) {
        return vm_error(vm, KRONOS_ERR_RUNTIME, "Maximum call depth exceeded");
      }

      // Create new call frame
      CallFrame *frame = &vm->call_stack[vm->call_stack_size++];
      frame->function = func;
      frame->return_ip = vm->ip;
      frame->return_bytecode = vm->bytecode;
      frame->frame_start = vm->stack_top;
      frame->local_count = 0;

      // Pop arguments and bind to parameters (in reverse order)
      KronosValue **args =
          arg_count > 0 ? malloc(sizeof(KronosValue *) * arg_count) : NULL;
      if (arg_count > 0 && !args) {
        // Allocation failure: restore VM state and abort call setup
        // Decrement call stack size to undo the increment above
        vm->call_stack_size--;
        return vm_error(vm, KRONOS_ERR_INTERNAL,
                        "Failed to allocate argument buffer");
      }
      for (int i = arg_count - 1; i >= 0; i--) {
        args[i] = pop(vm);
        if (!args[i]) {
          // Free already-popped arguments
          for (int j = i + 1; j < arg_count; j++) {
            value_release(args[j]);
          }
          free(args);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
      }

      // Set current frame before setting locals
      vm->current_frame = frame;

      // Set parameters as local variables in the new frame
      // Parameters are mutable by default
      for (size_t i = 0; i < arg_count; i++) {
        int arg_status =
            vm_set_local(vm, frame, func->params[i], args[i], true, NULL);
        value_release(args[i]);
        if (arg_status != 0) {
          for (size_t j = i + 1; j < arg_count; j++) {
            value_release(args[j]);
          }
          free(args);

          for (size_t j = 0; j < frame->local_count; j++) {
            free(frame->locals[j].name);
            value_release(frame->locals[j].value);
            free(frame->locals[j].type_name);
          }
          frame->local_count = 0;

          vm->call_stack_size--;
          if (vm->call_stack_size > 0) {
            vm->current_frame = &vm->call_stack[vm->call_stack_size - 1];
          } else {
            vm->current_frame = NULL;
          }
          return arg_status;
        }
      }
      free(args);

      // Switch to function bytecode
      vm->bytecode = &func->bytecode;
      vm->ip = func->bytecode.code;

      break;
    }

    case OP_RETURN_VAL: {
      // Pop return value from stack
      KronosValue *return_value = pop(vm);
      if (!return_value) {
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }

      // If we're in a function call, return from it
      if (vm->call_stack_size > 0) {
        CallFrame *frame = &vm->call_stack[vm->call_stack_size - 1];

        // Clean up local variables
        for (size_t i = 0; i < frame->local_count; i++) {
          free(frame->locals[i].name);
          value_release(frame->locals[i].value);
          free(frame->locals[i].type_name);
        }

        // Restore VM state
        vm->ip = frame->return_ip;
        vm->bytecode = frame->return_bytecode;
        vm->call_stack_size--;

        // Update current frame pointer
        if (vm->call_stack_size > 0) {
          vm->current_frame = &vm->call_stack[vm->call_stack_size - 1];
        } else {
          vm->current_frame = NULL;
        }

        // Push return value onto stack
        push(vm, return_value);
        value_release(return_value);
      } else {
        // Top-level return (shouldn't happen in normal code)
        push(vm, return_value);
        value_release(return_value);
      }

      break;
    }

    case OP_POP: {
      KronosValue *value = pop(vm);
      if (!value) {
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
      value_release(value);
      break;
    }

    case OP_LIST_NEW: {
      // Read element count from bytecode
      uint16_t count = (uint16_t)(read_byte(vm) << 8 | read_byte(vm));
      KronosValue *list = value_new_list(count);
      if (!list) {
        return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create list");
      }
      push(vm, list);
      value_release(list);
      break;
    }

    case OP_LIST_APPEND: {
      KronosValue *value = pop(vm);
      if (!value) {
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
      KronosValue *list = pop(vm);
      if (!list) {
        value_release(value);
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }

      if (list->type != VAL_LIST) {
        value_release(value);
        value_release(list);
        return vm_error(vm, KRONOS_ERR_RUNTIME, "Expected list for append");
      }

      // Grow list if needed
      if (list->as.list.count >= list->as.list.capacity) {
        size_t new_capacity =
            list->as.list.capacity == 0 ? 4 : list->as.list.capacity * 2;
        KronosValue **new_items =
            realloc(list->as.list.items, sizeof(KronosValue *) * new_capacity);
        if (!new_items) {
          value_release(value);
          value_release(list);
          return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to grow list");
        }
        list->as.list.items = new_items;
        list->as.list.capacity = new_capacity;
      }

      // Append value
      value_retain(value);
      list->as.list.items[list->as.list.count++] = value;
      push(vm, list);
      value_release(list);
      value_release(value);
      break;
    }

    case OP_LIST_GET: {
      KronosValue *index_val = pop(vm);
      if (!index_val) {
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
      KronosValue *container = pop(vm);
      if (!container) {
        value_release(index_val);
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }

      if (index_val->type != VAL_NUMBER) {
        value_release(index_val);
        value_release(container);
        return vm_error(vm, KRONOS_ERR_RUNTIME, "Index must be a number");
      }

      // Handle negative indices
      int64_t idx = (int64_t)index_val->as.number;

      if (container->type == VAL_LIST) {
        if (idx < 0) {
          idx = (int64_t)container->as.list.count + idx;
        }

        if (idx < 0 || (size_t)idx >= container->as.list.count) {
          value_release(index_val);
          value_release(container);
          return vm_error(vm, KRONOS_ERR_RUNTIME, "List index out of bounds");
        }

        KronosValue *item = container->as.list.items[(size_t)idx];
        value_retain(item);
        push(vm, item);
        value_release(item);
      } else if (container->type == VAL_STRING) {
        // String indexing
        if (idx < 0) {
          idx = (int64_t)container->as.string.length + idx;
        }

        if (idx < 0 || (size_t)idx >= container->as.string.length) {
          value_release(index_val);
          value_release(container);
          return vm_error(vm, KRONOS_ERR_RUNTIME, "String index out of bounds");
        }

        // Create a single-character string
        char ch = container->as.string.data[(size_t)idx];
        char str[2] = {ch, '\0'};
        KronosValue *char_str = value_new_string(str, 1);
        if (!char_str) {
          value_release(index_val);
          value_release(container);
          return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string value");
        }
        push(vm, char_str);
        value_release(char_str);
      } else {
        value_release(index_val);
        value_release(container);
        return vm_error(vm, KRONOS_ERR_RUNTIME, "Indexing only supported for lists and strings");
      }

      value_release(index_val);
      value_release(container);
      break;
    }

    case OP_LIST_LEN: {
      KronosValue *container = pop(vm);
      if (!container) {
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }

      if (container->type == VAL_LIST) {
        KronosValue *len = value_new_number((double)container->as.list.count);
        push(vm, len);
        value_release(len);
      } else if (container->type == VAL_STRING) {
        KronosValue *len = value_new_number((double)container->as.string.length);
        push(vm, len);
        value_release(len);
      } else {
        value_release(container);
        return vm_error(vm, KRONOS_ERR_RUNTIME, "Expected list or string for length");
      }

      value_release(container);
      break;
    }

    case OP_LIST_SLICE: {
      KronosValue *end_val = pop(vm);
      if (!end_val) {
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
      KronosValue *start_val = pop(vm);
      if (!start_val) {
        value_release(end_val);
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
      KronosValue *container = pop(vm);
      if (!container) {
        value_release(start_val);
        value_release(end_val);
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }

      if (start_val->type != VAL_NUMBER || end_val->type != VAL_NUMBER) {
        value_release(container);
        value_release(start_val);
        value_release(end_val);
        return vm_error(vm, KRONOS_ERR_RUNTIME, "Slice indices must be numbers");
      }

      int64_t start = (int64_t)start_val->as.number;
      int64_t end = (int64_t)end_val->as.number;

      if (container->type == VAL_LIST) {
        size_t len = container->as.list.count;

        // Handle negative indices
        if (start < 0) {
          start = (int64_t)len + start;
        }
        if (end < 0) {
          if (end == -1) {
            // Special marker for "to end"
            end = (int64_t)len;
          } else {
            end = (int64_t)len + end;
          }
        }

        // Clamp to valid range
        if (start < 0) start = 0;
        if (end < 0) end = 0;
        if ((size_t)start > len) start = (int64_t)len;
        if ((size_t)end > len) end = (int64_t)len;
        if (start > end) start = end;

        // Create new list with sliced elements
        size_t slice_len = (size_t)(end - start);
        KronosValue *slice = value_new_list(slice_len);
        if (!slice) {
          value_release(container);
          value_release(start_val);
          value_release(end_val);
          return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create list");
        }

        for (size_t i = 0; i < slice_len; i++) {
          KronosValue *item = container->as.list.items[(size_t)start + i];
          value_retain(item);
          slice->as.list.items[slice->as.list.count++] = item;
        }

        push(vm, slice);
        value_release(slice);
      } else if (container->type == VAL_STRING) {
        size_t len = container->as.string.length;

        // Handle negative indices
        if (start < 0) {
          start = (int64_t)len + start;
        }
        if (end < 0) {
          if (end == -1) {
            // Special marker for "to end"
            end = (int64_t)len;
          } else {
            end = (int64_t)len + end;
          }
        }

        // Clamp to valid range
        if (start < 0) start = 0;
        if (end < 0) end = 0;
        if ((size_t)start > len) start = (int64_t)len;
        if ((size_t)end > len) end = (int64_t)len;
        if (start > end) start = end;

        // Create new string with sliced characters
        size_t slice_len = (size_t)(end - start);
        char *slice_data = malloc(slice_len + 1);
        if (!slice_data) {
          value_release(container);
          value_release(start_val);
          value_release(end_val);
          return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate memory for string slice");
        }

        memcpy(slice_data, container->as.string.data + start, slice_len);
        slice_data[slice_len] = '\0';

        KronosValue *slice = value_new_string(slice_data, slice_len);
        free(slice_data);
        if (!slice) {
          value_release(container);
          value_release(start_val);
          value_release(end_val);
          return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string value");
        }

        push(vm, slice);
        value_release(slice);
      } else {
        value_release(container);
        value_release(start_val);
        value_release(end_val);
        return vm_error(vm, KRONOS_ERR_RUNTIME, "Slicing only supported for lists and strings");
      }

      value_release(container);
      value_release(start_val);
      value_release(end_val);
      break;
    }

    case OP_LIST_ITER: {
      KronosValue *list = pop(vm);
      if (!list) {
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }

      if (list->type != VAL_LIST) {
        value_release(list);
        return vm_error(vm, KRONOS_ERR_RUNTIME, "Expected list for iteration");
      }

      // Create iterator (just push the list and current index)
      // For simplicity, we'll use a list value with a special marker
      // Actually, we need to track iteration state. Let's use a simple
      // approach: Push list, then push index 0
      value_retain(list);
      push(vm, list);
      KronosValue *index = value_new_number(0);
      push(vm, index);
      value_release(index);
      value_release(list);
      break;
    }

    case OP_LIST_NEXT: {
      // Stack: [list, index] (list on bottom, index on top)
      // Verify stack has at least 2 items before popping
      size_t stack_depth = (size_t)(vm->stack_top - vm->stack);
      if (stack_depth < 2) {
        return vm_errorf(
            vm, KRONOS_ERR_RUNTIME,
            "Stack underflow in list iteration: expected 2 items, got %zu. "
            "This usually means iterator variables were not loaded correctly.",
            stack_depth);
      }
      KronosValue *index_val = pop(vm);
      if (!index_val) {
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
      KronosValue *list = pop(vm);
      if (!list) {
        value_release(index_val);
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }

      if (list->type != VAL_LIST || index_val->type != VAL_NUMBER) {
        value_release(index_val);
        value_release(list);
        return vm_error(vm, KRONOS_ERR_RUNTIME, "Invalid iterator state");
      }

      size_t idx = (size_t)index_val->as.number;
      bool has_more = idx < list->as.list.count;

      if (has_more) {
        // Push in order: [list, index+1, item, has_more]
        // This way, after popping has_more, item is on top for OP_STORE_VAR
        // After storing item, we have [list, index+1] for next iteration

        // Push in order: [list, index+1, item, has_more]
        // This way, after popping has_more, item is on top for OP_STORE_VAR
        // After storing item, we have [list, index+1] for next iteration

        // Push list first (bottom of stack)
        value_retain(list);
        push(vm, list);
        value_release(list);

        // Update and push index
        KronosValue *next_index = value_new_number((double)(idx + 1));
        push(vm, next_index);
        value_release(next_index);

        // Push item
        KronosValue *item = list->as.list.items[idx];
        value_retain(item);
        push(vm, item);
        value_release(item);

        // Push has_more flag (true) on top
        KronosValue *has_more_val = value_new_bool(true);
        push(vm, has_more_val);
        value_release(has_more_val);
      } else {
        // No more items - push iterator state back, then has_more flag (false)
        // Push list back (for cleanup)
        value_retain(list);
        push(vm, list);
        value_release(list);

        // Push index back (for cleanup)
        value_retain(index_val);
        push(vm, index_val);
        value_release(index_val);

        // Push has_more flag (false) on top
        KronosValue *has_more_val = value_new_bool(false);
        push(vm, has_more_val);
        value_release(has_more_val);
      }

      value_release(index_val);
      value_release(list);
      break;
    }

    case OP_HALT: {
      return 0;
    }

    default:
      return vm_errorf(
          vm, KRONOS_ERR_INTERNAL,
          "Unknown bytecode instruction: %d (this is a compiler bug)",
          instruction);
    }
  }

  return 0;
}
