#include "vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

  // Initialize Pi constant (100 decimal places) - immutable
  KronosValue *pi_value = value_new_number(
      3.1415926535897932384626433832795028841971693993751058209749445923078164062862089986280348253421170679);

  // Manually add Pi as immutable global
  if (vm->global_count < GLOBALS_MAX) {
    vm->globals[vm->global_count].name = strdup("Pi");
    vm->globals[vm->global_count].value = pi_value;
    vm->globals[vm->global_count].is_mutable = false; // Immutable!
    vm->globals[vm->global_count].type_name = strdup("number");
    value_retain(pi_value);
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
void vm_define_function(KronosVM *vm, Function *func) {
  if (vm->function_count >= FUNCTIONS_MAX) {
    fprintf(
        stderr,
        "Error: Maximum number of functions exceeded (%d functions allowed)\n",
        FUNCTIONS_MAX);
    return;
  }

  vm->functions[vm->function_count++] = func;
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
    fprintf(stderr, "Error: Stack overflow (too many nested operations or "
                    "function calls)\n");
    return;
  }
  *vm->stack_top = value;
  vm->stack_top++;
  value_retain(value); // Retain while on stack
}

static KronosValue *pop(KronosVM *vm) {
  if (vm->stack_top <= vm->stack) {
    fprintf(stderr, "Error: Internal stack error (this shouldn't happen - "
                    "please report this bug)\n");
    return value_new_nil();
  }
  vm->stack_top--;
  KronosValue *val = *vm->stack_top;
  return val;
}

static KronosValue *peek(KronosVM *vm, int distance) {
  return vm->stack_top[-1 - distance];
}

// Global variable management
void vm_set_global(KronosVM *vm, const char *name, KronosValue *value,
                   bool is_mutable, const char *type_name) {
  // Check if variable already exists
  for (size_t i = 0; i < vm->global_count; i++) {
    if (strcmp(vm->globals[i].name, name) == 0) {
      // Check if it's immutable
      if (!vm->globals[i].is_mutable) {
        fprintf(stderr, "Error: Cannot reassign immutable variable '%s'\n",
                name);
        exit(1);
      }

      // Check type if specified
      if (vm->globals[i].type_name != NULL &&
          !value_is_type(value, vm->globals[i].type_name)) {
        fprintf(stderr,
                "Error: Type mismatch for variable '%s': expected '%s'\n", name,
                vm->globals[i].type_name);
        exit(1);
      }

      value_release(vm->globals[i].value);
      vm->globals[i].value = value;
      value_retain(value);
      return;
    }
  }

  // Add new global
  if (vm->global_count >= GLOBALS_MAX) {
    fprintf(stderr,
            "Error: Maximum number of global variables exceeded (%d allowed)\n",
            GLOBALS_MAX);
    return;
  }

  vm->globals[vm->global_count].name = strdup(name);
  vm->globals[vm->global_count].value = value;
  vm->globals[vm->global_count].is_mutable = is_mutable;
  vm->globals[vm->global_count].type_name =
      type_name ? strdup(type_name) : NULL;
  value_retain(value);
  vm->global_count++;
}

KronosValue *vm_get_global(KronosVM *vm, const char *name) {
  for (size_t i = 0; i < vm->global_count; i++) {
    if (strcmp(vm->globals[i].name, name) == 0) {
      return vm->globals[i].value;
    }
  }

  fprintf(stderr, "Error: Undefined variable '%s'\n", name);
  exit(1);
}

// Set local variable in current frame
void vm_set_local(CallFrame *frame, const char *name, KronosValue *value,
                  bool is_mutable, const char *type_name) {
  if (!frame)
    return;

  // Check if variable already exists
  for (size_t i = 0; i < frame->local_count; i++) {
    if (strcmp(frame->locals[i].name, name) == 0) {
      // Check if it's immutable
      if (!frame->locals[i].is_mutable) {
        fprintf(stderr,
                "Error: Cannot reassign immutable local variable '%s'\n", name);
        exit(1);
      }

      // Check type if specified
      if (frame->locals[i].type_name != NULL &&
          !value_is_type(value, frame->locals[i].type_name)) {
        fprintf(stderr,
                "Error: Type mismatch for local variable '%s': expected '%s'\n",
                name, frame->locals[i].type_name);
        exit(1);
      }

      value_release(frame->locals[i].value);
      frame->locals[i].value = value;
      value_retain(value);
      return;
    }
  }

  // Add new local variable
  if (frame->local_count >= LOCALS_MAX) {
    fprintf(stderr,
            "Error: Maximum number of local variables exceeded in function (%d "
            "allowed)\n",
            LOCALS_MAX);
    return;
  }

  frame->locals[frame->local_count].name = strdup(name);
  frame->locals[frame->local_count].value = value;
  frame->locals[frame->local_count].is_mutable = is_mutable;
  frame->locals[frame->local_count].type_name =
      type_name ? strdup(type_name) : NULL;
  value_retain(value);
  frame->local_count++;
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
  return vm_get_global(vm, name);
}

// Read byte from bytecode
static uint8_t read_byte(KronosVM *vm) { return *vm->ip++; }

// Read constant from pool
static KronosValue *read_constant(KronosVM *vm) {
  uint8_t idx = read_byte(vm);
  return vm->bytecode->constants[idx];
}

// Execute bytecode
int vm_execute(KronosVM *vm, Bytecode *bytecode) {
  if (!vm || !bytecode)
    return -1;

  vm->bytecode = bytecode;
  vm->ip = bytecode->code;

  while (1) {
    uint8_t instruction = read_byte(vm);

    switch (instruction) {
    case OP_LOAD_CONST: {
      KronosValue *constant = read_constant(vm);
      push(vm, constant);
      break;
    }

    case OP_LOAD_VAR: {
      KronosValue *name_val = read_constant(vm);
      if (name_val->type != VAL_STRING) {
        fprintf(stderr,
                "Error: Internal error - variable name is not a string\n");
        return -1;
      }
      KronosValue *value = vm_get_variable(vm, name_val->as.string.data);
      push(vm, value);
      break;
    }

    case OP_STORE_VAR: {
      KronosValue *name_val = read_constant(vm);
      if (name_val->type != VAL_STRING) {
        fprintf(stderr,
                "Error: Internal error - variable name is not a string\n");
        return -1;
      }
      KronosValue *value = pop(vm);

      // Read mutability flag
      uint8_t is_mutable_byte = read_byte(vm);
      bool is_mutable = (is_mutable_byte == 1);

      // Read type name (if specified)
      uint8_t type_idx = read_byte(vm);
      const char *type_name = NULL;
      if (type_idx != 255) {
        KronosValue *type_val = vm->bytecode->constants[type_idx];
        if (type_val->type == VAL_STRING) {
          type_name = type_val->as.string.data;
        }
      }

      // If in function, set as local variable; otherwise, set as global
      if (vm->current_frame) {
        vm_set_local(vm->current_frame, name_val->as.string.data, value,
                     is_mutable, type_name);
      } else {
        vm_set_global(vm, name_val->as.string.data, value, is_mutable,
                      type_name);
      }

      value_release(value); // Release our reference
      break;
    }

    case OP_PRINT: {
      KronosValue *value = pop(vm);
      value_print(value);
      printf("\n");
      value_release(value);
      break;
    }

    case OP_ADD: {
      KronosValue *b = pop(vm);
      KronosValue *a = pop(vm);

      if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
        KronosValue *result = value_new_number(a->as.number + b->as.number);
        push(vm, result);
        value_release(result); // Push retains it
      } else {
        fprintf(stderr, "Error: Cannot add - both values must be numbers\n");
        value_release(a);
        value_release(b);
        return -1;
      }

      value_release(a);
      value_release(b);
      break;
    }

    case OP_SUB: {
      KronosValue *b = pop(vm);
      KronosValue *a = pop(vm);

      if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
        KronosValue *result = value_new_number(a->as.number - b->as.number);
        push(vm, result);
        value_release(result);
      } else {
        fprintf(stderr,
                "Error: Cannot subtract - both values must be numbers\n");
        value_release(a);
        value_release(b);
        return -1;
      }

      value_release(a);
      value_release(b);
      break;
    }

    case OP_MUL: {
      KronosValue *b = pop(vm);
      KronosValue *a = pop(vm);

      if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
        KronosValue *result = value_new_number(a->as.number * b->as.number);
        push(vm, result);
        value_release(result);
      } else {
        fprintf(stderr,
                "Error: Cannot multiply - both values must be numbers\n");
        value_release(a);
        value_release(b);
        return -1;
      }

      value_release(a);
      value_release(b);
      break;
    }

    case OP_DIV: {
      KronosValue *b = pop(vm);
      KronosValue *a = pop(vm);

      if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
        if (b->as.number == 0) {
          fprintf(stderr, "Error: Cannot divide by zero\n");
          value_release(a);
          value_release(b);
          return -1;
        }
        KronosValue *result = value_new_number(a->as.number / b->as.number);
        push(vm, result);
        value_release(result);
      } else {
        fprintf(stderr, "Error: Cannot divide - both values must be numbers\n");
        value_release(a);
        value_release(b);
        return -1;
      }

      value_release(a);
      value_release(b);
      break;
    }

    case OP_EQ: {
      KronosValue *b = pop(vm);
      KronosValue *a = pop(vm);
      bool result = value_equals(a, b);
      push(vm, value_new_bool(result));
      value_release(a);
      value_release(b);
      break;
    }

    case OP_NEQ: {
      KronosValue *b = pop(vm);
      KronosValue *a = pop(vm);
      bool result = !value_equals(a, b);
      push(vm, value_new_bool(result));
      value_release(a);
      value_release(b);
      break;
    }

    case OP_GT: {
      KronosValue *b = pop(vm);
      KronosValue *a = pop(vm);

      if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
        bool result = a->as.number > b->as.number;
        push(vm, value_new_bool(result));
      } else {
        fprintf(stderr,
                "Error: Cannot compare - both values must be numbers\n");
        value_release(a);
        value_release(b);
        return -1;
      }

      value_release(a);
      value_release(b);
      break;
    }

    case OP_LT: {
      KronosValue *b = pop(vm);
      KronosValue *a = pop(vm);

      if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
        bool result = a->as.number < b->as.number;
        push(vm, value_new_bool(result));
      } else {
        fprintf(stderr,
                "Error: Cannot compare - both values must be numbers\n");
        value_release(a);
        value_release(b);
        return -1;
      }

      value_release(a);
      value_release(b);
      break;
    }

    case OP_GTE: {
      KronosValue *b = pop(vm);
      KronosValue *a = pop(vm);

      if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
        bool result = a->as.number >= b->as.number;
        push(vm, value_new_bool(result));
      } else {
        fprintf(stderr,
                "Error: Cannot compare - both values must be numbers\n");
        value_release(a);
        value_release(b);
        return -1;
      }

      value_release(a);
      value_release(b);
      break;
    }

    case OP_LTE: {
      KronosValue *b = pop(vm);
      KronosValue *a = pop(vm);

      if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
        bool result = a->as.number <= b->as.number;
        push(vm, value_new_bool(result));
      } else {
        fprintf(stderr,
                "Error: Cannot compare - both values must be numbers\n");
        value_release(a);
        value_release(b);
        return -1;
      }

      value_release(a);
      value_release(b);
      break;
    }

    case OP_JUMP: {
      int8_t offset = (int8_t)read_byte(vm);
      vm->ip += offset;
      break;
    }

    case OP_JUMP_IF_FALSE: {
      uint8_t offset = read_byte(vm);
      KronosValue *condition = peek(vm, 0);
      if (!value_is_truthy(condition)) {
        vm->ip += offset;
      }
      value_release(pop(vm)); // Pop condition
      break;
    }

    case OP_DEFINE_FUNC: {
      // Read function name
      KronosValue *name_val = read_constant(vm);
      uint8_t param_count = read_byte(vm);

      // Create function
      Function *func = malloc(sizeof(Function));
      func->name = strdup(name_val->as.string.data);
      func->param_count = param_count;
      func->params = malloc(sizeof(char *) * param_count);

      // Read parameter names
      for (size_t i = 0; i < param_count; i++) {
        KronosValue *param_val = read_constant(vm);

        // Check if parameter name is a reserved constant
        if (strcmp(param_val->as.string.data, "Pi") == 0) {
          fprintf(stderr, "Error: Cannot use 'Pi' as a parameter name (it is a "
                          "reserved constant)\n");
          // Clean up and error
          for (size_t j = 0; j < i; j++) {
            free(func->params[j]);
          }
          free(func->params);
          free(func->name);
          free(func);
          return -1;
        }

        func->params[i] = strdup(param_val->as.string.data);
      }

      // Read function body start position
      uint8_t body_high = read_byte(vm);
      uint8_t body_low = read_byte(vm);
      size_t body_start = (body_high << 8) | body_low;

      // Skip the OP_JUMP instruction
      read_byte(vm);

      // Read jump offset (skip body)
      uint8_t skip_offset = read_byte(vm);

      // Calculate body end (before the jump we just read)
      uint8_t *body_end_ptr = vm->ip + skip_offset;

      // Copy function body bytecode
      size_t bytecode_size = body_end_ptr - vm->ip;
      func->bytecode.code = malloc(bytecode_size);
      func->bytecode.count = bytecode_size;
      func->bytecode.capacity = bytecode_size;
      memcpy(func->bytecode.code, vm->ip, bytecode_size);

      // Copy constants (retain references)
      func->bytecode.const_count = vm->bytecode->const_count;
      func->bytecode.const_capacity = vm->bytecode->const_count;
      func->bytecode.constants =
          malloc(sizeof(KronosValue *) * func->bytecode.const_count);
      for (size_t i = 0; i < func->bytecode.const_count; i++) {
        func->bytecode.constants[i] = vm->bytecode->constants[i];
        value_retain(func->bytecode.constants[i]);
      }

      // Store function
      vm_define_function(vm, func);

      // Skip over function body
      vm->ip = body_end_ptr;
      break;
    }

    case OP_CALL_FUNC: {
      KronosValue *name_val = read_constant(vm);
      uint8_t arg_count = read_byte(vm);

      // Check for built-in functions first
      const char *func_name = name_val->as.string.data;

      // Built-in: add(a, b)
      if (strcmp(func_name, "add") == 0) {
        if (arg_count != 2) {
          fprintf(stderr,
                  "Error: Function 'add' expects 2 arguments, but got %d\n",
                  arg_count);
          return -1;
        }
        KronosValue *b = pop(vm);
        KronosValue *a = pop(vm);
        if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
          KronosValue *result = value_new_number(a->as.number + b->as.number);
          push(vm, result);
          value_release(result);
        } else {
          fprintf(
              stderr,
              "Error: Function 'add' requires both arguments to be numbers\n");
          value_release(a);
          value_release(b);
          return -1;
        }
        value_release(a);
        value_release(b);
        break;
      }

      // Built-in: subtract(a, b)
      if (strcmp(func_name, "subtract") == 0) {
        if (arg_count != 2) {
          fprintf(
              stderr,
              "Error: Function 'subtract' expects 2 arguments, but got %d\n",
              arg_count);
          return -1;
        }
        KronosValue *b = pop(vm);
        KronosValue *a = pop(vm);
        if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
          KronosValue *result = value_new_number(a->as.number - b->as.number);
          push(vm, result);
          value_release(result);
        } else {
          fprintf(stderr, "Error: Function 'subtract' requires both arguments "
                          "to be numbers\n");
          value_release(a);
          value_release(b);
          return -1;
        }
        value_release(a);
        value_release(b);
        break;
      }

      // Built-in: multiply(a, b)
      if (strcmp(func_name, "multiply") == 0) {
        if (arg_count != 2) {
          fprintf(
              stderr,
              "Error: Function 'multiply' expects 2 arguments, but got %d\n",
              arg_count);
          return -1;
        }
        KronosValue *b = pop(vm);
        KronosValue *a = pop(vm);
        if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
          KronosValue *result = value_new_number(a->as.number * b->as.number);
          push(vm, result);
          value_release(result);
        } else {
          fprintf(stderr, "Error: Function 'multiply' requires both arguments "
                          "to be numbers\n");
          value_release(a);
          value_release(b);
          return -1;
        }
        value_release(a);
        value_release(b);
        break;
      }

      // Built-in: divide(a, b)
      if (strcmp(func_name, "divide") == 0) {
        if (arg_count != 2) {
          fprintf(stderr,
                  "Error: Function 'divide' expects 2 arguments, but got %d\n",
                  arg_count);
          return -1;
        }
        KronosValue *b = pop(vm);
        KronosValue *a = pop(vm);
        if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
          if (b->as.number == 0) {
            fprintf(stderr, "Error: Cannot divide by zero\n");
            value_release(a);
            value_release(b);
            return -1;
          } else {
            KronosValue *result = value_new_number(a->as.number / b->as.number);
            push(vm, result);
            value_release(result);
          }
        } else {
          fprintf(stderr, "Error: Function 'divide' requires both arguments to "
                          "be numbers\n");
          value_release(a);
          value_release(b);
          return -1;
        }
        value_release(a);
        value_release(b);
        break;
      }

      // Get user-defined function
      Function *func = vm_get_function(vm, func_name);
      if (!func) {
        fprintf(stderr, "Error: Undefined function '%s'\n", func_name);
        return -1;
      }

      if (arg_count != func->param_count) {
        fprintf(stderr,
                "Error: Function '%s' expects %zu argument%s, but got %d\n",
                func->name, func->param_count,
                func->param_count == 1 ? "" : "s", arg_count);
        return -1;
      }

      // Check call stack size
      if (vm->call_stack_size >= CALL_STACK_MAX) {
        fprintf(stderr, "Error: Maximum call depth exceeded (too many nested "
                        "function calls)\n");
        return -1;
      }

      // Create new call frame
      CallFrame *frame = &vm->call_stack[vm->call_stack_size++];
      frame->function = func;
      frame->return_ip = vm->ip;
      frame->return_bytecode = vm->bytecode;
      frame->frame_start = vm->stack_top;
      frame->local_count = 0;

      // Pop arguments and bind to parameters (in reverse order)
      KronosValue **args = malloc(sizeof(KronosValue *) * arg_count);
      for (int i = arg_count - 1; i >= 0; i--) {
        args[i] = pop(vm);
      }

      // Set current frame before setting locals
      vm->current_frame = frame;

      // Set parameters as local variables in the new frame
      // Parameters are mutable by default
      for (size_t i = 0; i < arg_count; i++) {
        vm_set_local(frame, func->params[i], args[i], true, NULL);
        value_release(args[i]);
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

      // If we're in a function call, return from it
      if (vm->call_stack_size > 0) {
        CallFrame *frame = &vm->call_stack[vm->call_stack_size - 1];

        // Clean up local variables
        for (size_t i = 0; i < frame->local_count; i++) {
          free(frame->locals[i].name);
          value_release(frame->locals[i].value);
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
      value_release(pop(vm));
      break;
    }

    case OP_HALT: {
      return 0;
    }

    default:
      fprintf(
          stderr,
          "Error: Unknown bytecode instruction: %d (this is a compiler bug)\n",
          instruction);
      return -1;
    }
  }

  return 0;
}
