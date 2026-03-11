/**
 * @file vm.c
 * @brief Virtual machine for executing Kronos bytecode
 *
 * DESIGN DECISIONS:
 * - Stack-based: Simpler than register-based, easier to implement, good for
 *   interpreted languages. Trade-off is more instructions for complex operations.
 * - Fixed-size stacks: STACK_MAX, CALL_STACK_MAX, etc. prevent unbounded growth
 *   but limit program complexity. Overflow is detected and reported as errors.
 * - Hash tables for variables: O(1) lookup for globals/locals/functions using
 *   linear probing. Faster than linear search for large programs.
 * - Module isolation: Each module has its own VM instance for namespace
 *   isolation (separate globals, functions).
 * - Exception handling: Stack-based exception handlers (try/catch/finally)
 *   stored in VM, allowing proper unwinding and cleanup.
 *
 * EDGE CASES:
 * - Stack overflow: Detected and reported as runtime error
 * - Call stack overflow: Detected before creating new frame
 * - Variable lookup: Locals checked first, then globals (lexical scoping)
 * - Module imports: Circular imports detected via loading_modules stack
 * - Exception handling: Handlers stored on stack, properly unwound on errors
 * - Break/continue: Implemented via OP_JUMP with patched offsets (no separate
 *   opcodes to keep instruction set small)
 *
 * Implements a stack-based virtual machine that executes compiled bytecode.
 * Features:
 * - Stack-based execution model
 * - Global and local variable management
 * - Function call stack with local scoping
 * - Built-in functions (math, string operations, list operations)
 * - Error handling and reporting
 * - Break/continue support in loops
 */

#define _POSIX_C_SOURCE 200809L
#include "vm.h"
#include "vm_builtins.h"
#include "vm_builtins_registry.h"
#include "../compiler/compiler.h"
#include "../frontend/parser.h"
#include "../frontend/tokenizer.h"
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <math.h>
#include <regex.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Constants for buffer sizes
#define NUMBER_STRING_BUFFER_SIZE                                              \
  64 // Buffer size for converting numbers to strings
#define REGEX_ERROR_BUFFER_SIZE 256 // Buffer size for regex error messages
#define PORTABLE_GETLINE_INITIAL_SIZE                                          \
  256 // Initial buffer size for portable getline

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#endif

/**
 * @brief Portable fopen() implementation with UTF-8 support
 *
 * Opens a file with UTF-8 path support on all platforms. On Windows, converts
 * UTF-8 to wide characters and uses _wfopen() for proper UTF-8 handling. On
 * other platforms, uses standard fopen().
 *
 * @param path UTF-8 encoded file path
 * @param mode File access mode (same as fopen)
 * @return FILE pointer on success, NULL on failure
 */
static FILE *portable_fopen(const char *path, const char *mode) {
#ifdef _WIN32
  // Convert UTF-8 to wide characters for Windows
  int path_len = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
  if (path_len <= 0) {
    return NULL;
  }
  wchar_t *wpath = malloc(path_len * sizeof(wchar_t));
  if (!wpath) {
    return NULL;
  }
  if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, path_len) <= 0) {
    free(wpath);
    return NULL;
  }

  // Convert mode string to wide characters
  int mode_len = MultiByteToWideChar(CP_UTF8, 0, mode, -1, NULL, 0);
  if (mode_len <= 0) {
    free(wpath);
    return NULL;
  }
  wchar_t *wmode = malloc(mode_len * sizeof(wchar_t));
  if (!wmode) {
    free(wpath);
    return NULL;
  }
  if (MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, mode_len) <= 0) {
    free(wpath);
    free(wmode);
    return NULL;
  }

  // Use _wfopen for UTF-8 support on Windows
  FILE *file = _wfopen(wpath, wmode);
  free(wpath);
  free(wmode);
  return file;
#else
  // On non-Windows platforms, use standard fopen
  return fopen(path, mode);
#endif
}

/**
 * @brief Portable getline() implementation using fgetc()
 *
 * Reads a line from a file stream, dynamically allocating memory as needed.
 * This is a portable replacement for POSIX getline() that works on Windows
 * and other non-POSIX systems. On POSIX systems, the native getline() is
 * preferred, but this provides a fallback.
 *
 * @param lineptr Pointer to buffer pointer (will be allocated/reallocated)
 * @param n Pointer to buffer size (will be updated)
 * @param stream File stream to read from
 * @return Number of characters read (excluding null terminator), or -1 on
 * EOF/error
 */
static ssize_t __attribute__((unused))
portable_getline(char **lineptr, size_t *n, FILE *stream) {
  if (!lineptr || !n || !stream) {
    return -1;
  }

  // Initialize buffer if needed
  if (!*lineptr || *n == 0) {
    *n = PORTABLE_GETLINE_INITIAL_SIZE;
    *lineptr = malloc(*n);
    if (!*lineptr) {
      return -1;
    }
  }

  size_t pos = 0;
  int c;

  while ((c = fgetc(stream)) != EOF) {
    // Grow buffer if needed
    if (pos + 1 >= *n) {
      size_t new_size = *n * 2;
      char *new_buf = realloc(*lineptr, new_size);
      if (!new_buf) {
        return -1;
      }
      *lineptr = new_buf;
      *n = new_size;
    }

    (*lineptr)[pos++] = (char)c;

    // Stop at newline
    if (c == '\n') {
      break;
    }
  }

  // Check for EOF without reading anything
  if (pos == 0 && c == EOF) {
    return -1;
  }

  // Null-terminate the string
  (*lineptr)[pos] = '\0';

  return (ssize_t)pos;
}

// Define a macro to use the appropriate getline implementation
#ifdef _WIN32
// On Windows, always use portable implementation
#define KRONOS_GETLINE(lineptr, n, stream) portable_getline(lineptr, n, stream)
#else
// On POSIX systems, prefer native getline() but fallback to portable if needed
#if defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200809L
// Use native POSIX getline()
#define KRONOS_GETLINE(lineptr, n, stream) getline(lineptr, n, stream)
#else
// Fallback to portable implementation
#define KRONOS_GETLINE(lineptr, n, stream) portable_getline(lineptr, n, stream)
#endif
#endif

/**
 * @brief Clean up a call frame's local variables
 *
 * Frees all names, releases all values, and frees all type names
 * in the given call frame, then resets the local_count to 0.
 *
 * @param frame Call frame to clean up (must not be NULL)
 */
static void cleanup_call_frame_locals(CallFrame *frame) {
  for (size_t i = 0; i < frame->local_count; i++) {
    if (frame->locals[i].name) {
      free(frame->locals[i].name);
      frame->locals[i].name = NULL;
    }
    if (frame->locals[i].value) {
      value_release(frame->locals[i].value);
      frame->locals[i].value = NULL;
    }
    if (frame->locals[i].type_name) {
      free(frame->locals[i].type_name);
      frame->locals[i].type_name = NULL;
    }
  }
  frame->local_count = 0;

  // Initialize local variable hash table to all NULL
  for (size_t i = 0; i < LOCALS_MAX; i++) {
    frame->local_hash[i] = NULL;
  }
}

// Forward declaration for vm_execute (needed by call_module_function)
int vm_execute(KronosVM *vm, Bytecode *bytecode);

// Forward declarations for functions used in vm_call_function_value
static int push(KronosVM *vm, KronosValue *value);
static KronosValue *pop(KronosVM *vm);
static int vm_propagate_error(KronosVM *vm, KronosErrorCode fallback);


/**
 * @brief Call a function value (lambda)
 *
 * Handles calling a VAL_FUNCTION value stored in a variable. Creates a call
 * frame, binds arguments to parameters, and executes the function body.
 *
 * @param vm The VM
 * @param func_val The function value to call
 * @param func_name The variable name (for error messages)
 * @param arg_count Number of arguments on the stack
 * @return 0 on success, negative error code on failure
 */
int vm_call_function_value(KronosVM *vm, KronosValue *func_val,
                           const char *func_name, uint8_t arg_count) {
  int total_arity = func_val->as.function.arity;
  int required_arity = func_val->as.function.required_arity;
  bool has_variadic = func_val->as.function.has_variadic;
  size_t regular_param_count = total_arity - (has_variadic ? 1 : 0);

  // Validate argument count with default/variadic support
  if (arg_count < required_arity) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function '%s' requires at least %d argument%s, but got %d",
                     func_name, required_arity,
                     required_arity == 1 ? "" : "s", arg_count);
  }
  if (!has_variadic && arg_count > total_arity) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function '%s' accepts at most %d argument%s, but got %d",
                     func_name, total_arity,
                     total_arity == 1 ? "" : "s", arg_count);
  }

  // Check call stack size
  if (vm->call_stack_size >= CALL_STACK_MAX) {
    return vm_error(vm, KRONOS_ERR_RUNTIME, "Maximum call depth exceeded");
  }

  // Create new call frame
  CallFrame *frame = &vm->call_stack[vm->call_stack_size++];
  frame->function = NULL; // Lambda (no named function)
  frame->return_ip = vm->ip;
  frame->return_bytecode = vm->bytecode;
  frame->frame_start = vm->stack_top;
  frame->local_count = 0;
  frame->owned_bytecode = NULL; // Will be set if we allocate bytecode
  // Initialize local variable hash table to all NULL
  for (size_t i = 0; i < LOCALS_MAX; i++) {
    frame->local_hash[i] = NULL;
  }

  // Validate stack has enough arguments
  if (vm->stack_top < vm->stack) {
    vm->call_stack_size--;
    if (vm->call_stack_size > 0) {
      vm->current_frame = &vm->call_stack[vm->call_stack_size - 1];
    } else {
      vm->current_frame = NULL;
    }
    return vm_error(vm, KRONOS_ERR_RUNTIME, "Stack pointer corruption");
  }

  size_t stack_size = vm->stack_top - vm->stack;
  if (stack_size < arg_count) {
    vm->call_stack_size--;
    if (vm->call_stack_size > 0) {
      vm->current_frame = &vm->call_stack[vm->call_stack_size - 1];
    } else {
      vm->current_frame = NULL;
    }
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Stack underflow: function expects %d arguments",
                     arg_count);
  }

  // Pop arguments and bind to parameters
  KronosValue **args =
      arg_count > 0 ? malloc(sizeof(KronosValue *) * arg_count) : NULL;
  if (arg_count > 0 && !args) {
    vm->call_stack_size--;
    if (vm->call_stack_size > 0) {
      vm->current_frame = &vm->call_stack[vm->call_stack_size - 1];
    } else {
      vm->current_frame = NULL;
    }
    return vm_error(vm, KRONOS_ERR_INTERNAL,
                    "Failed to allocate argument buffer");
  }

  for (int i = arg_count - 1; i >= 0; i--) {
    args[i] = pop(vm);
    if (!args[i]) {
      for (size_t j = i + 1; j < arg_count; j++) {
        value_release(args[j]);
      }
      free(args);
      vm->call_stack_size--;
      if (vm->call_stack_size > 0) {
        vm->current_frame = &vm->call_stack[vm->call_stack_size - 1];
      } else {
        vm->current_frame = NULL;
      }
      return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
    }
  }

  // Set current frame before setting locals
  vm->current_frame = frame;

  // Helper macro for cleanup on error
  #define CLEANUP_LAMBDA_FRAME() do { \
    for (size_t j = 0; j < frame->local_count; j++) { \
      free(frame->locals[j].name); \
      value_release(frame->locals[j].value); \
      free(frame->locals[j].type_name); \
    } \
    frame->local_count = 0; \
    vm->call_stack_size--; \
    if (vm->call_stack_size > 0) { \
      vm->current_frame = &vm->call_stack[vm->call_stack_size - 1]; \
    } else { \
      vm->current_frame = NULL; \
    } \
  } while(0)

  // Calculate how many regular arguments were provided
  size_t regular_args_provided = has_variadic ?
      ((size_t)arg_count > regular_param_count ? regular_param_count : arg_count) :
      arg_count;

  // Bind regular parameters (provided arguments + defaults)
  for (size_t i = 0; i < regular_param_count; i++) {
    KronosValue *arg_val;
    if (i < regular_args_provided) {
      arg_val = args[i];
      value_retain(arg_val);
    } else {
      // Use default value
      if (func_val->as.function.param_defaults &&
          func_val->as.function.param_defaults[i]) {
        arg_val = func_val->as.function.param_defaults[i];
        value_retain(arg_val);
      } else {
        for (size_t j = 0; j < arg_count; j++) {
          value_release(args[j]);
        }
        free(args);
        CLEANUP_LAMBDA_FRAME();
        return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                         "Missing required argument for parameter '%s'",
                         func_val->as.function.param_names[i]);
      }
    }

    int arg_status = vm_set_local(vm, frame,
                                   func_val->as.function.param_names[i],
                                   arg_val, true, NULL);
    value_release(arg_val);
    if (arg_status != 0) {
      for (size_t j = 0; j < arg_count; j++) {
        value_release(args[j]);
      }
      free(args);
      CLEANUP_LAMBDA_FRAME();
      return arg_status;
    }
  }

  // Handle variadic parameter - collect remaining arguments into a list
  if (has_variadic) {
    size_t variadic_idx = total_arity - 1;
    size_t variadic_count = (size_t)arg_count > regular_param_count ?
        arg_count - regular_param_count : 0;

    KronosValue *variadic_list = value_new_list(variadic_count > 0 ? variadic_count : 4);
    if (!variadic_list) {
      for (size_t j = 0; j < arg_count; j++) {
        value_release(args[j]);
      }
      free(args);
      CLEANUP_LAMBDA_FRAME();
      return vm_error(vm, KRONOS_ERR_INTERNAL,
                      "Failed to create variadic argument list");
    }

    for (size_t i = 0; i < variadic_count; i++) {
      size_t arg_idx = regular_param_count + i;
      if (variadic_list->as.list.count >= variadic_list->as.list.capacity) {
        size_t new_capacity = variadic_list->as.list.capacity == 0 ? 4 :
                              variadic_list->as.list.capacity * 2;
        KronosValue **new_items = realloc(variadic_list->as.list.items,
                                          sizeof(KronosValue *) * new_capacity);
        if (!new_items) {
          value_release(variadic_list);
          for (size_t j = 0; j < arg_count; j++) {
            value_release(args[j]);
          }
          free(args);
          CLEANUP_LAMBDA_FRAME();
          return vm_error(vm, KRONOS_ERR_INTERNAL,
                          "Failed to grow variadic argument list");
        }
        variadic_list->as.list.items = new_items;
        variadic_list->as.list.capacity = new_capacity;
      }
      value_retain(args[arg_idx]);
      variadic_list->as.list.items[variadic_list->as.list.count++] = args[arg_idx];
    }

    int arg_status = vm_set_local(vm, frame,
                                   func_val->as.function.param_names[variadic_idx],
                                   variadic_list, true, NULL);
    value_release(variadic_list);
    if (arg_status != 0) {
      for (size_t j = 0; j < arg_count; j++) {
        value_release(args[j]);
      }
      free(args);
      CLEANUP_LAMBDA_FRAME();
      return arg_status;
    }
  }

  // Release original argument references
  for (size_t i = 0; i < arg_count; i++) {
    value_release(args[i]);
  }
  free(args);

  #undef CLEANUP_LAMBDA_FRAME

  // Validate function bytecode
  if (!func_val->as.function.bytecode || func_val->as.function.length == 0) {
    // Clean up and error
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
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Function bytecode is invalid");
  }

  // Create temporary bytecode structure for lambda execution
  // Lambda body uses the parent's constant pool
  Bytecode lambda_bytecode;
  lambda_bytecode.code = func_val->as.function.bytecode;
  lambda_bytecode.count = func_val->as.function.length;
  lambda_bytecode.capacity = func_val->as.function.length;
  lambda_bytecode.constants = vm->bytecode->constants;
  lambda_bytecode.const_count = vm->bytecode->const_count;
  lambda_bytecode.const_capacity = vm->bytecode->const_capacity;

  // Store lambda bytecode in frame for lifetime management
  // Actually, we need to keep the bytecode alive during execution.
  // Since the lambda's bytecode is in VAL_FUNCTION which is retained elsewhere,
  // we can just point to it. The Bytecode struct is on the stack but that's ok
  // since we return synchronously.

  // Switch to lambda bytecode
  // We need to allocate a Bytecode on the heap because we switch vm->bytecode
  Bytecode *func_bytecode = malloc(sizeof(Bytecode));
  if (!func_bytecode) {
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
    return vm_error(vm, KRONOS_ERR_INTERNAL,
                    "Failed to allocate function bytecode structure");
  }
  *func_bytecode = lambda_bytecode;

  // Store pointer for cleanup on return
  frame->owned_bytecode = func_bytecode;

  // Switch to lambda bytecode
  vm->bytecode = func_bytecode;
  vm->ip = func_bytecode->code;

  return 0;
}

/**
 * @brief Call a function in an external module
 *
 * DESIGN DECISION: Each module has its own VM for namespace isolation. Create
 * call frame in module's VM (not caller's) so module functions access their
 * own globals.
 *
 * EDGE CASES: Call stack overflow checked, errors propagated to caller_vm,
 * arguments released by this function (ownership to module VM), return IP NULL
 * (indicates module call, not bytecode jump).
 *
 * @param caller_vm The VM making the call (for error reporting)
 * @param mod The module containing the function
 * @param mod_func The function to call
 * @param args Array of arguments (will be released by this function)
 * @param arg_count Number of arguments
 * @return 0 on success, negative error code on failure
 */
static int call_module_function(KronosVM *caller_vm, Module *mod,
                                Function *mod_func, KronosValue **args,
                                uint8_t arg_count) {
  KronosVM *module_vm = mod->module_vm;

  // Check call stack depth
  if (module_vm->call_stack_size >= CALL_STACK_MAX) {
    for (size_t i = 0; i < arg_count; i++) {
      value_release(args[i]);
    }
    return vm_error(caller_vm, KRONOS_ERR_RUNTIME,
                    "Maximum call depth exceeded in module");
  }

  // Create call frame in module VM
  CallFrame *mod_frame = &module_vm->call_stack[module_vm->call_stack_size++];
  mod_frame->function = mod_func;
  mod_frame->return_ip = NULL;
  mod_frame->return_bytecode = NULL;
  mod_frame->frame_start = module_vm->stack_top;
  mod_frame->local_count = 0;
  mod_frame->owned_bytecode = NULL;
  // Initialize local variable hash table to all NULL
  for (size_t i = 0; i < LOCALS_MAX; i++) {
    mod_frame->local_hash[i] = NULL;
  }

  // Set current_frame BEFORE setting locals
  module_vm->current_frame = mod_frame;

  // Set parameters as local variables
  for (size_t i = 0; i < mod_func->param_count; i++) {
    int status = vm_set_local(module_vm, mod_frame, mod_func->params[i],
                              args[i], true, NULL);
    value_release(args[i]); // Local now owns it

    if (status != 0) {
      // Release remaining arguments
      for (size_t j = i + 1; j < mod_func->param_count; j++) {
        value_release(args[j]);
      }
      // Clean up the frame
      cleanup_call_frame_locals(mod_frame);
      module_vm->call_stack_size--;
      module_vm->current_frame = NULL;
      return status;
    }
  }

  // Save module VM's execution state
  uint8_t *saved_mod_ip = module_vm->ip;
  Bytecode *saved_mod_bytecode = module_vm->bytecode;

  // Execute function body
  int exec_result = vm_execute(module_vm, &mod_func->bytecode);

  if (exec_result < 0) {
    // Copy error to caller VM
    if (module_vm->last_error_message) {
      vm_set_error(caller_vm, module_vm->last_error_code,
                   module_vm->last_error_message);
    }
    // Clean up
    cleanup_call_frame_locals(mod_frame);
    module_vm->call_stack_size--;
    module_vm->current_frame = NULL;
    module_vm->ip = saved_mod_ip;
    module_vm->bytecode = saved_mod_bytecode;
    return exec_result;
  }

  // Get return value from module VM stack
  KronosValue *return_val = NULL;
  if (module_vm->stack_top > module_vm->stack) {
    return_val = module_vm->stack_top[-1];
    module_vm->stack_top--;
    // return_val is now ours (stack no longer owns it)
  } else {
    return_val = value_new_nil();
    if (!return_val) {
      cleanup_call_frame_locals(mod_frame);
      module_vm->call_stack_size--;
      module_vm->current_frame = NULL;
      module_vm->ip = saved_mod_ip;
      module_vm->bytecode = saved_mod_bytecode;
      return vm_error(caller_vm, KRONOS_ERR_INTERNAL,
                      "Failed to create nil value");
    }
  }

  // Clean up module VM state
  cleanup_call_frame_locals(mod_frame);
  module_vm->call_stack_size--;
  module_vm->current_frame = NULL;
  module_vm->ip = saved_mod_ip;
  module_vm->bytecode = saved_mod_bytecode;

  // Push return value to caller VM
  if (caller_vm->stack_top >= caller_vm->stack + STACK_MAX) {
    value_release(return_val);
    return vm_error(caller_vm, KRONOS_ERR_RUNTIME, "Stack overflow");
  }
  *caller_vm->stack_top++ = return_val;
  value_retain(return_val);
  value_release(return_val);

  return 0;
}

/**
 * @brief Finalize error state in the VM
 *
 * Internal helper to set error code and message, and invoke error callback.
 *
 * @param vm VM instance
 * @param code Error code
 * @param owned_message Error message (will be owned by VM, can be NULL)
 * @param fallback_msg Fallback message if owned_message is NULL
 */
static void vm_finalize_error(KronosVM *vm, KronosErrorCode code,
                              char *owned_message, const char *fallback_msg) {
  if (!vm) {
    return;
  }

  free(vm->last_error_message);
  vm->last_error_message = owned_message;
  vm->last_error_code = code;

  // Clear error type on error clear, but preserve on error set
  if (code == KRONOS_OK) {
    free(vm->last_error_type);
    vm->last_error_type = NULL;
  }

  if (vm->error_callback && code != KRONOS_OK) {
    const char *callback_msg = vm->last_error_message
                                   ? vm->last_error_message
                                   : (fallback_msg ? fallback_msg : "");
    vm->error_callback(vm, code, callback_msg);
  }
}

// Set error with explicit type name
static void vm_set_error_with_type(KronosVM *vm, KronosErrorCode code,
                                   const char *type_name, const char *message) {
  char *msg_copy = NULL;
  if (message) {
    msg_copy = strdup(message);
  }

  // Free and set error type
  free(vm->last_error_type);
  vm->last_error_type = type_name ? strdup(type_name) : NULL;

  vm_finalize_error(vm, code, msg_copy, message);
}

static char *vm_format_message(const char *fmt, va_list args) {
  if (!fmt) {
    return NULL;
  }

  va_list copy;
  va_copy(copy, args);
  int needed = vsnprintf(NULL, 0, fmt, copy);
  va_end(copy);

  if (needed < 0) {
    return NULL;
  }

  size_t size = (size_t)needed + 1;
  char *buffer = malloc(size);
  if (!buffer) {
    return NULL;
  }

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

/**
 * @brief Map error code to error type name
 *
 * Maps KronosErrorCode to Python-style error type names.
 *
 * @param code Error code
 * @return Error type name string (static, don't free)
 */
static const char *error_code_to_type_name(KronosErrorCode code) {
  switch (code) {
  case KRONOS_ERR_RUNTIME:
    return "RuntimeError";
  case KRONOS_ERR_PARSE:
    return "SyntaxError";
  case KRONOS_ERR_COMPILE:
    return "CompileError";
  case KRONOS_ERR_NOT_FOUND:
    return "NameError";
  case KRONOS_ERR_INVALID_ARGUMENT:
    return "ValueError";
  case KRONOS_ERR_INTERNAL:
    return "InternalError";
  default:
    return "Error";
  }
}

/**
 * @brief Handle exception if one occurred and exception handler exists
 *
 * Checks if there's an error and an active exception handler. If so,
 * jumps to the catch block handler. OP_CATCH will handle error type matching.
 *
 * @param vm VM instance
 * @return true if exception was handled (execution should continue), false
 * otherwise
 */
static bool handle_exception_if_any(KronosVM *vm) {
  if (!vm || vm->last_error_code == KRONOS_OK) {
    return false;
  }

  // If no exception handler, propagate the error (stop execution)
  if (vm->exception_handler_count == 0) {
    return false;
  }

  // Get the innermost exception handler
  size_t idx = vm->exception_handler_count - 1;

  // Jump to the exception handler (catch or finally)
  // The handler_ip points to the first OP_CATCH instruction
  vm->ip = vm->exception_handlers[idx].handler_ip;

  return true; // Exception handled, continue execution from handler
}

// Forward declaration
static size_t hash_global_name(const char *str);

/**
 * @brief Create a new virtual machine instance
 *
 * Initializes a VM with empty stack, no globals, and the built-in Pi constant.
 * The VM is ready to execute bytecode after creation.
 *
 * @return New VM instance, or NULL on allocation failure
 */
KronosVM *vm_new(void) {
  KronosVM *vm = malloc(sizeof(KronosVM));
  if (!vm) {
    return NULL;
  }

  vm->stack_top = vm->stack;
  vm->global_count = 0;
  vm->function_count = 0;
  vm->module_count = 0;
  vm->loading_count = 0;
  vm->current_file_path = NULL;
  vm->root_vm_ref = NULL; // Root VM has no parent
  vm->call_stack_size = 0;
  vm->current_frame = NULL;
  vm->ip = NULL;
  vm->bytecode = NULL;

  vm->last_error_message = NULL;
  vm->last_error_type = NULL;
  vm->last_error_code = KRONOS_OK;
  vm->error_callback = NULL;
  vm->exception_handler_count = 0;

  // Initialize function hash table to all NULL
  for (size_t i = 0; i < FUNCTIONS_MAX; i++) {
    vm->function_hash[i] = NULL;
  }

  // Initialize global variable hash table to all NULL
  for (size_t i = 0; i < GLOBALS_MAX; i++) {
    vm->global_hash[i] = NULL;
  }

  // Initialize Pi constant - immutable
  // Note: double precision provides ~15-17 decimal digits of precision
  // Use M_PI from math.h if available, otherwise use hardcoded value
#ifdef M_PI
  KronosValue *pi_value = value_new_number(M_PI);
#else
  KronosValue *pi_value = value_new_number(3.14159265358979323846);
#endif
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

    // Add to hash table for O(1) lookup (same as vm_set_global)
    size_t hash_index = hash_global_name("Pi");
    for (size_t i = 0; i < GLOBALS_MAX; i++) {
      size_t idx = (hash_index + i) % GLOBALS_MAX;
      if (!vm->global_hash[idx]) {
        // Found empty slot
        vm->global_hash[idx] = &vm->globals[vm->global_count];
        break;
      }
    }

    vm->global_count++;
  }

  return vm;
}

/**
 * @brief Free a VM instance and all its resources
 *
 * Releases all values on the stack, call frames, global variables,
 * and functions. After calling this, the VM pointer should not be used.
 *
 * @param vm VM instance to free (safe to pass NULL)
 */
void vm_free(KronosVM *vm) {
  if (!vm) {
    return;
  }

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

  // Release modules
  for (size_t i = 0; i < vm->module_count; i++) {
    Module *mod = vm->modules[i];
    if (mod) {
      free(mod->name);
      free(mod->file_path);
      if (mod->module_vm) {
        vm_free(mod->module_vm);
      }
      free(mod);
    }
  }

  // Release loading tracking
  for (size_t i = 0; i < vm->loading_count; i++) {
    free(vm->loading_modules[i]);
  }

  free(vm->current_file_path);
  free(vm->last_error_message);
  free(vm->last_error_type);
  free(vm);
}

/**
 * @brief Clear the VM stack, releasing all values
 *
 * This should be called before freeing bytecode to ensure constants
 * aren't retained on the stack, which would prevent them from being freed.
 *
 * @param vm VM instance
 */
void vm_clear_stack(KronosVM *vm) {
  if (!vm) {
    return;
  }

  // Release all values on stack
  while (vm->stack_top > vm->stack) {
    vm->stack_top--;
    value_release(*vm->stack_top);
  }
}

// Free a function
void function_free(Function *func) {
  if (!func) {
    return;
  }

  free(func->name);
  for (size_t i = 0; i < func->param_count; i++) {
    free(func->params[i]);
  }
  free(func->params);

  // Free default values
  if (func->param_defaults) {
    for (size_t i = 0; i < func->param_count; i++) {
      if (func->param_defaults[i]) {
        value_release(func->param_defaults[i]);
      }
    }
    free(func->param_defaults);
  }

  // Free bytecode structure
  free(func->bytecode.code);
  if (func->bytecode.constants) {
    for (size_t i = 0; i < func->bytecode.const_count; i++) {
      if (func->bytecode.constants[i]) {
        value_release(func->bytecode.constants[i]);
      }
    }
    free(func->bytecode.constants);
  }

  free(func);
}

/**
 * @brief Hash function for function names
 *
 * Simple djb2 hash algorithm for string hashing.
 *
 * @param str String to hash
 * @return Hash value (modulo FUNCTIONS_MAX)
 */
static size_t hash_function_name(const char *str) {
  unsigned long hash = 5381;
  int c;
  while ((c = *str++)) {
    hash = ((hash << 5) + hash) + c; // hash * 33 + c
  }
  return hash % FUNCTIONS_MAX;
}

/**
 * @brief Hash function for variable names (globals)
 *
 * Simple djb2 hash algorithm for string hashing.
 *
 * @param str String to hash
 * @return Hash value (modulo GLOBALS_MAX)
 */
static size_t hash_global_name(const char *str) {
  unsigned long hash = 5381;
  int c;
  while ((c = *str++)) {
    hash = ((hash << 5) + hash) + c; // hash * 33 + c
  }
  return hash % GLOBALS_MAX;
}

/**
 * @brief Hash function for variable names (locals)
 *
 * Simple djb2 hash algorithm for string hashing.
 *
 * @param str String to hash
 * @return Hash value (modulo LOCALS_MAX)
 */
static size_t hash_local_name(const char *str) {
  unsigned long hash = 5381;
  int c;
  while ((c = *str++)) {
    hash = ((hash << 5) + hash) + c; // hash * 33 + c
  }
  return hash % LOCALS_MAX;
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

  // Add to hash table for O(1) lookup.
  // Probe first so VM state is not mutated on duplicate/hash-full errors.
  if (func->name) {
    size_t index = hash_function_name(func->name);
    size_t empty_slot = SIZE_MAX;

    for (size_t i = 0; i < FUNCTIONS_MAX; i++) {
      size_t idx = (index + i) % FUNCTIONS_MAX;
      Function *existing = vm->function_hash[idx];
      if (!existing) {
        empty_slot = idx;
        break;
      }
      if (existing->name && strcmp(existing->name, func->name) == 0) {
        return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                         "Function '%s' is already defined", func->name);
      }
    }

    if (empty_slot == SIZE_MAX) {
      return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                       "Function hash table is full (internal error)");
    }
    vm->function_hash[empty_slot] = func;
  }

  // Add to array (for iteration/debugging)
  vm->functions[vm->function_count++] = func;
  return 0;
}

// Get a function by name using hash table for O(1) lookup
Function *vm_get_function(KronosVM *vm, const char *name) {
  if (!vm || !name) {
    return NULL;
  }

  // Compute hash index
  size_t index = hash_function_name(name);

  // Linear probing to handle collisions
  for (size_t i = 0; i < FUNCTIONS_MAX; i++) {
    size_t idx = (index + i) % FUNCTIONS_MAX;
    Function *func = vm->function_hash[idx];

    // Empty slot means function not found
    if (!func) {
      return NULL;
    }

    // Check if this is the function we're looking for
    if (func->name && strcmp(func->name, name) == 0) {
      return func;
    }
  }

  // Hash table full (shouldn't happen if FUNCTIONS_MAX is respected)
  return NULL;
}

// Get a module by name
Module *vm_get_module(KronosVM *vm, const char *name) {
  if (!vm || !name) {
    return NULL;
  }

  for (size_t i = 0; i < vm->module_count; i++) {
    if (vm->modules[i] && strcmp(vm->modules[i]->name, name) == 0) {
      return vm->modules[i];
    }
  }
  return NULL;
}

// Resolve module file path (handles relative paths)
static char *resolve_module_path(const char *base_path,
                                 const char *module_path) {
  if (!module_path) {
    return NULL;
  }

  // If module_path is absolute, use it as-is
  if (module_path[0] == '/') {
    return strdup(module_path);
  }

  // If module_path starts with ./ or ../, resolve relative to base_path
  if ((module_path[0] == '.' && module_path[1] == '/') ||
      (module_path[0] == '.' && module_path[1] == '.' &&
       module_path[2] == '/')) {
    if (base_path && base_path[0] != '\0') {
      // Find the directory of base_path
      char *last_slash = strrchr(base_path, '/');
      if (last_slash) {
        size_t dir_len = (size_t)(last_slash - base_path) + 1;
        size_t module_len = strlen(module_path);
        char *resolved = malloc(dir_len + module_len + 1);
        if (!resolved)
          return NULL;

        strncpy(resolved, base_path, dir_len);
        strcpy(resolved + dir_len, module_path);
        return resolved;
      }
    }
    // No base_path or no directory separator, use as-is
    return strdup(module_path);
  }

  // If module_path contains a / but doesn't start with ./ or ../,
  // treat it as relative to project root (current working directory)
  // This handles cases like "examples/utils.kr" or "tests/module.kr"
  if (strchr(module_path, '/')) {
    return strdup(module_path);
  }

  // If base_path is provided and module_path has no /, resolve relative to
  // base_path's directory
  if (base_path && base_path[0] != '\0') {
    // Find the directory of base_path
    char *last_slash = strrchr(base_path, '/');
    if (last_slash) {
      size_t dir_len = (size_t)(last_slash - base_path) + 1;
      size_t module_len = strlen(module_path);
      char *resolved = malloc(dir_len + module_len + 1);
      if (!resolved)
        return NULL;

      strncpy(resolved, base_path, dir_len);
      strcpy(resolved + dir_len, module_path);
      return resolved;
    }
  }

  // Fallback: use module_path as-is (relative to current working directory)
  return strdup(module_path);
}

// Load a module from a file
// If parent_vm is provided, it's used to check for already-loaded modules and
// propagate loading stack
static int vm_load_module(KronosVM *vm, const char *module_name,
                          const char *file_path, const char *base_path,
                          KronosVM *parent_vm) {
  if (!vm || !module_name || !file_path) {
    return vm_error(vm, KRONOS_ERR_INVALID_ARGUMENT,
                    "Invalid arguments for module loading");
  }

  // Determine the root VM - modules are always stored in the root VM
  // The root VM is the one that doesn't have a parent, or is the top-level VM
  // If parent_vm is provided, use it. Otherwise, if vm is a module VM, use its
  // root_vm_ref
  KronosVM *root_vm = parent_vm;
  if (!root_vm) {
    // If vm is a module VM, use its root_vm_ref, otherwise vm is the root
    root_vm = vm->root_vm_ref ? vm->root_vm_ref : vm;
  }

  if (!root_vm) {
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to determine root VM");
  }

  // Check for circular imports in the root VM's loading stack
  for (size_t i = 0; i < root_vm->loading_count; i++) {
    if (root_vm->loading_modules[i] &&
        strcmp(root_vm->loading_modules[i], module_name) == 0) {
      return vm_errorf(
          vm, KRONOS_ERR_RUNTIME,
          "Circular import detected: module '%s' is already being loaded",
          module_name);
    }
  }

  // Check if module already loaded in root VM
  if (vm_get_module(root_vm, module_name)) {
    return 0; // Already loaded, success
  }

  if (root_vm->module_count >= MODULES_MAX) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Maximum number of modules exceeded (%d allowed)",
                     MODULES_MAX);
  }

  // Use current_file_path as base if base_path is NULL
  const char *actual_base = base_path ? base_path : vm->current_file_path;

  // Resolve file path
  char *resolved_path = resolve_module_path(actual_base, file_path);
  if (!resolved_path) {
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to resolve module path");
  }

  // Add to root VM's loading stack for circular import detection
  if (root_vm->loading_count >= MODULES_MAX) {
    free(resolved_path);
    return vm_error(vm, KRONOS_ERR_RUNTIME, "Too many nested imports");
  }
  root_vm->loading_modules[root_vm->loading_count] = strdup(module_name);
  if (!root_vm->loading_modules[root_vm->loading_count]) {
    free(resolved_path);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to track module loading");
  }
  root_vm->loading_count++;

  // Read file (using portable fopen for UTF-8 support)
  FILE *file = portable_fopen(resolved_path, "r");
  if (!file) {
    int err =
        vm_errorf(vm, KRONOS_ERR_NOT_FOUND, "Failed to open module file: %s",
                  file_path);
    free(resolved_path);
    root_vm->loading_count--;
    free(root_vm->loading_modules[root_vm->loading_count]);
    root_vm->loading_modules[root_vm->loading_count] = NULL;
    return err;
  }

  // Determine file size
  if (fseek(file, 0, SEEK_END) != 0) {
    int err = vm_errorf(vm, KRONOS_ERR_IO, "Failed to seek to end of file: %s",
                        resolved_path);
    fclose(file);
    free(resolved_path);
    root_vm->loading_count--;
    free(root_vm->loading_modules[root_vm->loading_count]);
    root_vm->loading_modules[root_vm->loading_count] = NULL;
    return err;
  }

  long size = ftell(file);
  if (size < 0) {
    int err = vm_errorf(vm, KRONOS_ERR_IO, "Failed to determine file size: %s",
                        resolved_path);
    fclose(file);
    free(resolved_path);
    root_vm->loading_count--;
    free(root_vm->loading_modules[root_vm->loading_count]);
    root_vm->loading_modules[root_vm->loading_count] = NULL;
    return err;
  }

  if ((uintmax_t)size > (uintmax_t)(SIZE_MAX - 1)) {
    int err =
        vm_errorf(vm, KRONOS_ERR_IO, "File too large to read: %s", resolved_path);
    fclose(file);
    free(resolved_path);
    root_vm->loading_count--;
    free(root_vm->loading_modules[root_vm->loading_count]);
    root_vm->loading_modules[root_vm->loading_count] = NULL;
    return err;
  }

  if (fseek(file, 0, SEEK_SET) != 0) {
    int err = vm_errorf(vm, KRONOS_ERR_IO, "Failed to seek to start of file: %s",
                        resolved_path);
    fclose(file);
    free(resolved_path);
    root_vm->loading_count--;
    free(root_vm->loading_modules[root_vm->loading_count]);
    root_vm->loading_modules[root_vm->loading_count] = NULL;
    return err;
  }

  // Allocate buffer
  size_t length = (size_t)size;
  char *source = malloc(length + 1);
  if (!source) {
    int err = vm_error(vm, KRONOS_ERR_INTERNAL,
                       "Failed to allocate memory for module file");
    free(resolved_path);
    fclose(file);
    root_vm->loading_count--;
    free(root_vm->loading_modules[root_vm->loading_count]);
    root_vm->loading_modules[root_vm->loading_count] = NULL;
    return err;
  }

  size_t read_size = fread(source, 1, length, file);
  if (ferror(file) || (read_size < length && !feof(file))) {
    int err = vm_errorf(vm, KRONOS_ERR_IO, "Failed to read module file: %s",
                        resolved_path);
    free(source);
    fclose(file);
    free(resolved_path);
    root_vm->loading_count--;
    free(root_vm->loading_modules[root_vm->loading_count]);
    root_vm->loading_modules[root_vm->loading_count] = NULL;
    return err;
  }

  source[read_size] = '\0';
  fclose(file);

  // Create a new VM for the module
  KronosVM *module_vm = vm_new();
  if (!module_vm) {
    free(source);
    free(resolved_path);
    // Remove from root VM's loading stack
    root_vm->loading_count--;
    free(root_vm->loading_modules[root_vm->loading_count]);
    root_vm->loading_modules[root_vm->loading_count] = NULL;
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create VM for module");
  }

  // Set the module VM's root VM reference for circular import detection
  module_vm->root_vm_ref = root_vm;

  // Set the module VM's current file path for relative imports
  module_vm->current_file_path = strdup(resolved_path);
  if (!module_vm->current_file_path) {
    vm_free(module_vm);
    free(source);
    free(resolved_path);
    // Remove from root VM's loading stack
    root_vm->loading_count--;
    free(root_vm->loading_modules[root_vm->loading_count]);
    root_vm->loading_modules[root_vm->loading_count] = NULL;
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to set module file path");
  }

  // Tokenize, parse, compile, and execute the module
  TokenArray *tokens = tokenize(source, NULL);
  free(source);

  if (!tokens) {
    vm_free(module_vm);
    free(resolved_path);
    // Remove from root VM's loading stack
    root_vm->loading_count--;
    free(root_vm->loading_modules[root_vm->loading_count]);
    root_vm->loading_modules[root_vm->loading_count] = NULL;
    return vm_error(vm, KRONOS_ERR_TOKENIZE, "Failed to tokenize module");
  }

  AST *ast = parse(tokens, NULL);
  token_array_free(tokens);

  if (!ast) {
    vm_free(module_vm);
    free(resolved_path);
    // Remove from root VM's loading stack
    root_vm->loading_count--;
    free(root_vm->loading_modules[root_vm->loading_count]);
    root_vm->loading_modules[root_vm->loading_count] = NULL;
    return vm_error(vm, KRONOS_ERR_PARSE, "Failed to parse module");
  }

  const char *compile_err = NULL;
  Bytecode *bytecode = compile(ast, &compile_err);
  ast_free(ast);

  if (!bytecode) {
    vm_free(module_vm);
    free(resolved_path);
    // Remove from root VM's loading stack
    root_vm->loading_count--;
    free(root_vm->loading_modules[root_vm->loading_count]);
    root_vm->loading_modules[root_vm->loading_count] = NULL;
    return vm_errorf(vm, KRONOS_ERR_COMPILE, "Failed to compile module%s%s",
                     compile_err ? ": " : "", compile_err ? compile_err : "");
  }

  // Check import depth before recursive vm_execute() call to prevent C stack
  // exhaustion loading_count represents the current depth of the import chain
  if (root_vm->loading_count > IMPORT_DEPTH_MAX) {
    vm_free(module_vm);
    free(resolved_path);
    // Remove from root VM's loading stack
    root_vm->loading_count--;
    free(root_vm->loading_modules[root_vm->loading_count]);
    root_vm->loading_modules[root_vm->loading_count] = NULL;
    bytecode_free(bytecode);
    return vm_errorf(
        vm, KRONOS_ERR_RUNTIME,
        "Import depth exceeded maximum (%d). Deep import chains can "
        "exhaust the C stack.",
        IMPORT_DEPTH_MAX);
  }

  int exec_result = vm_execute(module_vm, bytecode);

  if (exec_result < 0) {
    // Execution failed - clean up resources
    // Clear stack first to release any values that might reference bytecode
    // constants
    vm_clear_stack(module_vm);
    // Free bytecode (frees constants that might be referenced)
    bytecode_free(bytecode);
    // Copy error from module_vm to main vm before freeing
    if (module_vm->last_error_message) {
      vm_set_error(vm, module_vm->last_error_code,
                   module_vm->last_error_message);
    }
    // Free module VM (frees all VM resources including current_file_path)
    vm_free(module_vm);
    free(resolved_path);
    // Remove from root VM's loading stack
    root_vm->loading_count--;
    free(root_vm->loading_modules[root_vm->loading_count]);
    root_vm->loading_modules[root_vm->loading_count] = NULL;
    return exec_result;
  }

  // Execution succeeded - clean up execution resources
  vm_clear_stack(module_vm);
  bytecode_free(bytecode);

  // Remove from root VM's loading stack (module successfully loaded)
  root_vm->loading_count--;
  free(root_vm->loading_modules[root_vm->loading_count]);
  root_vm->loading_modules[root_vm->loading_count] = NULL;

  // Create module structure
  Module *mod = malloc(sizeof(Module));
  if (!mod) {
    vm_free(module_vm);
    free(resolved_path);
    return vm_error(vm, KRONOS_ERR_INTERNAL,
                    "Failed to allocate module structure");
  }

  mod->name = strdup(module_name);
  mod->file_path = resolved_path;
  mod->module_vm = module_vm;
  mod->root_vm = root_vm; // Store root VM for circular import detection
  mod->is_loaded = true;

  if (!mod->name) {
    free(mod);
    vm_free(module_vm);
    free(resolved_path);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate module name");
  }

  // Add module to root VM (not the current VM, which might be a module VM)
  root_vm->modules[root_vm->module_count++] = mod;

  return 0;
}

/**
 * @brief Push a value onto the VM stack
 *
 * Retains the value while it's on the stack. Fails if stack overflow occurs.
 *
 * @param vm VM instance
 * @param value Value to push (will be retained)
 * @return 0 on success, negative error code on failure
 */
static int push(KronosVM *vm, KronosValue *value) {
  if (vm->stack_top >= vm->stack + STACK_MAX) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Stack overflow (too many nested operations or calls)");
  }
  *vm->stack_top = value;
  vm->stack_top++;
  value_retain(value); // Retain while on stack
  return 0;
}

/**
 * @brief Pop a value from the VM stack
 *
 * Returns the value without releasing it (caller must handle reference
 * counting). Fails if stack underflow occurs.
 *
 * @param vm VM instance
 * @return Popped value, or NULL on underflow
 */
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

/**
 * @brief Helper macro to pop a value and check for errors
 *
 * Pops a value from the stack and checks if it's NULL. If NULL, returns
 * an error immediately. This reduces boilerplate in opcode handlers.
 *
 * Usage:
 *   KronosValue *value;
 *   POP_OR_RETURN(vm, value);
 *   // value is now guaranteed to be non-NULL
 *
 * @param vm VM instance
 * @param var Variable name to store the popped value
 */
#define POP_OR_RETURN(vm, var)                                                 \
  do {                                                                         \
    (var) = pop(vm);                                                           \
    if (!(var)) {                                                              \
      return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);                       \
    }                                                                          \
  } while (0)

/**
 * @brief Helper macro to pop a value with cleanup on error
 *
 * Pops a value from the stack and checks if it's NULL. If NULL, executes
 * cleanup code and returns an error. Used when popping multiple values where
 * earlier values need cleanup on error.
 *
 * Usage:
 *   KronosValue *b;
 *   POP_OR_RETURN(vm, b);
 *   KronosValue *a;
 *   POP_OR_RETURN_WITH_CLEANUP(vm, a, value_release(b));
 *   // a and b are now guaranteed to be non-NULL
 *
 * @param vm VM instance
 * @param var Variable name to store the popped value
 * @param cleanup Code to execute before returning on error (e.g.,
 * value_release(...))
 */
#define POP_OR_RETURN_WITH_CLEANUP(vm, var, cleanup)                           \
  do {                                                                         \
    (var) = pop(vm);                                                           \
    if (!(var)) {                                                              \
      cleanup;                                                                 \
      return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);                       \
    }                                                                          \
  } while (0)

/**
 * @brief Helper macro to push a value with cleanup on error
 *
 * Pushes a value onto the stack. If push fails, executes cleanup code and
 * returns an error. Used when pushing a value that needs to be released if
 * the push fails, or when other values need cleanup.
 *
 * Usage:
 *   KronosValue *result = value_new_number(42);
 *   PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result));
 *   // result is now on the stack (retained by push)
 *
 * Or with multiple cleanup statements:
 *   PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
 *                                value_release(a); value_release(b));
 *
 * @param vm VM instance
 * @param value Value to push onto the stack
 * @param cleanup Code to execute before returning on error (e.g.,
 * value_release(...))
 */
#define PUSH_OR_RETURN_WITH_CLEANUP(vm, value, cleanup)                        \
  do {                                                                         \
    if (push(vm, value) != 0) {                                                \
      cleanup;                                                                 \
      return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);                       \
    }                                                                          \
  } while (0)

static KronosValue *peek(KronosVM *vm, int distance) {
  // Bounds checking: ensure distance is valid
  // Guard: distance must be >= 0 and < stack size
  if (distance < 0) {
    vm_set_errorf(vm, KRONOS_ERR_INTERNAL,
                  "peek: distance must be non-negative (got %d)", distance);
    return NULL;
  }

  // Compute current stack size
  size_t stack_size = vm->stack_top - vm->stack;

  // Guard: distance must be < stack size to access valid memory
  if ((size_t)distance >= stack_size) {
    vm_set_errorf(
        vm, KRONOS_ERR_RUNTIME,
        "Stack underflow in peek (distance %d exceeds stack size %zu)",
        distance, stack_size);
    return NULL;
  }

  return vm->stack_top[-1 - distance];
}

/**
 * @brief Set or create a global variable
 *
 * Creates a new global variable or updates an existing mutable one.
 * Enforces immutability and type checking if type_name was specified.
 *
 * @param vm VM instance
 * @param name Variable name
 * @param value Value to assign (will be retained by VM)
 * @param is_mutable Whether the variable can be reassigned
 * @param type_name Optional type annotation (e.g., "number", "string")
 * @return 0 on success, negative error code on failure
 */
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
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Maximum number of global variables exceeded (%d allowed)",
                     GLOBALS_MAX);
  }

  // Validate initial assignment against declared type.
  if (type_name != NULL && !value_is_type(value, type_name)) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Type mismatch for variable '%s': expected '%s'", name,
                     type_name);
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

  // Add to hash table for O(1) lookup
  size_t hash_index = hash_global_name(name);
  for (size_t i = 0; i < GLOBALS_MAX; i++) {
    size_t idx = (hash_index + i) % GLOBALS_MAX;
    if (!vm->global_hash[idx]) {
      // Found empty slot
      vm->global_hash[idx] = &vm->globals[vm->global_count];
      break;
    }
  }

  // Only increment global_count after everything succeeds
  vm->global_count++;
  return 0;
}

KronosValue *vm_get_global(KronosVM *vm, const char *name) {
  if (!vm || !name) {
    return NULL;
  }

  // Use hash table for O(1) lookup
  size_t index = hash_global_name(name);
  for (size_t i = 0; i < GLOBALS_MAX; i++) {
    size_t idx = (index + i) % GLOBALS_MAX;
    struct GlobalVar *global = vm->global_hash[idx];

    // Empty slot means variable not found
    if (!global) {
      return NULL;
    }

    // Check if this is the variable we're looking for
    if (global->name && strcmp(global->name, name) == 0) {
      return global->value;
    }
  }

  // Hash table full (shouldn't happen if GLOBALS_MAX is respected)
  return NULL;
}

// Set local variable in current frame
int vm_set_local(KronosVM *vm, CallFrame *frame, const char *name,
                 KronosValue *value, bool is_mutable, const char *type_name) {
  if (!vm || !frame || !name || !value)
    return vm_error(vm, KRONOS_ERR_INVALID_ARGUMENT,
                    "vm_set_local requires non-null inputs");

  // Check if variable already exists using hash table
  size_t index = hash_local_name(name);
  for (size_t i = 0; i < LOCALS_MAX; i++) {
    size_t idx = (index + i) % LOCALS_MAX;
    struct LocalVar *local = frame->local_hash[idx];

    if (!local) {
      // Empty slot - variable doesn't exist, will add new one below
      break;
    }

    if (local->name && strcmp(local->name, name) == 0) {
      // Found existing variable
      // Check if it's immutable
      if (!local->is_mutable) {
        return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                         "Cannot reassign immutable local variable '%s'", name);
      }

      // Check type if specified
      if (local->type_name != NULL && !value_is_type(value, local->type_name)) {
        return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                         "Type mismatch for local variable '%s': expected '%s'",
                         name, local->type_name);
      }

      value_release(local->value);
      local->value = value;
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

  // Validate initial assignment against declared type.
  if (type_name != NULL && !value_is_type(value, type_name)) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Type mismatch for local variable '%s': expected '%s'",
                     name, type_name);
  }

  // Allocate into temporary pointers first, check each for NULL
  char *name_copy = strdup(name);
  if (!name_copy) {
    // Allocation failure: return error without modifying frame state
    // Note: Do NOT release value here - caller still owns it
    return vm_error(vm, KRONOS_ERR_INTERNAL,
                    "Failed to allocate memory for local name");
  }

  char *type_copy = NULL;
  if (type_name) {
    type_copy = strdup(type_name);
    if (!type_copy) {
      // Allocation failure: free already-allocated name_copy and return error
      // Note: Do NOT release value here - caller still owns it
      free(name_copy);
      return vm_error(vm, KRONOS_ERR_INTERNAL,
                      "Failed to allocate memory for local type");
    }
  }

  // Only assign to frame->locals[...] after all allocations succeed
  frame->locals[frame->local_count].name = name_copy;
  frame->locals[frame->local_count].value = value;
  frame->locals[frame->local_count].is_mutable = is_mutable;
  frame->locals[frame->local_count].type_name = type_copy;

  // Add to hash table for O(1) lookup
  size_t hash_index = hash_local_name(name);
  for (size_t i = 0; i < LOCALS_MAX; i++) {
    size_t idx = (hash_index + i) % LOCALS_MAX;
    if (!frame->local_hash[idx]) {
      // Found empty slot
      frame->local_hash[idx] = &frame->locals[frame->local_count];
      break;
    }
  }

  // Only call value_retain after all allocations and assignments succeed
  value_retain(value);
  // Only increment frame->local_count after everything succeeds
  frame->local_count++;
  return 0;
}

// Get local variable from current frame using hash table for O(1) lookup
KronosValue *vm_get_local(CallFrame *frame, const char *name) {
  if (!frame || !name) {
    return NULL;
  }

  // Use hash table for O(1) lookup
  size_t index = hash_local_name(name);
  for (size_t i = 0; i < LOCALS_MAX; i++) {
    size_t idx = (index + i) % LOCALS_MAX;
    struct LocalVar *local = frame->local_hash[idx];

    // Empty slot means variable not found
    if (!local) {
      return NULL;
    }

    // Check if this is the variable we're looking for
    if (local->name && strcmp(local->name, name) == 0) {
      return local->value;
    }
  }

  // Hash table full (shouldn't happen if LOCALS_MAX is respected)
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
  // Check for error after first read_byte
  if (vm->last_error_message) {
    return 0; // Return 0 on error (caller should check error state)
  }
  uint16_t low = read_byte(vm);
  // Check for error after second read_byte
  if (vm->last_error_message) {
    return 0; // Return 0 on error (caller should check error state)
  }
  return (high << 8) | low;
}

static int16_t read_int16(KronosVM *vm) {
  uint16_t high = read_byte(vm);
  // Check for error after first read_byte
  if (vm->last_error_message) {
    return 0; // Return 0 on error (caller should check error state)
  }
  uint16_t low = read_byte(vm);
  // Check for error after second read_byte
  if (vm->last_error_message) {
    return 0; // Return 0 on error (caller should check error state)
  }
  uint16_t unsigned_val = (high << 8) | low;
  // Sign extend from 16-bit to int16_t
  return (int16_t)unsigned_val;
}

// Read constant from pool
static KronosValue *read_constant(KronosVM *vm) {
  uint16_t idx = read_uint16(vm);
  // Check for error from read_uint16 (which calls read_byte twice)
  if (vm->last_error_message) {
    return NULL; // Error already set by read_byte
  }
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

// Opcode handler function type
// Returns 0 on success, negative error code on failure
typedef int (*OpcodeHandler)(KronosVM *vm);

// Forward declarations for all opcode handlers
static int handle_op_load_const(KronosVM *vm);
static int handle_op_load_var(KronosVM *vm);
static int handle_op_store_var(KronosVM *vm);
static int handle_op_print(KronosVM *vm);
static int handle_op_debug(KronosVM *vm);
static int handle_op_add(KronosVM *vm);
static int handle_op_sub(KronosVM *vm);
static int handle_op_mul(KronosVM *vm);
static int handle_op_div(KronosVM *vm);
static int handle_op_mod(KronosVM *vm);
static int handle_op_neg(KronosVM *vm);
static int handle_op_eq(KronosVM *vm);
static int handle_op_neq(KronosVM *vm);
static int handle_op_gt(KronosVM *vm);
static int handle_op_lt(KronosVM *vm);
static int handle_op_gte(KronosVM *vm);
static int handle_op_lte(KronosVM *vm);
static int handle_op_and(KronosVM *vm);
static int handle_op_or(KronosVM *vm);
static int handle_op_not(KronosVM *vm);
static int handle_op_jump(KronosVM *vm);
static int handle_op_jump_if_false(KronosVM *vm);
static int handle_op_define_func(KronosVM *vm);
static int handle_op_call_func(KronosVM *vm);
static int handle_op_make_function(KronosVM *vm);
static int handle_op_call_value(KronosVM *vm);
static int handle_op_tuple_new(KronosVM *vm);
static int handle_op_unpack(KronosVM *vm);
static int handle_op_return_val(KronosVM *vm);
static int handle_op_pop(KronosVM *vm);
static int handle_op_list_new(KronosVM *vm);
static int handle_op_range_new(KronosVM *vm);
static int handle_op_list_append(KronosVM *vm);
static int handle_op_map_new(KronosVM *vm);
static int handle_op_map_set(KronosVM *vm);
static int handle_op_list_get(KronosVM *vm);
static int handle_op_list_set(KronosVM *vm);
static int handle_op_delete(KronosVM *vm);
static int handle_op_try_enter(KronosVM *vm);
static int handle_op_try_exit(KronosVM *vm);
static int handle_op_catch(KronosVM *vm);
static int handle_op_finally(KronosVM *vm);
static int handle_op_throw(KronosVM *vm);
static int handle_op_list_len(KronosVM *vm);
static int handle_op_list_slice(KronosVM *vm);
static int handle_op_list_iter(KronosVM *vm);
static int handle_op_list_next(KronosVM *vm);
static int handle_op_import(KronosVM *vm);
static int handle_op_format_value(KronosVM *vm);
static int handle_op_halt(KronosVM *vm);

// Helper function to convert a value to a string representation
// Returns a newly allocated string that the caller must free
static char *value_to_string_repr(const KronosValue *val) {
  if (val->type == VAL_STRING) {
    char *str = malloc(val->as.string.length + 1);
    if (!str)
      return NULL;
    memcpy(str, val->as.string.data, val->as.string.length);
    str[val->as.string.length] = '\0';
    return str;
  } else if (val->type == VAL_NUMBER) {
    char *str_buf = malloc(NUMBER_STRING_BUFFER_SIZE);
    if (!str_buf)
      return NULL;
    double intpart;
    double frac = modf(val->as.number, &intpart);
    size_t len;
    // Use scientific notation for large numbers to prevent buffer overflow
    // (buffer is NUMBER_STRING_BUFFER_SIZE bytes)

    if (frac == 0.0 && fabs(val->as.number) < 1.0e15) {

      len = (size_t)snprintf(str_buf, NUMBER_STRING_BUFFER_SIZE, "%.0f",
                             val->as.number);
    } else {
      len = (size_t)snprintf(str_buf, NUMBER_STRING_BUFFER_SIZE, "%g",
                             val->as.number);
    }
    // Reallocate to exact size
    char *result = realloc(str_buf, len + 1);
    return result ? result : str_buf;
  } else if (val->type == VAL_BOOL) {
    return strdup(val->as.boolean ? "true" : "false");
  } else if (val->type == VAL_NIL) {
    return strdup("null");
  }
  return strdup(""); // Unknown type
}

// Opcode handler implementations
static int handle_op_load_const(KronosVM *vm) {
  KronosValue *constant = read_constant(vm);
  if (!constant) {
    return vm_propagate_error(vm, KRONOS_ERR_INTERNAL);
  }
  PUSH_OR_RETURN_WITH_CLEANUP(vm, constant, (void)0);
  return 0;
}

static int handle_op_load_var(KronosVM *vm) {
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
    return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
  }
  PUSH_OR_RETURN_WITH_CLEANUP(vm, value, (void)0);
  return 0;
}

static int handle_op_store_var(KronosVM *vm) {
  KronosValue *name_val = read_constant(vm);
  if (!name_val) {
    return vm_propagate_error(vm, KRONOS_ERR_INTERNAL);
  }
  if (name_val->type != VAL_STRING) {
    return vm_error(vm, KRONOS_ERR_INTERNAL,
                    "Variable name constant is not a string");
  }
  KronosValue *value;
  POP_OR_RETURN(vm, value);

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
    store_status = vm_set_local(vm, vm->current_frame, name_val->as.string.data,
                                value, is_mutable, type_name);
  } else {
    store_status = vm_set_global(vm, name_val->as.string.data, value,
                                 is_mutable, type_name);
  }

  value_release(value); // Release our reference
  if (store_status != 0) {
    return store_status;
  }
  return 0;
}

static int handle_op_print(KronosVM *vm) {
  KronosValue *value;
  POP_OR_RETURN(vm, value);
  value_fprint(stdout, value);
  printf("\n");
  value_release(value);
  return 0;
}

static int handle_op_debug(KronosVM *vm) {
  uint8_t arg_count = read_byte(vm);
  if (vm->last_error_message) {
    return vm_propagate_error(vm, KRONOS_ERR_INTERNAL);
  }

  KronosValue **values = NULL;
  if (arg_count > 0) {
    values = malloc(sizeof(KronosValue *) * arg_count);
    if (!values) {
      return vm_error(vm, KRONOS_ERR_INTERNAL,
                      "Failed to allocate debug value buffer");
    }

    for (size_t i = arg_count; i > 0; i--) {
      KronosValue *value = pop(vm);
      if (!value) {
        for (size_t j = i; j < arg_count; j++) {
          value_release(values[j]);
        }
        free(values);
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
      values[i - 1] = value;
    }
  }

  fputs("[DEBUG]", stdout);
  for (size_t i = 0; i < arg_count; i++) {
    fputc(' ', stdout);
    value_fprint(stdout, values[i]);
    value_release(values[i]);
  }
  fputc('\n', stdout);
  free(values);
  return 0;
}

static int handle_op_add(KronosVM *vm) {
  KronosValue *b;
  POP_OR_RETURN(vm, b);
  KronosValue *a;
  POP_OR_RETURN_WITH_CLEANUP(vm, a, value_release(b));

  if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
    // Numeric addition
    KronosValue *result = value_new_number(a->as.number + b->as.number);
    PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                                value_release(a); value_release(b););
    value_release(result); // Push retains it
  } else {
    // String concatenation (handles string+string, number+string,
    // string+number) Order matters: left operand first, then right operand
    char *str_a = value_to_string_repr(a);
    char *str_b = value_to_string_repr(b);

    if (!str_a || !str_b) {
      free(str_a);
      free(str_b);
      value_release(a);
      value_release(b);
      return vm_error(vm, KRONOS_ERR_INTERNAL,
                      "Failed to allocate memory for string conversion");
    }

    size_t len_a = strlen(str_a);
    size_t len_b = strlen(str_b);
    size_t total_len = len_a + len_b;

    char *concat = malloc(total_len + 1);
    if (!concat) {
      free(str_a);
      free(str_b);
      value_release(a);
      value_release(b);
      return vm_error(vm, KRONOS_ERR_INTERNAL,
                      "Failed to allocate memory for string concatenation");
    }

    // Concatenate in order: left operand first, then right operand
    memcpy(concat, str_a, len_a);
    memcpy(concat + len_a, str_b, len_b);
    concat[total_len] = '\0';

    KronosValue *result = value_new_string(concat, total_len);
    free(concat);
    free(str_a);
    free(str_b);

    if (!result) {
      value_release(a);
      value_release(b);
      return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string value");
    }

    PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                                value_release(a); value_release(b););
    value_release(result);
  }

  value_release(a);
  value_release(b);
  return 0;
}

static int handle_op_sub(KronosVM *vm) {
  KronosValue *b;

  POP_OR_RETURN(vm, b);
  KronosValue *a;

  POP_OR_RETURN_WITH_CLEANUP(vm, a, value_release(b));

  if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
    KronosValue *result = value_new_number(a->as.number - b->as.number);
    PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                                value_release(a); value_release(b););
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
  return 0;
}

static int handle_op_mul(KronosVM *vm) {
  KronosValue *b;

  POP_OR_RETURN(vm, b);
  KronosValue *a;

  POP_OR_RETURN_WITH_CLEANUP(vm, a, value_release(b));

  if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
    KronosValue *result = value_new_number(a->as.number * b->as.number);
    PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                                value_release(a); value_release(b););
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
  return 0;
}

static int handle_op_div(KronosVM *vm) {
  KronosValue *b;

  POP_OR_RETURN(vm, b);
  KronosValue *a;

  POP_OR_RETURN_WITH_CLEANUP(vm, a, value_release(b));

  if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
    if (b->as.number == 0) {
      int err = vm_error(vm, KRONOS_ERR_RUNTIME, "Cannot divide by zero");
      value_release(a);
      value_release(b);
      return err;
    }
    KronosValue *result = value_new_number(a->as.number / b->as.number);
    PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                                value_release(a); value_release(b););
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
  return 0;
}

static int handle_op_mod(KronosVM *vm) {
  KronosValue *b;

  POP_OR_RETURN(vm, b);
  KronosValue *a;

  POP_OR_RETURN_WITH_CLEANUP(vm, a, value_release(b));

  if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
    if (b->as.number == 0) {
      int err = vm_error(vm, KRONOS_ERR_RUNTIME, "Cannot modulo by zero");
      value_release(a);
      value_release(b);
      return err;
    }
    // Use fmod for floating-point modulo
    KronosValue *result = value_new_number(fmod(a->as.number, b->as.number));
    PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                                value_release(a); value_release(b););
    value_release(result);
  } else {
    int err = vm_error(vm, KRONOS_ERR_RUNTIME,
                       "Cannot modulo - both values must be numbers");
    value_release(a);
    value_release(b);
    return err;
  }

  value_release(a);
  value_release(b);
  return 0;
}

static int handle_op_neg(KronosVM *vm) {
  KronosValue *val;

  POP_OR_RETURN(vm, val);

  if (val->type == VAL_NUMBER) {
    KronosValue *result = value_new_number(-val->as.number);
    PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                                value_release(val););
    value_release(result);
  } else {
    int err = vm_error(vm, KRONOS_ERR_RUNTIME,
                       "Cannot negate - value must be a number");
    value_release(val);
    return err;
  }

  value_release(val);
  return 0;
}

static int handle_op_eq(KronosVM *vm) {
  KronosValue *b;

  POP_OR_RETURN(vm, b);
  KronosValue *a;

  POP_OR_RETURN_WITH_CLEANUP(vm, a, value_release(b));
  bool result = value_equals(a, b);
  KronosValue *res = value_new_bool(result);
  PUSH_OR_RETURN_WITH_CLEANUP(vm, res, value_release(res); value_release(a);
                              value_release(b););
  value_release(res);
  value_release(a);
  value_release(b);
  return 0;
}

static int handle_op_neq(KronosVM *vm) {
  KronosValue *b;

  POP_OR_RETURN(vm, b);
  KronosValue *a;

  POP_OR_RETURN_WITH_CLEANUP(vm, a, value_release(b));
  bool result = !value_equals(a, b);
  KronosValue *res = value_new_bool(result);
  PUSH_OR_RETURN_WITH_CLEANUP(vm, res, value_release(res); value_release(a);
                              value_release(b););
  value_release(res);
  value_release(a);
  value_release(b);
  return 0;
}

static int handle_op_gt(KronosVM *vm) {
  KronosValue *b;

  POP_OR_RETURN(vm, b);
  KronosValue *a;

  POP_OR_RETURN_WITH_CLEANUP(vm, a, value_release(b));

  if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
    bool result = a->as.number > b->as.number;
    KronosValue *res = value_new_bool(result);
    PUSH_OR_RETURN_WITH_CLEANUP(vm, res, value_release(res); value_release(a);
                                value_release(b););
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
  return 0;
}

static int handle_op_lt(KronosVM *vm) {
  KronosValue *b;

  POP_OR_RETURN(vm, b);
  KronosValue *a;

  POP_OR_RETURN_WITH_CLEANUP(vm, a, value_release(b));

  if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
    bool result = a->as.number < b->as.number;
    KronosValue *res = value_new_bool(result);
    PUSH_OR_RETURN_WITH_CLEANUP(vm, res, value_release(res); value_release(a);
                                value_release(b););
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
  return 0;
}

static int handle_op_gte(KronosVM *vm) {
  KronosValue *b;

  POP_OR_RETURN(vm, b);
  KronosValue *a;

  POP_OR_RETURN_WITH_CLEANUP(vm, a, value_release(b));

  if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
    bool result = a->as.number >= b->as.number;
    KronosValue *res = value_new_bool(result);
    PUSH_OR_RETURN_WITH_CLEANUP(vm, res, value_release(res); value_release(a);
                                value_release(b););
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
  return 0;
}

static int handle_op_lte(KronosVM *vm) {
  KronosValue *b;

  POP_OR_RETURN(vm, b);
  KronosValue *a;

  POP_OR_RETURN_WITH_CLEANUP(vm, a, value_release(b));

  if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
    bool result = a->as.number <= b->as.number;
    KronosValue *res = value_new_bool(result);
    PUSH_OR_RETURN_WITH_CLEANUP(vm, res, value_release(res); value_release(a);
                                value_release(b););
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
  return 0;
}

static int handle_op_and(KronosVM *vm) {
  KronosValue *b;

  POP_OR_RETURN(vm, b);
  KronosValue *a;

  POP_OR_RETURN_WITH_CLEANUP(vm, a, value_release(b));

  // Both operands must be truthy for AND to be true
  bool a_truthy = value_is_truthy(a);
  bool b_truthy = value_is_truthy(b);
  bool result = a_truthy && b_truthy;
  KronosValue *res = value_new_bool(result);
  PUSH_OR_RETURN_WITH_CLEANUP(vm, res, value_release(res); value_release(a);
                              value_release(b););
  value_release(res);
  value_release(a);
  value_release(b);
  return 0;
}

static int handle_op_or(KronosVM *vm) {
  KronosValue *b;

  POP_OR_RETURN(vm, b);
  KronosValue *a;

  POP_OR_RETURN_WITH_CLEANUP(vm, a, value_release(b));

  // At least one operand must be truthy for OR to be true
  bool a_truthy = value_is_truthy(a);
  bool b_truthy = value_is_truthy(b);
  bool result = a_truthy || b_truthy;
  KronosValue *res = value_new_bool(result);
  PUSH_OR_RETURN_WITH_CLEANUP(vm, res, value_release(res); value_release(a);
                              value_release(b););
  value_release(res);
  value_release(a);
  value_release(b);
  return 0;
}

static int handle_op_not(KronosVM *vm) {
  KronosValue *a;

  POP_OR_RETURN(vm, a);

  // NOT returns the opposite of the truthiness
  bool a_truthy = value_is_truthy(a);
  bool result = !a_truthy;
  KronosValue *res = value_new_bool(result);
  PUSH_OR_RETURN_WITH_CLEANUP(vm, res, value_release(res); value_release(a););
  value_release(res);
  value_release(a);
  return 0;
}

static int handle_op_jump(KronosVM *vm) {
  int16_t offset = read_int16(vm);
  // Check for error from read_int16
  if (vm->last_error_message) {
    return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
  }
  uint8_t *new_ip = vm->ip + offset;
  // Bounds check: ensure jump target is within valid bytecode range
  if (new_ip < vm->bytecode->code ||
      new_ip >= vm->bytecode->code + vm->bytecode->count) {
    return vm_errorf(
        vm, KRONOS_ERR_RUNTIME,
        "Jump target out of bounds (offset: %d, bytecode size: %zu)", offset,
        vm->bytecode->count);
  }
  vm->ip = new_ip;
  return 0;
}

static int handle_op_jump_if_false(KronosVM *vm) {
  uint16_t offset = read_uint16(vm);
  // Check for error from read_uint16
  if (vm->last_error_message) {
    return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
  }
  KronosValue *condition = peek(vm, 0);
  if (!condition) {
    return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
  }
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
          "Jump target out of bounds (offset: %u, bytecode size: %zu)", offset,
          vm->bytecode->count);
    }
    vm->ip = new_ip;
  }
  KronosValue *condition_val;

  POP_OR_RETURN(vm, condition_val);
  value_release(condition_val); // Pop condition
  return 0;
}

static int handle_op_pop(KronosVM *vm) {
  KronosValue *value;

  POP_OR_RETURN(vm, value);
  value_release(value);
  return 0;
}

static int handle_op_list_new(KronosVM *vm) {
  // Read element count from bytecode
  uint8_t high = read_byte(vm);
  if (vm->last_error_message) {
    return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
  }
  uint8_t low = read_byte(vm);
  if (vm->last_error_message) {
    return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
  }
  uint16_t count = (uint16_t)(high << 8 | low);
  KronosValue *list = value_new_list(count);
  if (!list) {
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create list");
  }
  PUSH_OR_RETURN_WITH_CLEANUP(vm, list, value_release(list););
  value_release(list);
  return 0;
}

/**
 * @brief Format specifier structure
 *
 * Parsed from format strings like ".2f", ">10", "0>5d"
 * Format: [[fill]align][width][.precision][type]
 */
typedef struct {
  char fill_char;  // Fill character (default: ' ')
  char align;      // '<' (left), '>' (right), '^' (center), or '\0' (default)
  int width;       // Minimum width (0 = no minimum)
  int precision;   // For floats: decimal places (-1 = not specified)
  char type;       // 'd' (int), 'f' (float), 's' (string), or '\0' (default)
} FormatSpec;

/**
 * @brief Parse a format specifier string
 *
 * @param spec Format specifier string (e.g., ".2f", ">10", "0>5d")
 * @param len Length of the format specifier string
 * @param out Output FormatSpec structure
 * @return 0 on success, -1 on invalid format
 */
static int parse_format_spec(const char *spec, size_t len, FormatSpec *out) {
  out->fill_char = ' ';
  out->align = '\0';
  out->width = 0;
  out->precision = -1;
  out->type = '\0';

  if (!spec || len == 0) {
    return 0; // Empty spec is valid (default formatting)
  }

  size_t i = 0;

  // Check for fill and align: [[fill]align]
  // If second char is an align char, first char is fill
  if (len >= 2 && (spec[1] == '<' || spec[1] == '>' || spec[1] == '^')) {
    out->fill_char = spec[0];
    out->align = spec[1];
    i = 2;
  } else if (len >= 1 && (spec[0] == '<' || spec[0] == '>' || spec[0] == '^')) {
    out->align = spec[0];
    i = 1;
  }

  // Parse width
  while (i < len && spec[i] >= '0' && spec[i] <= '9') {
    out->width = out->width * 10 + (spec[i] - '0');
    i++;
  }

  // Parse precision (.N)
  if (i < len && spec[i] == '.') {
    i++;
    out->precision = 0;
    while (i < len && spec[i] >= '0' && spec[i] <= '9') {
      out->precision = out->precision * 10 + (spec[i] - '0');
      i++;
    }
  }

  // Parse type (d, f, s)
  if (i < len) {
    char t = spec[i];
    if (t == 'd' || t == 'f' || t == 's') {
      out->type = t;
      i++;
    } else {
      return -1; // Invalid type character
    }
  }

  // Should have consumed entire spec
  if (i != len) {
    return -1; // Extra characters after type
  }

  return 0;
}

/**
 * @brief Apply alignment and padding to a string
 *
 * @param str String to pad
 * @param str_len Length of input string
 * @param spec Format specification
 * @param out_len Output length (set on success)
 * @return Newly allocated padded string, or NULL on error
 */
static char *apply_alignment(const char *str, size_t str_len,
                             const FormatSpec *spec, size_t *out_len) {
  if (spec->width <= 0 || str_len >= (size_t)spec->width) {
    // No padding needed
    char *result = malloc(str_len + 1);
    if (!result) return NULL;
    memcpy(result, str, str_len);
    result[str_len] = '\0';
    *out_len = str_len;
    return result;
  }

  size_t pad_len = (size_t)spec->width - str_len;
  size_t total_len = (size_t)spec->width;
  char *result = malloc(total_len + 1);
  if (!result) return NULL;

  char align = spec->align ? spec->align : '>'; // Default: right-align
  char fill = spec->fill_char;

  if (align == '<') {
    // Left-align: string then padding
    memcpy(result, str, str_len);
    memset(result + str_len, fill, pad_len);
  } else if (align == '>') {
    // Right-align: padding then string
    memset(result, fill, pad_len);
    memcpy(result + pad_len, str, str_len);
  } else if (align == '^') {
    // Center: padding on both sides
    size_t left_pad = pad_len / 2;
    size_t right_pad = pad_len - left_pad;
    memset(result, fill, left_pad);
    memcpy(result + left_pad, str, str_len);
    memset(result + left_pad + str_len, fill, right_pad);
  }

  result[total_len] = '\0';
  *out_len = total_len;
  return result;
}

/**
 * @brief Format a value according to a format specifier
 *
 * @param vm VM instance (for error reporting)
 * @param value Value to format
 * @param spec Format specification
 * @return Newly allocated formatted string value, or NULL on error
 */
static KronosValue *format_value_with_spec(KronosVM *vm, KronosValue *value,
                                           const FormatSpec *spec) {
  char buf[256];
  const char *str = NULL;
  size_t str_len = 0;
  bool free_str = false;

  // Format based on value type and format spec type
  if (value->type == VAL_NUMBER) {
    double num = value->as.number;

    if (spec->type == 'f' || spec->precision >= 0) {
      // Floating-point format
      int prec = (spec->precision >= 0) ? spec->precision : 6;
      int written = snprintf(buf, sizeof(buf), "%.*f", prec, num);
      if (written < 0 || (size_t)written >= sizeof(buf)) {
        vm_error(vm, KRONOS_ERR_RUNTIME, "Number too large to format");
        return NULL;
      }
      str = buf;
      str_len = (size_t)written;
    } else if (spec->type == 'd') {
      // Integer format
      long long int_val = (long long)num;
      int written = snprintf(buf, sizeof(buf), "%lld", int_val);
      if (written < 0 || (size_t)written >= sizeof(buf)) {
        vm_error(vm, KRONOS_ERR_RUNTIME, "Number too large to format");
        return NULL;
      }
      str = buf;
      str_len = (size_t)written;
    } else {
      // Default number format: use %g for cleaner output
      int written = snprintf(buf, sizeof(buf), "%g", num);
      if (written < 0 || (size_t)written >= sizeof(buf)) {
        vm_error(vm, KRONOS_ERR_RUNTIME, "Number too large to format");
        return NULL;
      }
      str = buf;
      str_len = (size_t)written;
    }
  } else if (value->type == VAL_STRING) {
    if (spec->type == 'd' || spec->type == 'f') {
      vm_errorf(vm, KRONOS_ERR_RUNTIME,
                "Cannot use numeric format '%%%c' with string value",
                spec->type);
      return NULL;
    }
    str = value->as.string.data;
    str_len = value->as.string.length;

    // Apply precision to strings (max length)
    if (spec->precision >= 0 && str_len > (size_t)spec->precision) {
      str_len = (size_t)spec->precision;
    }
  } else if (value->type == VAL_BOOL) {
    str = value->as.boolean ? "true" : "false";
    str_len = strlen(str);
  } else if (value->type == VAL_NIL) {
    str = "null";
    str_len = 4;
  } else {
    // Other types (list, map, range, etc.): convert to string representation
    char *str_repr = value_to_string_repr(value);
    if (!str_repr) {
      vm_error(vm, KRONOS_ERR_RUNTIME, "Failed to convert value to string");
      return NULL;
    }
    str = str_repr;
    str_len = strlen(str_repr);
    free_str = true;
  }

  // Apply alignment and width
  size_t result_len = 0;
  char *result_str = apply_alignment(str, str_len, spec, &result_len);

  if (free_str) {
    free((void *)str);
  }

  if (!result_str) {
    vm_error(vm, KRONOS_ERR_RUNTIME, "Memory allocation failed during formatting");
    return NULL;
  }

  KronosValue *result = value_new_string(result_str, result_len);
  free(result_str);

  if (!result) {
    vm_error(vm, KRONOS_ERR_RUNTIME, "Failed to create formatted string value");
    return NULL;
  }

  return result;
}

static int handle_op_format_value(KronosVM *vm) {
  // Read format spec constant index
  uint16_t spec_idx = read_uint16(vm);
  if (vm->last_error_message) {
    return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
  }

  // Get format spec string from constant pool
  if (!vm->bytecode || spec_idx >= vm->bytecode->const_count) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Invalid format spec constant index: %u", spec_idx);
  }

  KronosValue *spec_val = vm->bytecode->constants[spec_idx];
  if (!spec_val || spec_val->type != VAL_STRING) {
    return vm_error(vm, KRONOS_ERR_INTERNAL,
                    "Format spec constant must be a string");
  }

  // Parse format spec
  FormatSpec spec;
  if (parse_format_spec(spec_val->as.string.data, spec_val->as.string.length,
                        &spec) < 0) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME, "Invalid format specifier: %s",
                     spec_val->as.string.data);
  }

  // Pop value to format
  KronosValue *value;
  POP_OR_RETURN(vm, value);

  // Format the value
  KronosValue *result = format_value_with_spec(vm, value, &spec);
  value_release(value);

  if (!result) {
    return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
  }

  // Push result
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result););
  value_release(result);
  return 0;
}

static int handle_op_halt(KronosVM *vm) {
  (void)vm; // Unused parameter
  return 0;
}

static int handle_op_call_func(KronosVM *vm) {
  KronosValue *name_val = read_constant(vm);
  if (!name_val) {
    return vm_propagate_error(vm, KRONOS_ERR_INTERNAL);
  }
  if (name_val->type != VAL_STRING) {
    return vm_error(vm, KRONOS_ERR_INTERNAL,
                    "Function name constant is not a string");
  }
  uint8_t arg_count = read_byte(vm);
  uint8_t named_count = read_byte(vm);

  // Read named argument info if present
  // named_args: array of (arg_index, param_name) pairs
  struct { uint8_t arg_idx; char *param_name; } *named_args = NULL;
  if (named_count > 0) {
    named_args = malloc(sizeof(*named_args) * named_count);
    if (!named_args) {
      return vm_error(vm, KRONOS_ERR_INTERNAL,
                      "Failed to allocate named argument info");
    }
    for (uint8_t i = 0; i < named_count; i++) {
      named_args[i].arg_idx = read_byte(vm);
      KronosValue *pname = read_constant(vm);
      if (!pname || pname->type != VAL_STRING) {
        for (uint8_t j = 0; j < i; j++) {
          free(named_args[j].param_name);
        }
        free(named_args);
        return vm_error(vm, KRONOS_ERR_INTERNAL,
                        "Invalid named argument info in bytecode");
      }
      named_args[i].param_name = strdup(pname->as.string.data);
      if (!named_args[i].param_name) {
        for (uint8_t j = 0; j < i; j++) {
          free(named_args[j].param_name);
        }
        free(named_args);
        return vm_error(vm, KRONOS_ERR_INTERNAL,
                        "Failed to allocate named argument parameter name");
      }
    }
  }

  // Helper macro to free named_args
  #define FREE_NAMED_ARGS() do { \
    if (named_args) { \
      for (uint8_t _i = 0; _i < named_count; _i++) { \
        free(named_args[_i].param_name); \
      } \
      free(named_args); \
    } \
  } while(0)

  // Check for built-in functions first
  const char *func_name = name_val->as.string.data;

  // Check for module.function syntax (e.g., math.sqrt)
  const char *dot = strchr(func_name, '.');
  if (dot) {
    // Split module and function name
    size_t module_len = (size_t)(dot - func_name);
    char *module_name = malloc(module_len + 1);
    if (!module_name) {
      FREE_NAMED_ARGS();
      return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate memory");
    }
    strncpy(module_name, func_name, module_len);
    module_name[module_len] = '\0';

    const char *actual_func_name = dot + 1;

    // Check for built-in modules first
    if (strcmp(module_name, "math") == 0) {
      // Math functions are already implemented as built-ins
      // Just route to the built-in function by name
      free(module_name);
      // Continue to built-in function checks below with actual_func_name
      func_name = actual_func_name;
    } else if (strcmp(module_name, "regex") == 0) {
      // Regex functions are implemented as built-ins
      free(module_name);
      // Continue to built-in function checks below with actual_func_name
      func_name = actual_func_name;
    } else {
      // Check for loaded file-based modules
      Module *mod = vm_get_module(vm, module_name);
      if (mod && mod->is_loaded && mod->module_vm) {
        // Look up function in module's VM
        Function *mod_func = vm_get_function(mod->module_vm, actual_func_name);

        if (!mod_func) {
          int err = vm_errorf(vm, KRONOS_ERR_NOT_FOUND,
                              "Function '%s' not found in module '%s'",
                              actual_func_name, module_name);
          free(module_name);
          FREE_NAMED_ARGS();
          return err;
        }

        // Check parameter count (named args not supported for module functions)
        if (named_count > 0) {
          int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Named arguments not supported for module function '%s.%s'",
                        module_name, actual_func_name);
          free(module_name);
          FREE_NAMED_ARGS();
          return err;
        }
        if (arg_count != (uint8_t)mod_func->param_count) {
          int err =
              vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function '%s.%s' expects %zu argument%s, but got %d",
                        module_name, actual_func_name, mod_func->param_count,
                        mod_func->param_count == 1 ? "" : "s", arg_count);
          free(module_name);
          FREE_NAMED_ARGS();
          return err;
        }

        // Pop arguments from current VM
        KronosValue **args = NULL;
        if (arg_count > 0) {
          args = malloc(sizeof(KronosValue *) * arg_count);
          if (!args) {
            free(module_name);
            FREE_NAMED_ARGS();
            return vm_error(vm, KRONOS_ERR_INTERNAL,
                            "Failed to allocate argument buffer");
          }

          for (int i = arg_count - 1; i >= 0; i--) {
            args[i] = pop(vm);
            if (!args[i]) {
              for (int j = i + 1; j < arg_count; j++) {
                value_release(args[j]);
              }
              free(args);
              free(module_name);
              FREE_NAMED_ARGS();
              return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
            }
          }
        }

        // Call the module function using helper
        int result = call_module_function(vm, mod, mod_func, args, arg_count);
        free(args);
        free(module_name);
        FREE_NAMED_ARGS();

        if (result < 0) {
          return result;
        }

        return 0; // Function call completed
      } else {
        int err = vm_errorf(vm, KRONOS_ERR_NOT_FOUND, "Unknown module '%s'",
                            module_name);
        free(module_name);
        FREE_NAMED_ARGS();
        return err;
      }
    }
  }

  // Try to find built-in function using dispatch table
  BuiltinHandler builtin = vm_find_builtin(func_name);
  if (builtin) {
    // Named arguments not supported for built-in functions
    if (named_count > 0) {
      FREE_NAMED_ARGS();
      return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                       "Named arguments not supported for built-in function '%s'",
                       func_name);
    }
    FREE_NAMED_ARGS();
    return builtin(vm, arg_count);
  }

  // Try variable containing a function value (lambda)
  KronosValue *var_val = vm_get_variable(vm, func_name);
  if (var_val && var_val->type == VAL_FUNCTION) {
    // Named arguments not yet supported for lambda calls
    if (named_count > 0) {
      FREE_NAMED_ARGS();
      return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                       "Named arguments not yet supported for lambda calls");
    }
    FREE_NAMED_ARGS();
    // Call the function value
    return vm_call_function_value(vm, var_val, func_name, arg_count);
  }
  // If vm_get_variable set an error (variable not found), clear it because
  // we're going to try looking up a named function instead
  if (!var_val) {
    vm_clear_error(vm);
  }

  // Try user-defined function
  Function *func = vm_get_function(vm, func_name);
  if (!func) {
    FREE_NAMED_ARGS();
    return vm_errorf(vm, KRONOS_ERR_NOT_FOUND, "Undefined function '%s'",
                     func_name);
  }

  // Calculate the number of non-variadic parameters
  size_t regular_param_count = func->param_count - (func->has_variadic ? 1 : 0);

  // Build parameter name to index mapping for named argument resolution
  // This maps from arg array position to parameter index
  int *arg_to_param_map = NULL;
  if (named_count > 0) {
    arg_to_param_map = malloc(sizeof(int) * arg_count);
    if (!arg_to_param_map) {
      FREE_NAMED_ARGS();
      return vm_error(vm, KRONOS_ERR_INTERNAL,
                      "Failed to allocate argument mapping");
    }
    // Initialize: positional args map directly
    for (uint8_t i = 0; i < arg_count; i++) {
      arg_to_param_map[i] = i; // Default: positional mapping
    }
    // Override with named argument mappings
    for (uint8_t i = 0; i < named_count; i++) {
      uint8_t arg_idx = named_args[i].arg_idx;
      const char *pname = named_args[i].param_name;
      // Find parameter index by name
      int param_idx = -1;
      for (size_t j = 0; j < regular_param_count; j++) {
        if (strcmp(func->params[j], pname) == 0) {
          param_idx = (int)j;
          break;
        }
      }
      if (param_idx < 0) {
        free(arg_to_param_map);
        FREE_NAMED_ARGS();
        return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                         "Function '%s' has no parameter named '%s'",
                         func->name, pname);
      }
      arg_to_param_map[arg_idx] = param_idx;
    }
  }

  // Helper macro to cleanup the arg_to_param_map
  #define FREE_ARG_MAP() do { free(arg_to_param_map); } while(0)

  // Track which parameters are covered by arguments
  // (for validating required params are satisfied)
  bool *param_covered = NULL;
  if (named_count > 0) {
    param_covered = calloc(regular_param_count, sizeof(bool));
    if (!param_covered) {
      FREE_ARG_MAP();
      FREE_NAMED_ARGS();
      return vm_error(vm, KRONOS_ERR_INTERNAL,
                      "Failed to allocate parameter tracking");
    }
    for (uint8_t i = 0; i < arg_count && i < regular_param_count; i++) {
      int param_idx = arg_to_param_map[i];
      if (param_idx >= 0 && (size_t)param_idx < regular_param_count) {
        param_covered[param_idx] = true;
      }
    }
    // Check that all required parameters are covered
    for (size_t i = 0; i < func->required_param_count; i++) {
      if (!param_covered[i]) {
        free(param_covered);
        FREE_ARG_MAP();
        FREE_NAMED_ARGS();
        return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                         "Function '%s' missing required argument '%s'",
                         func->name, func->params[i]);
      }
    }
    free(param_covered);
  } else {
    // No named args - use simple count validation
    if (arg_count < func->required_param_count) {
      FREE_NAMED_ARGS();
      return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                       "Function '%s' requires at least %zu argument%s, but got %d",
                       func->name, func->required_param_count,
                       func->required_param_count == 1 ? "" : "s", arg_count);
    }
  }

  // Validate max argument count (for non-variadic functions)
  if (!func->has_variadic && arg_count > func->param_count) {
    FREE_ARG_MAP();
    FREE_NAMED_ARGS();
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function '%s' accepts at most %zu argument%s, but got %d",
                     func->name, func->param_count,
                     func->param_count == 1 ? "" : "s", arg_count);
  }

  // Check call stack size
  if (vm->call_stack_size >= CALL_STACK_MAX) {
    FREE_ARG_MAP();
    FREE_NAMED_ARGS();
    return vm_error(vm, KRONOS_ERR_RUNTIME, "Maximum call depth exceeded");
  }

  // Create new call frame
  CallFrame *frame = &vm->call_stack[vm->call_stack_size++];
  frame->function = func;
  frame->return_ip = vm->ip;
  frame->return_bytecode = vm->bytecode;
  frame->frame_start = vm->stack_top;
  frame->local_count = 0;
  frame->owned_bytecode = NULL;
  // Initialize local variable hash table to all NULL
  for (size_t i = 0; i < LOCALS_MAX; i++) {
    frame->local_hash[i] = NULL;
  }

  // Validate stack has enough arguments before popping
  // Check both stack size and that stack_top is valid
  if (vm->stack_top < vm->stack) {
    vm->call_stack_size--;
    if (vm->call_stack_size > 0) {
      vm->current_frame = &vm->call_stack[vm->call_stack_size - 1];
    } else {
      vm->current_frame = NULL;
    }
    FREE_ARG_MAP();
    FREE_NAMED_ARGS();
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Stack pointer corruption: stack_top (%p) < stack (%p)",
                     (void *)vm->stack_top, (void *)vm->stack);
  }

  size_t stack_size = vm->stack_top - vm->stack;

  if (stack_size < arg_count) {
    vm->call_stack_size--;
    if (vm->call_stack_size > 0) {
      vm->current_frame = &vm->call_stack[vm->call_stack_size - 1];
    } else {
      vm->current_frame = NULL;
    }
    FREE_ARG_MAP();
    FREE_NAMED_ARGS();
    return vm_errorf(
        vm, KRONOS_ERR_RUNTIME,
        "Stack underflow: function '%s' expects %d argument%s, but "
        "only %zu value%s on stack",
        func_name, arg_count, arg_count == 1 ? "" : "s", stack_size,
        stack_size == 1 ? "" : "s");
  }

  // Pop arguments and bind to parameters (in reverse order)
  KronosValue **args =
      arg_count > 0 ? malloc(sizeof(KronosValue *) * arg_count) : NULL;
  if (arg_count > 0 && !args) {
    // Allocation failure: restore VM state and abort call setup
    // Decrement call stack size to undo the increment above
    vm->call_stack_size--;
    if (vm->call_stack_size > 0) {
      vm->current_frame = &vm->call_stack[vm->call_stack_size - 1];
    } else {
      vm->current_frame = NULL;
    }
    FREE_ARG_MAP();
    FREE_NAMED_ARGS();
    return vm_error(vm, KRONOS_ERR_INTERNAL,
                    "Failed to allocate argument buffer");
  }
  for (int i = arg_count - 1; i >= 0; i--) {
    // Double-check stack before each pop
    if (vm->stack_top <= vm->stack) {
      // Free already-popped arguments
      for (size_t j = i + 1; j < arg_count; j++) {
        value_release(args[j]);
      }
      free(args);
      vm->call_stack_size--;
      if (vm->call_stack_size > 0) {
        vm->current_frame = &vm->call_stack[vm->call_stack_size - 1];
      } else {
        vm->current_frame = NULL;
      }
      FREE_ARG_MAP();
      FREE_NAMED_ARGS();
      return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                       "Stack underflow during pop: function '%s', "
                       "expected %d args, popped %d, stack_size=%zu",
                       func_name, arg_count, (int)(arg_count - i - 1),
                       (size_t)(vm->stack_top - vm->stack));
    }
    args[i] = pop(vm);
    if (!args[i]) {
      // Free already-popped arguments
      for (size_t j = i + 1; j < arg_count; j++) {
        value_release(args[j]);
      }
      free(args);
      vm->call_stack_size--;
      if (vm->call_stack_size > 0) {
        vm->current_frame = &vm->call_stack[vm->call_stack_size - 1];
      } else {
        vm->current_frame = NULL;
      }
      FREE_ARG_MAP();
      FREE_NAMED_ARGS();
      return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
    }
  }

  // Set current frame before setting locals
  vm->current_frame = frame;

  // Helper macro for cleanup on error (includes named args cleanup)
  #define CLEANUP_CALL_FRAME() do { \
    for (size_t j = 0; j < frame->local_count; j++) { \
      free(frame->locals[j].name); \
      value_release(frame->locals[j].value); \
      free(frame->locals[j].type_name); \
    } \
    frame->local_count = 0; \
    vm->call_stack_size--; \
    if (vm->call_stack_size > 0) { \
      vm->current_frame = &vm->call_stack[vm->call_stack_size - 1]; \
    } else { \
      vm->current_frame = NULL; \
    } \
    FREE_ARG_MAP(); \
    FREE_NAMED_ARGS(); \
  } while(0)

  // Track which parameters have been bound (for named argument support)
  bool *param_bound = calloc(regular_param_count > 0 ? regular_param_count : 1, sizeof(bool));
  if (!param_bound) {
    for (size_t j = 0; j < arg_count; j++) {
      value_release(args[j]);
    }
    free(args);
    CLEANUP_CALL_FRAME();
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate param tracking");
  }

  // Calculate how many regular arguments were provided vs how many go to variadic
  size_t regular_args_provided = func->has_variadic ?
      (arg_count > regular_param_count ? regular_param_count : arg_count) :
      arg_count;

  // Bind arguments to parameters (respecting named argument mapping)
  for (size_t i = 0; i < regular_args_provided; i++) {
    // Determine target parameter index
    size_t param_idx = (arg_to_param_map && i < arg_count) ?
                       (size_t)arg_to_param_map[i] : i;

    // Check for duplicate binding (same parameter bound twice)
    if (param_idx < regular_param_count && param_bound[param_idx]) {
      for (size_t j = 0; j < arg_count; j++) {
        value_release(args[j]);
      }
      free(args);
      free(param_bound);
      CLEANUP_CALL_FRAME();
      return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                       "Duplicate argument for parameter '%s'",
                       func->params[param_idx]);
    }

    if (param_idx < regular_param_count) {
      param_bound[param_idx] = true;
      KronosValue *arg_val = args[i];
      value_retain(arg_val);

      int arg_status = vm_set_local(vm, frame, func->params[param_idx], arg_val, true, NULL);
      value_release(arg_val);
      if (arg_status != 0) {
        for (size_t j = 0; j < arg_count; j++) {
          value_release(args[j]);
        }
        free(args);
        free(param_bound);
        CLEANUP_CALL_FRAME();
        return arg_status;
      }
    }
  }

  // Fill in unbound parameters with default values
  for (size_t i = 0; i < regular_param_count; i++) {
    if (!param_bound[i]) {
      KronosValue *arg_val;
      if (func->param_defaults && func->param_defaults[i]) {
        arg_val = func->param_defaults[i];
        value_retain(arg_val);
      } else {
        // No default - this shouldn't happen if validation is correct
        for (size_t j = 0; j < arg_count; j++) {
          value_release(args[j]);
        }
        free(args);
        free(param_bound);
        CLEANUP_CALL_FRAME();
        return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                         "Missing required argument for parameter '%s'",
                         func->params[i]);
      }

      int arg_status = vm_set_local(vm, frame, func->params[i], arg_val, true, NULL);
      value_release(arg_val);
      if (arg_status != 0) {
        for (size_t j = 0; j < arg_count; j++) {
          value_release(args[j]);
        }
        free(args);
        free(param_bound);
        CLEANUP_CALL_FRAME();
        return arg_status;
      }
    }
  }

  free(param_bound);

  // Handle variadic parameter - collect remaining arguments into a list
  if (func->has_variadic) {
    size_t variadic_idx = func->param_count - 1;
    size_t variadic_count = arg_count > regular_param_count ?
        arg_count - regular_param_count : 0;

    // Create list for variadic arguments
    KronosValue *variadic_list = value_new_list(variadic_count > 0 ? variadic_count : 4);
    if (!variadic_list) {
      for (size_t j = 0; j < arg_count; j++) {
        value_release(args[j]);
      }
      free(args);
      CLEANUP_CALL_FRAME();
      return vm_error(vm, KRONOS_ERR_INTERNAL,
                      "Failed to create variadic argument list");
    }

    // Add variadic arguments to the list
    for (size_t i = 0; i < variadic_count; i++) {
      size_t arg_idx = regular_param_count + i;

      // Grow list if needed
      if (variadic_list->as.list.count >= variadic_list->as.list.capacity) {
        size_t new_capacity = variadic_list->as.list.capacity == 0 ? 4 :
                              variadic_list->as.list.capacity * 2;
        KronosValue **new_items = realloc(variadic_list->as.list.items,
                                          sizeof(KronosValue *) * new_capacity);
        if (!new_items) {
          value_release(variadic_list);
          for (size_t j = 0; j < arg_count; j++) {
            value_release(args[j]);
          }
          free(args);
          CLEANUP_CALL_FRAME();
          return vm_error(vm, KRONOS_ERR_INTERNAL,
                          "Failed to grow variadic argument list");
        }
        variadic_list->as.list.items = new_items;
        variadic_list->as.list.capacity = new_capacity;
      }

      // Append value to list
      value_retain(args[arg_idx]);
      variadic_list->as.list.items[variadic_list->as.list.count++] = args[arg_idx];
    }

    // Bind variadic list to parameter
    int arg_status = vm_set_local(vm, frame, func->params[variadic_idx],
                                  variadic_list, true, NULL);
    value_release(variadic_list);
    if (arg_status != 0) {
      for (size_t j = 0; j < arg_count; j++) {
        value_release(args[j]);
      }
      free(args);
      CLEANUP_CALL_FRAME();
      return arg_status;
    }
  }

  // Release original argument references
  for (size_t i = 0; i < arg_count; i++) {
    value_release(args[i]);
  }
  free(args);

  // Clean up named argument resources (no longer needed after binding)
  FREE_ARG_MAP();
  FREE_NAMED_ARGS();

  #undef CLEANUP_CALL_FRAME
  #undef FREE_ARG_MAP
  #undef FREE_NAMED_ARGS

  // Validate function bytecode before switching to it
  if (!func->bytecode.code) {
    vm->call_stack_size--;
    if (vm->call_stack_size > 0) {
      vm->current_frame = &vm->call_stack[vm->call_stack_size - 1];
    } else {
      vm->current_frame = NULL;
    }
    return vm_error(vm, KRONOS_ERR_INTERNAL,
                    "Function bytecode is NULL (internal error)");
  }

  // Switch to function bytecode
  vm->bytecode = &func->bytecode;
  vm->ip = func->bytecode.code;

  return 0;
}

static int handle_op_range_new(KronosVM *vm) {
  // Stack: [start, end, step]
  // Pop step, end, start and create range
  KronosValue *step_val;

  POP_OR_RETURN(vm, step_val);
  KronosValue *end_val;

  POP_OR_RETURN_WITH_CLEANUP(vm, end_val, value_release(step_val));
  KronosValue *start_val;
  POP_OR_RETURN_WITH_CLEANUP(vm, start_val, value_release(step_val);
                             value_release(end_val));

  // All must be numbers
  if (start_val->type != VAL_NUMBER || end_val->type != VAL_NUMBER ||
      step_val->type != VAL_NUMBER) {
    value_release(start_val);
    value_release(end_val);
    value_release(step_val);
    return vm_error(vm, KRONOS_ERR_RUNTIME,
                    "Range start, end, and step must be numbers");
  }

  KronosValue *range = value_new_range(start_val->as.number, end_val->as.number,
                                       step_val->as.number);
  if (!range) {
    value_release(start_val);
    value_release(end_val);
    value_release(step_val);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create range");
  }

  PUSH_OR_RETURN_WITH_CLEANUP(vm, range, value_release(range);
                              value_release(start_val); value_release(end_val);
                              value_release(step_val););
  value_release(range);
  value_release(start_val);
  value_release(end_val);
  value_release(step_val);
  return 0;
}

static int handle_op_list_append(KronosVM *vm) {
  KronosValue *value;

  POP_OR_RETURN(vm, value);
  KronosValue *list;

  POP_OR_RETURN_WITH_CLEANUP(vm, list, value_release(value));

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

  // Push first (retains the list), then release our popped reference
  // Note: cleanup only releases list because value is now owned by list
  PUSH_OR_RETURN_WITH_CLEANUP(vm, list, value_release(list););
  value_release(list);

  value_release(value);
  return 0;
}

static int handle_op_map_new(KronosVM *vm) {
  // Read entry count from bytecode (unused, but kept for consistency)
  uint8_t high = read_byte(vm);
  if (vm->last_error_message) {
    return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
  }
  uint8_t low = read_byte(vm);
  if (vm->last_error_message) {
    return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
  }
  uint16_t count = (uint16_t)(high << 8 | low);
  (void)count; // Unused, maps grow dynamically
  KronosValue *map = value_new_map(0);
  if (!map) {
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create map");
  }
  PUSH_OR_RETURN_WITH_CLEANUP(vm, map, value_release(map););
  value_release(map);
  return 0;
}

static int handle_op_map_set(KronosVM *vm) {
  // Stack: [map, key, value]
  KronosValue *value;

  POP_OR_RETURN(vm, value);
  KronosValue *key;

  POP_OR_RETURN_WITH_CLEANUP(vm, key, value_release(value));
  KronosValue *map;
  POP_OR_RETURN_WITH_CLEANUP(vm, map, value_release(key); value_release(value));

  if (map->type != VAL_MAP) {
    value_release(key);
    value_release(value);
    value_release(map);
    return vm_error(vm, KRONOS_ERR_RUNTIME,
                    "Expected map for map set operation");
  }

  int result = map_set(map, key, value);
  value_release(key);
  value_release(value);
  if (result != 0) {
    value_release(map);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to set map entry");
  }

  PUSH_OR_RETURN_WITH_CLEANUP(vm, map, value_release(map););
  value_release(map);
  return 0;
}

static int handle_op_list_get(KronosVM *vm) {
  KronosValue *index_val;

  POP_OR_RETURN(vm, index_val);
  KronosValue *container;

  POP_OR_RETURN_WITH_CLEANUP(vm, container, value_release(index_val));

  // Handle maps first (they accept any key type)
  if (container->type == VAL_MAP) {
    KronosValue *value = map_get(container, index_val);
    if (!value) {
      value_release(index_val);
      value_release(container);
      return vm_error(vm, KRONOS_ERR_RUNTIME, "Map key not found");
    }
    value_retain(value);
    PUSH_OR_RETURN_WITH_CLEANUP(vm, value, value_release(value);
                                value_release(index_val);
                                value_release(container););
    value_release(value);
    value_release(index_val);
    value_release(container);
    return 0;
  }

  // For lists, strings, and ranges, index must be a number
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
    PUSH_OR_RETURN_WITH_CLEANUP(vm, item, value_release(item);
                                value_release(index_val);
                                value_release(container););
    value_release(item);
  } else if (container->type == VAL_RANGE) {
    // Calculate value at index: start + (index * step)
    double start = container->as.range.start;
    double step = container->as.range.step;
    double end = container->as.range.end;

    // Handle negative indices by calculating range length
    if (idx < 0) {
      if (step == 0.0) {
        value_release(index_val);
        value_release(container);
        return vm_error(vm, KRONOS_ERR_RUNTIME, "Range step cannot be zero");
      }
      double diff = end - start;
      double count = floor((diff / step)) + 1.0;
      if (count < 0)
        count = 0;
      idx = (int64_t)count + idx;
    }

    // Calculate the value at this index
    double value = start + (idx * step);

    // Check bounds based on step direction
    bool in_bounds = false;
    if (step > 0) {
      in_bounds = (value >= start && value <= end && idx >= 0);
    } else if (step < 0) {
      in_bounds = (value <= start && value >= end && idx >= 0);
    } else {
      // step == 0 is invalid, but we handle it
      in_bounds = (idx == 0);
    }

    if (!in_bounds) {
      value_release(index_val);
      value_release(container);
      return vm_error(vm, KRONOS_ERR_RUNTIME, "Range index out of bounds");
    }

    KronosValue *result = value_new_number(value);
    if (!result) {
      value_release(index_val);
      value_release(container);
      return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create number");
    }
    PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                                value_release(index_val);
                                value_release(container););
    value_release(result);
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
    PUSH_OR_RETURN_WITH_CLEANUP(vm, char_str, value_release(char_str);
                                value_release(index_val);
                                value_release(container););
    value_release(char_str);
  } else {
    // Note: Maps are handled earlier in this function with an early return
    value_release(index_val);
    value_release(container);
    return vm_error(
        vm, KRONOS_ERR_RUNTIME,
        "Indexing only supported for lists, strings, ranges, and maps");
  }

  value_release(index_val);
  value_release(container);
  return 0;
}

static int handle_op_list_set(KronosVM *vm) {
  // Stack: [list, index, value]
  KronosValue *value;

  POP_OR_RETURN(vm, value);
  KronosValue *index_val;

  POP_OR_RETURN_WITH_CLEANUP(vm, index_val, value_release(value));
  KronosValue *list;
  POP_OR_RETURN_WITH_CLEANUP(vm, list, value_release(index_val);
                             value_release(value));

  if (list->type != VAL_LIST) {
    value_release(index_val);
    value_release(value);
    value_release(list);
    return vm_error(vm, KRONOS_ERR_RUNTIME,
                    "Expected list for index assignment");
  }

  if (index_val->type != VAL_NUMBER) {
    value_release(index_val);
    value_release(value);
    value_release(list);
    return vm_error(vm, KRONOS_ERR_RUNTIME, "Index must be a number");
  }

  // Handle negative indices
  int64_t idx = (int64_t)index_val->as.number;
  if (idx < 0) {
    idx = (int64_t)list->as.list.count + idx;
  }

  if (idx < 0 || (size_t)idx >= list->as.list.count) {
    value_release(index_val);
    value_release(value);
    value_release(list);
    return vm_error(vm, KRONOS_ERR_RUNTIME, "List index out of bounds");
  }

  // Release old value and set new value
  value_release(list->as.list.items[(size_t)idx]);
  value_retain(value);
  list->as.list.items[(size_t)idx] = value;

  // Push list back
  PUSH_OR_RETURN_WITH_CLEANUP(vm, list, value_release(list);
                              value_release(index_val); value_release(value););
  value_release(list);
  value_release(index_val);
  value_release(value);
  return 0;
}

static int handle_op_delete(KronosVM *vm) {
  // Stack: [map, key]
  KronosValue *key;

  POP_OR_RETURN(vm, key);
  KronosValue *map;

  POP_OR_RETURN_WITH_CLEANUP(vm, map, value_release(key));

  if (map->type != VAL_MAP) {
    value_release(key);
    value_release(map);
    return vm_error(vm, KRONOS_ERR_RUNTIME,
                    "Expected map for delete operation");
  }

  bool deleted = map_delete(map, key);
  value_release(key);
  if (!deleted) {
    value_release(map);
    return vm_error(vm, KRONOS_ERR_RUNTIME, "Map key not found");
  }

  // Push map back
  PUSH_OR_RETURN_WITH_CLEANUP(vm, map, value_release(map););
  value_release(map);
  return 0;
}

static int handle_op_list_len(KronosVM *vm) {
  KronosValue *container;

  POP_OR_RETURN(vm, container);

  if (container->type == VAL_LIST) {
    KronosValue *len = value_new_number((double)container->as.list.count);
    PUSH_OR_RETURN_WITH_CLEANUP(vm, len, value_release(len);
                                value_release(container););
    value_release(len);
  } else if (container->type == VAL_STRING) {
    KronosValue *len = value_new_number((double)container->as.string.length);
    PUSH_OR_RETURN_WITH_CLEANUP(vm, len, value_release(len);
                                value_release(container););
    value_release(len);
  } else if (container->type == VAL_RANGE) {
    // Calculate range length: number of values in range
    double start = container->as.range.start;
    double end = container->as.range.end;
    double step = container->as.range.step;

    if (step == 0.0) {
      value_release(container);
      return vm_error(vm, KRONOS_ERR_RUNTIME, "Range step cannot be zero");
    }

    // Calculate number of steps
    double diff = end - start;
    double count = floor((diff / step)) + 1.0;

    // Ensure count is non-negative
    if (count < 0) {
      count = 0;
    }

    KronosValue *len = value_new_number(count);
    PUSH_OR_RETURN_WITH_CLEANUP(vm, len, value_release(len);
                                value_release(container););
    value_release(len);
  } else {
    value_release(container);
    return vm_error(vm, KRONOS_ERR_RUNTIME,
                    "Expected list, string, or range for length");
  }

  value_release(container);
  return 0;
}

static int handle_op_list_slice(KronosVM *vm) {
  KronosValue *end_val;

  POP_OR_RETURN(vm, end_val);
  KronosValue *start_val;

  POP_OR_RETURN_WITH_CLEANUP(vm, start_val, value_release(end_val));
  KronosValue *container;
  POP_OR_RETURN_WITH_CLEANUP(vm, container, value_release(start_val);
                             value_release(end_val));

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
    if (start < 0)
      start = 0;
    if (end < 0)
      end = 0;
    if ((size_t)start > len)
      start = (int64_t)len;
    if ((size_t)end > len)
      end = (int64_t)len;
    if (start > end)
      start = end;

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

    PUSH_OR_RETURN_WITH_CLEANUP(
        vm, slice, value_release(slice); value_release(container);
        value_release(start_val); value_release(end_val););
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
    if (start < 0)
      start = 0;
    if (end < 0)
      end = 0;
    if ((size_t)start > len)
      start = (int64_t)len;
    if ((size_t)end > len)
      end = (int64_t)len;
    if (start > end)
      start = end;

    // Create new string with sliced characters
    size_t slice_len = (size_t)(end - start);
    char *slice_data = malloc(slice_len + 1);
    if (!slice_data) {
      value_release(container);
      value_release(start_val);
      value_release(end_val);
      return vm_error(vm, KRONOS_ERR_INTERNAL,
                      "Failed to allocate memory for string slice");
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

    PUSH_OR_RETURN_WITH_CLEANUP(
        vm, slice, value_release(slice); value_release(container);
        value_release(start_val); value_release(end_val););
    value_release(slice);
  } else if (container->type == VAL_RANGE) {
    // Range slicing: create a new range with adjusted start/end
    double orig_start = container->as.range.start;
    double orig_end = container->as.range.end;
    double step = container->as.range.step;

    // Calculate actual start and end values at these indices
    double new_start = orig_start + (start * step);
    double new_end = orig_start + (end * step);

    // For negative end, use original end
    if (end_val->as.number == -1.0) {
      new_end = orig_end;
    }

    // Ensure bounds are valid
    if (step > 0) {
      if (new_start < orig_start)
        new_start = orig_start;
      if (new_end > orig_end)
        new_end = orig_end;
      if (new_start > new_end) {
        // Empty range
        new_end = new_start;
      }
    } else if (step < 0) {
      if (new_start > orig_start)
        new_start = orig_start;
      if (new_end < orig_end)
        new_end = orig_end;
      if (new_start < new_end) {
        // Empty range
        new_end = new_start;
      }
    }

    KronosValue *slice = value_new_range(new_start, new_end, step);
    if (!slice) {
      value_release(container);
      value_release(start_val);
      value_release(end_val);
      return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create range");
    }

    PUSH_OR_RETURN_WITH_CLEANUP(
        vm, slice, value_release(slice); value_release(container);
        value_release(start_val); value_release(end_val););
    value_release(slice);
  } else {
    value_release(container);
    value_release(start_val);
    value_release(end_val);
    return vm_error(vm, KRONOS_ERR_RUNTIME,
                    "Slicing only supported for lists, strings, and ranges");
  }

  value_release(container);
  value_release(start_val);
  value_release(end_val);
  return 0;
}

static int handle_op_list_iter(KronosVM *vm) {
  KronosValue *iterable;

  POP_OR_RETURN(vm, iterable);

  if (iterable->type == VAL_LIST) {
    // Create iterator (just push the list and current index)
    // Push list back to stack, then push index 0
    PUSH_OR_RETURN_WITH_CLEANUP(vm, iterable, value_release(iterable););
    KronosValue *index = value_new_number(0);
    PUSH_OR_RETURN_WITH_CLEANUP(vm, index, value_release(index);
                                value_release(iterable););
    value_release(index);
    value_release(iterable); // Release our pop reference
  } else if (iterable->type == VAL_RANGE) {
    // For ranges, push the range and current value (start)
    PUSH_OR_RETURN_WITH_CLEANUP(vm, iterable, value_release(iterable););
    KronosValue *current = value_new_number(iterable->as.range.start);
    PUSH_OR_RETURN_WITH_CLEANUP(vm, current, value_release(current);
                                value_release(iterable););
    value_release(current);
    value_release(iterable); // Release our pop reference
  } else {
    value_release(iterable);
    return vm_error(vm, KRONOS_ERR_RUNTIME,
                    "Expected list or range for iteration");
  }
  return 0;
}

static int handle_op_list_next(KronosVM *vm) {
  // Stack: [iterable, state] (iterable on bottom, state on top)
  // For lists: state is index (number)
  // For ranges: state is current value (number)
  // Verify stack has at least 2 items before popping
  size_t stack_depth = (size_t)(vm->stack_top - vm->stack);
  if (stack_depth < 2) {
    return vm_errorf(
        vm, KRONOS_ERR_RUNTIME,
        "Stack underflow in iteration: expected 2 items, got %zu. "
        "This usually means iterator variables were not loaded correctly.",
        stack_depth);
  }
  KronosValue *state_val;

  POP_OR_RETURN(vm, state_val);
  KronosValue *iterable;

  POP_OR_RETURN_WITH_CLEANUP(vm, iterable, value_release(state_val));

  if (iterable->type == VAL_LIST) {
    if (state_val->type != VAL_NUMBER) {
      value_release(state_val);
      value_release(iterable);
      return vm_error(vm, KRONOS_ERR_RUNTIME, "Invalid iterator state");
    }

    size_t idx = (size_t)state_val->as.number;
    bool has_more = idx < iterable->as.list.count;

    if (has_more) {
      // Push in order: [list, index+1, item, has_more]
      // This way, after popping has_more, item is on top for OP_STORE_VAR
      // After storing item, we have [list, index+1] for next iteration

      // Push list first (bottom of stack)
      value_retain(iterable);
      PUSH_OR_RETURN_WITH_CLEANUP(vm, iterable, value_release(iterable);
                                  value_release(state_val););
      value_release(iterable);

      // Update and push index
      KronosValue *next_index = value_new_number((double)(idx + 1));
      PUSH_OR_RETURN_WITH_CLEANUP(vm, next_index, value_release(next_index);
                                  value_release(state_val););
      value_release(next_index);

      // Push current item
      KronosValue *item = iterable->as.list.items[idx];
      value_retain(item);
      PUSH_OR_RETURN_WITH_CLEANUP(vm, item, value_release(item);
                                  value_release(state_val););
      value_release(item);

      // Push has_more flag
      KronosValue *has_more_val = value_new_bool(true);
      PUSH_OR_RETURN_WITH_CLEANUP(vm, has_more_val, value_release(has_more_val);
                                  value_release(state_val););
      value_release(has_more_val);

      // Release our popped references (values are now on stack)
      value_release(state_val);
      value_release(iterable);
    } else {
      // No more items - push list and index back for cleanup, then has_more =
      // false Stack should be: [list, index, has_more=false] for cleanup code
      // Push list first (bottom of stack)
      value_retain(iterable);
      PUSH_OR_RETURN_WITH_CLEANUP(vm, iterable, value_release(iterable);
                                  value_release(state_val););
      value_release(iterable);

      // Push index back
      value_retain(state_val);
      PUSH_OR_RETURN_WITH_CLEANUP(vm, state_val, value_release(state_val););
      value_release(state_val);

      // Push has_more = false
      KronosValue *has_more_val = value_new_bool(false);
      PUSH_OR_RETURN_WITH_CLEANUP(vm, has_more_val,
                                  value_release(has_more_val););
      value_release(has_more_val);

      // Release our popped references (values are now on stack)
      // Note: we retained before pushing and released after, so the only
      // remaining refs are the pop refs which we release here
      value_release(state_val);
      value_release(iterable);
    }
  } else if (iterable->type == VAL_RANGE) {
    if (state_val->type != VAL_NUMBER) {
      value_release(state_val);
      value_release(iterable);
      return vm_error(vm, KRONOS_ERR_RUNTIME, "Invalid iterator state");
    }

    double current = state_val->as.number;
    double start = iterable->as.range.start;
    double end = iterable->as.range.end;
    double step = iterable->as.range.step;

    // Check if we've reached the end
    bool has_more = false;
    if (step > 0) {
      has_more = (current < end) || (current == start && start < end);
    } else if (step < 0) {
      has_more = (current > end) || (current == start && start > end);
    } else {
      // step == 0: only one value (start)
      has_more = (current == start);
    }

    if (has_more) {
      // Push in order: [range, next_value, current_value, has_more]
      // Push range back - push will retain it
      PUSH_OR_RETURN_WITH_CLEANUP(vm, iterable, value_release(state_val););

      // Calculate and push next value
      double next = current + step;
      KronosValue *next_val = value_new_number(next);
      PUSH_OR_RETURN_WITH_CLEANUP(vm, next_val, value_release(next_val);
                                  value_release(iterable);
                                  value_release(state_val););
      value_release(next_val);

      // Push current value (the item)
      KronosValue *current_val = value_new_number(current);
      PUSH_OR_RETURN_WITH_CLEANUP(vm, current_val, value_release(current_val);
                                  value_release(iterable);
                                  value_release(state_val););
      value_release(current_val);

      // Push has_more flag
      KronosValue *has_more_val = value_new_bool(true);
      PUSH_OR_RETURN_WITH_CLEANUP(vm, has_more_val, value_release(has_more_val);
                                  value_release(iterable);
                                  value_release(state_val););
      value_release(has_more_val);

      // Release our popped references (range is now on stack)
      value_release(state_val);
      value_release(iterable);
    } else {
      // No more items - push range and state back for cleanup, then has_more =
      // false Stack should be: [range, state, has_more=false] for cleanup code
      // Push range first (bottom of stack)
      value_retain(iterable);
      PUSH_OR_RETURN_WITH_CLEANUP(vm, iterable, value_release(iterable);
                                  value_release(state_val););
      value_release(iterable);

      // Push state back
      value_retain(state_val);
      PUSH_OR_RETURN_WITH_CLEANUP(vm, state_val, value_release(state_val););
      value_release(state_val);

      // Push has_more = false
      KronosValue *has_more_val = value_new_bool(false);
      PUSH_OR_RETURN_WITH_CLEANUP(vm, has_more_val,
                                  value_release(has_more_val););
      value_release(has_more_val);

      // Release our popped references (values are now on stack)
      value_release(state_val);
      value_release(iterable);
    }
  } else {
    value_release(state_val);
    value_release(iterable);
    return vm_error(vm, KRONOS_ERR_RUNTIME, "Invalid iterable type");
  }

  return 0;
}

static int handle_op_try_enter(KronosVM *vm) {
  // Save the IP before reading the offset bytes
  // vm->ip currently points to the first offset byte (try_start_pos)
  uint8_t *try_start_pos = vm->ip;

  // Read exception handler offset
  uint8_t high = read_byte(vm);
  if (vm->last_error_message) {
    return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
  }
  uint8_t low = read_byte(vm);
  if (vm->last_error_message) {
    return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
  }
  uint16_t handler_offset = (uint16_t)(high << 8 | low);

  if (vm->exception_handler_count >= EXCEPTION_HANDLERS_MAX) {
    return vm_error(vm, KRONOS_ERR_RUNTIME, "Too many nested try blocks");
  }

  // Validate handler offset is within bytecode bounds
  // The compiler calculates: handler_offset = exception_handler_pos -
  // (try_start_pos + 2) So exception_handler_pos = try_start_pos + 2 +
  // handler_offset try_start_pos points to the first offset byte (after
  // OP_TRY_ENTER)
  uint8_t *handler_ip = try_start_pos + 2 + handler_offset;
  if (handler_ip < vm->bytecode->code ||
      handler_ip >= vm->bytecode->code + vm->bytecode->count) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Exception handler offset out of bounds (offset: %u, "
                     "bytecode size: %zu)",
                     handler_offset, vm->bytecode->count);
  }

  // Push exception handler onto stack
  size_t idx = vm->exception_handler_count++;
  vm->exception_handlers[idx].try_start_ip = vm->ip;
  vm->exception_handlers[idx].handler_ip = handler_ip;
  vm->exception_handlers[idx].catch_start_ip =
      handler_ip; // Catch blocks start at handler
  vm->exception_handlers[idx].catch_count =
      0; // Will be incremented by OP_CATCH
  vm->exception_handlers[idx].has_finally = false; // Will be set by OP_FINALLY
  vm->exception_handlers[idx].finally_ip = NULL;

  return 0;
}

static int handle_op_try_exit(KronosVM *vm) {
  // Normal completion of try block - read finally jump offset
  uint8_t high = read_byte(vm);
  if (vm->last_error_message) {
    return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
  }
  uint8_t low = read_byte(vm);
  if (vm->last_error_message) {
    return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
  }
  uint16_t finally_offset = (uint16_t)(high << 8 | low);

  if (vm->exception_handler_count == 0) {
    return vm_error(vm, KRONOS_ERR_INTERNAL,
                    "OP_TRY_EXIT without matching OP_TRY_ENTER");
  }

  // Pop exception handler
  vm->exception_handler_count--;

  // If finally exists, jump to it
  if (finally_offset > 0) {
    vm->ip += finally_offset;
  }

  return 0;
}

static int handle_op_catch(KronosVM *vm) {
  // Read error type constant (0xFFFF means catch all)
  // Save the current error state - we're handling an exception, so errors from
  // OP_THROW are expected and shouldn't prevent reading operands
  bool had_error = (vm->last_error_code != KRONOS_OK);
  char *saved_error_msg =
      vm->last_error_message ? strdup(vm->last_error_message) : NULL;
  KronosErrorCode saved_error_code = vm->last_error_code;
  char *saved_error_type =
      vm->last_error_type ? strdup(vm->last_error_type) : NULL;

  // Temporarily clear error to allow reading operands
  vm_clear_error(vm);

  uint16_t error_type_idx = read_uint16(vm);
  // Check for error from read_uint16 (shouldn't happen now)
  if (vm->last_error_message) {
    free(saved_error_msg);
    free(saved_error_type);
    return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
  }
  // Read catch variable name constant (0xFFFF means no variable)
  uint16_t catch_var_idx = read_uint16(vm);
  // Check for error from read_uint16 (shouldn't happen now)
  if (vm->last_error_message) {
    free(saved_error_msg);
    free(saved_error_type);
    return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
  }

  // Restore error state if we had one
  if (had_error) {
    free(vm->last_error_message);
    free(vm->last_error_type);
    vm->last_error_message = saved_error_msg;
    vm->last_error_code = saved_error_code;
    vm->last_error_type = saved_error_type;
  } else {
    free(saved_error_msg);
    free(saved_error_type);
  }
  (void)catch_var_idx; // Will be used by OP_STORE_VAR following this
                       // instruction

  if (vm->exception_handler_count == 0) {
    return vm_error(vm, KRONOS_ERR_INTERNAL,
                    "OP_CATCH without matching OP_TRY_ENTER");
  }

  // Increment catch count for current exception handler
  size_t idx = vm->exception_handler_count - 1;
  vm->exception_handlers[idx].catch_count++;

  // Check if we have an active error to handle
  if (vm->last_error_code != KRONOS_OK) {
    // Get the current error type
    const char *current_error_type =
        vm->last_error_type ? vm->last_error_type
                            : error_code_to_type_name(vm->last_error_code);

    // Check if this catch block matches
    bool matches = false;
    if (error_type_idx == 0xFFFF) {
      // Catch all
      matches = true;
    } else if (error_type_idx < vm->bytecode->const_count) {
      KronosValue *type_val = vm->bytecode->constants[error_type_idx];
      if (type_val && type_val->type == VAL_STRING) {
        if (strcmp(type_val->as.string.data, current_error_type) == 0) {
          matches = true;
        }
      }
    }

    if (matches) {
      // Push error message onto stack for OP_STORE_VAR to consume
      const char *error_msg =
          vm->last_error_message ? vm->last_error_message : "Unknown error";
      KronosValue *error_val = value_new_string(error_msg, strlen(error_msg));
      if (error_val) {
        PUSH_OR_RETURN_WITH_CLEANUP(vm, error_val, value_release(error_val););
        value_release(error_val);
      } else {
        // Fallback - push empty string
        KronosValue *empty = value_new_string("", 0);
        PUSH_OR_RETURN_WITH_CLEANUP(vm, empty, value_release(empty););
        value_release(empty);
      }

      // Clear error - exception is now handled
      vm_clear_error(vm);
      // Note: handling_exception flag will be reset in the main loop after
      // OP_CATCH returns
    }
    // If not matched, continue to next OP_CATCH or fall through to finally
  }
  // If no error, this is normal execution - just skip the catch block
  // operands The actual catch block code will be skipped during normal try
  // execution
  return 0;
}

static int handle_op_finally(KronosVM *vm) {
  if (vm->exception_handler_count == 0) {
    return vm_error(vm, KRONOS_ERR_INTERNAL,
                    "OP_FINALLY without matching OP_TRY_ENTER");
  }

  // Mark that finally block exists
  size_t idx = vm->exception_handler_count - 1;
  vm->exception_handlers[idx].has_finally = true;
  vm->exception_handlers[idx].finally_ip = vm->ip;

  return 0;
}

static int handle_op_throw(KronosVM *vm) {
  // Read error type constant (0xFFFF means generic Error)
  uint16_t error_type_idx = read_uint16(vm);
  // Check for error from read_uint16
  if (vm->last_error_message) {
    return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
  }

  // Pop error message from stack
  KronosValue *message_val;

  POP_OR_RETURN(vm, message_val);

  // Get error message as string
  const char *message = "Unknown error";
  if (message_val->type == VAL_STRING) {
    message = message_val->as.string.data;
  }

  // Get error type name
  const char *type_name = "Error";
  if (error_type_idx != 0xFFFF && error_type_idx < vm->bytecode->const_count) {
    KronosValue *type_val = vm->bytecode->constants[error_type_idx];
    if (type_val && type_val->type == VAL_STRING) {
      type_name = type_val->as.string.data;
    }
  }

  // Set error with type
  vm_set_error_with_type(vm, KRONOS_ERR_RUNTIME, type_name, message);
  value_release(message_val);

  // Return 0 to continue loop - exception handling code will handle it
  return 0;
}

static int handle_op_import(KronosVM *vm) {
  // Read constant indices for module name and file path (in order of
  // emission)
  uint16_t module_name_idx = read_uint16(vm);
  // Check for error from read_uint16
  if (vm->last_error_message) {
    return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
  }
  uint16_t file_path_idx = read_uint16(vm);
  // Check for error from read_uint16
  if (vm->last_error_message) {
    return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
  }

  // Validate indices
  if (!vm->bytecode || file_path_idx >= vm->bytecode->const_count ||
      module_name_idx >= vm->bytecode->const_count) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Invalid constant index in import instruction "
                     "(module_idx=%u, file_idx=%u, const_count=%zu)",
                     module_name_idx, file_path_idx,
                     vm->bytecode ? vm->bytecode->const_count : 0);
  }

  // Get constants from pool (don't release - owned by bytecode)
  KronosValue *module_name_val = vm->bytecode->constants[module_name_idx];
  KronosValue *file_path_val = vm->bytecode->constants[file_path_idx];

  if (!module_name_val || !file_path_val) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Null constant at index (module_idx=%u, file_idx=%u)",
                     module_name_idx, file_path_idx);
  }

  if (module_name_val->type != VAL_STRING) {
    return vm_errorf(vm, KRONOS_ERR_INTERNAL,
                     "Module name must be a string, got type %d",
                     module_name_val->type);
  }

  const char *module_name = module_name_val->as.string.data;
  const char *file_path = NULL;

  // If file_path is not null, it's a file-based import
  if (file_path_val->type != VAL_NIL) {
    if (file_path_val->type != VAL_STRING) {
      return vm_error(vm, KRONOS_ERR_INTERNAL,
                      "File path must be a string or null");
    }
    file_path = file_path_val->as.string.data;
  }

  // Load the module
  // For built-in modules (file_path is NULL), we don't need to load
  // anything Module resolution happens when module.function is called
  if (file_path) {
    // Find the root VM for circular import detection
    // If this VM is a module VM, use its root_vm_ref
    // Otherwise, this is the root VM
    KronosVM *root_vm_for_import = vm->root_vm_ref ? vm->root_vm_ref : vm;

    // Use current file path as base for relative imports
    // Pass root_vm_for_import as parent_vm for circular import detection
    int load_result = vm_load_module(vm, module_name, file_path,
                                     vm->current_file_path, root_vm_for_import);
    if (load_result < 0) {
      return load_result;
    }
  }

  // Constants are owned by bytecode, don't release
  return 0;
}

static int handle_op_define_func(KronosVM *vm) {
  // Validate bytecode is available
  if (!vm->bytecode || !vm->bytecode->code) {
    return vm_error(vm, KRONOS_ERR_INTERNAL,
                    "Cannot define function: bytecode is NULL");
  }

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

  // Read required_param_count (params without defaults)
  uint8_t required_param_count = read_byte(vm);

  // Read has_variadic flag
  uint8_t has_variadic = read_byte(vm);

  if (has_variadic > 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Invalid function metadata: has_variadic=%u",
                     (unsigned)has_variadic);
  }
  size_t regular_param_count = (size_t)param_count;
  if (has_variadic) {
    if (param_count == 0) {
      return vm_error(vm, KRONOS_ERR_RUNTIME,
                      "Invalid function metadata: variadic function has zero "
                      "parameters");
    }
    regular_param_count--;
  }
  if ((size_t)required_param_count > regular_param_count) {
    return vm_errorf(
        vm, KRONOS_ERR_RUNTIME,
        "Invalid function metadata: required_param_count (%u) exceeds "
        "non-variadic parameter count (%zu)",
        (unsigned)required_param_count, regular_param_count);
  }

  // Create function
  Function *func = calloc(1, sizeof(Function));
  if (!func) {
    return vm_error(vm, KRONOS_ERR_INTERNAL,
                    "Failed to allocate function structure");
  }

  // Allocate function name - check for NULL immediately after strdup
  func->name = strdup(name_val->as.string.data);
  if (!func->name) {
    // Allocation failure: free func and return error
    free(func);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to copy function name");
  }

  func->param_count = param_count;
  func->required_param_count = required_param_count;
  func->has_variadic = (has_variadic != 0);
  func->params = param_count > 0 ? calloc(param_count, sizeof(char *)) : NULL;
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
      param_error = vm_error(vm, KRONOS_ERR_RUNTIME,
                             "Cannot use 'Pi' as a parameter name (reserved)");
      break;
    }

    // Allocate parameter name - check for NULL immediately after strdup
    func->params[i] = strdup(param_val->as.string.data);
    if (!func->params[i]) {
      // Allocation failure: set error and break (cleanup happens below)
      param_error =
          vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to copy parameter name");
      break;
    }
    // Only increment filled_params after successful allocation
    filled_params++;
  }

  // Cleanup on any error: free all allocated resources
  if (param_error != 0) {
    (void)filled_params;
    function_free(func);
    return param_error;
  }

  // Read default value constants
  // Number of defaults = param_count - required_param_count - (has_variadic ? 1 : 0)
  size_t num_defaults = regular_param_count - (size_t)required_param_count;
  if (num_defaults > 0) {
    func->param_defaults = malloc(sizeof(KronosValue *) * param_count);
    if (!func->param_defaults) {
      function_free(func);
      return vm_error(vm, KRONOS_ERR_INTERNAL,
                      "Failed to allocate default values array");
    }
    // Initialize all to NULL
    for (size_t i = 0; i < param_count; i++) {
      func->param_defaults[i] = NULL;
    }
    // Read default values for optional parameters
    for (size_t i = 0; i < num_defaults; i++) {
      size_t param_idx = required_param_count + i;
      KronosValue *default_val = read_constant(vm);
      if (!default_val) {
        function_free(func);
        return vm_propagate_error(vm, KRONOS_ERR_INTERNAL);
      }
      value_retain(default_val);
      func->param_defaults[param_idx] = default_val;
    }
  }

  // Consume function body start position (2 bytes) - part of bytecode
  // format but not used at runtime; we just need to advance the instruction
  // pointer Format:
  // [OP_DEFINE_FUNC][name_idx:2][param_count:1][required:1][variadic:1][params:2*N][defaults:2*M][body_start:2][OP_JUMP][skip_offset:2]
  read_byte(vm); // body_start high byte
  if (vm->last_error_message) {
    function_free(func);
    return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
  }
  read_byte(vm); // body_start low byte
  if (vm->last_error_message) {
    function_free(func);
    return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
  }

  // Consume OP_JUMP instruction byte (part of bytecode format)
  read_byte(vm);
  if (vm->last_error_message) {
    function_free(func);
    return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
  }

  // Read jump offset to skip function body (2 bytes: high byte, low byte)
  // The compiler calculates offset as: func_end - (skip_body_pos + 2)
  // where skip_body_pos is the position of OP_JUMP instruction.
  // After reading OP_JUMP (1 byte) and offset (2 bytes), vm->ip points to
  // skip_body_pos + 3, which is the start of the function body.
  // The offset tells us: from position (skip_body_pos + 2), skip forward by
  // offset bytes to reach func_end. So: func_end = (skip_body_pos + 2) + offset
  // In VM terms: func_end = (vm->ip - 1) + offset
  uint16_t skip_offset = read_uint16(vm);
  if (vm->last_error_message) {
    function_free(func);
    return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
  }

  // Save the position where function body starts (current ip after reading
  // offset)
  uint8_t *body_start_ptr = vm->ip;

  // Validate vm->ip is still within bounds before calculating body_end_ptr
  if (vm->ip < vm->bytecode->code ||
      vm->ip >= vm->bytecode->code + vm->bytecode->count) {
    function_free(func);
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Instruction pointer out of bounds when defining function "
                     "(ip offset: %zu, bytecode size: %zu)",
                     (size_t)(vm->ip - vm->bytecode->code),
                     vm->bytecode->count);
  }

  // Calculate body end: offset is relative to position 2 bytes before current
  // ip (the position after the OP_JUMP byte and before the offset bytes).
  // The compiler calculates offset as: func_end - (skip_body_pos + 2)
  // where skip_body_pos is the position of OP_JUMP.
  // After reading OP_JUMP (1 byte) and offset (2 bytes), vm->ip points to
  // skip_body_pos + 3, which is the start of the function body.
  // The offset tells us: from position (skip_body_pos + 2), skip forward by
  // offset bytes to reach func_end. So: func_end = (skip_body_pos + 2) + offset
  // In VM terms: func_end = (vm->ip - 1) + offset
  //
  // IMPORTANT: func_end points to the position AFTER the last byte of the
  // function body (after OP_RETURN_VAL), so body_end_ptr should also point
  // after the function body. The calculation (ip - 1) + offset gives us
  // func_end, but func_end points TO OP_RETURN_VAL, not after it. We need to
  // add 1 to skip past the OP_RETURN_VAL instruction.
  uint8_t *body_end_ptr = vm->ip - 1 + skip_offset + 1;

  // Validate that body_end_ptr is within valid bytecode bounds
  if (body_end_ptr < vm->bytecode->code ||
      body_end_ptr > vm->bytecode->code + vm->bytecode->count) {
    function_free(func);
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function body extends beyond bytecode bounds "
                     "(offset: %u, bytecode size: %zu, ip offset: %zu)",
                     skip_offset, vm->bytecode->count,
                     (size_t)(vm->ip - vm->bytecode->code));
  }

  // Copy function body bytecode
  // Validate that body_end_ptr >= body_start_ptr to prevent wrap-around
  if (body_end_ptr < body_start_ptr) {
    function_free(func);
    return vm_errorf(
        vm, KRONOS_ERR_RUNTIME,
        "Invalid function body: backward jump detected (offset: %u)",
        skip_offset);
  }

  size_t bytecode_size = body_end_ptr - body_start_ptr;

  // Additional validation: ensure we're not copying beyond bytecode bounds
  if (body_start_ptr + bytecode_size >
      vm->bytecode->code + vm->bytecode->count) {
    function_free(func);
    return vm_errorf(
        vm, KRONOS_ERR_RUNTIME,
        "Function body bytecode extends beyond valid range "
        "(size: %zu, available: %zu)",
        bytecode_size,
        (size_t)(vm->bytecode->code + vm->bytecode->count - body_start_ptr));
  }

  // Handle empty function body (valid case)
  if (bytecode_size == 0) {
    func->bytecode.code = NULL;
    func->bytecode.count = 0;
    func->bytecode.capacity = 0;
  } else {
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
    memcpy(func->bytecode.code, body_start_ptr, bytecode_size);
  }

  // Copy constants (retain references)
  func->bytecode.const_count = vm->bytecode->const_count;
  func->bytecode.const_capacity = vm->bytecode->const_count;

  // Handle empty constants array
  if (func->bytecode.const_count == 0) {
    func->bytecode.constants = NULL;
  } else {
    // Validate that parent bytecode has constants array
    if (!vm->bytecode->constants) {
      if (func->bytecode.code) {
        free(func->bytecode.code);
        func->bytecode.code = NULL;
        func->bytecode.count = 0;
        func->bytecode.capacity = 0;
      }
      function_free(func);
      return vm_error(
          vm, KRONOS_ERR_INTERNAL,
          "Parent bytecode has non-zero const_count but NULL constants array");
    }

    func->bytecode.constants =
        malloc(sizeof(KronosValue *) * func->bytecode.const_count);
    if (!func->bytecode.constants) {
      // Allocation failure: free func->bytecode.code, then clean up func and
      // return error
      if (func->bytecode.code) {
        free(func->bytecode.code);
        func->bytecode.code = NULL;
        func->bytecode.count = 0;
        func->bytecode.capacity = 0;
      }
      function_free(func);
      return vm_error(vm, KRONOS_ERR_INTERNAL,
                      "Failed to allocate memory for function constants");
    }
    for (size_t i = 0; i < func->bytecode.const_count; i++) {
      func->bytecode.constants[i] = vm->bytecode->constants[i];
      value_retain(func->bytecode.constants[i]);
    }
  }

  // Store function
  int define_status = vm_define_function(vm, func);
  if (define_status != 0) {
    function_free(func);
    return define_status;
  }

  // Skip over function body
  vm->ip = body_end_ptr;
  return 0;
}

/**
 * @brief Handle OP_MAKE_FUNCTION - create a function value (lambda)
 *
 * Bytecode format:
 *   OP_MAKE_FUNCTION [param_count:1] [required_param_count:1] [has_variadic:1]
 *   [param_name_idx:2*N] [default_const:2*M] [body_len:2] [body:N]
 *
 * Creates a VAL_FUNCTION value containing the bytecode and parameter names,
 * pushes it onto the stack, and skips over the inline body bytecode.
 */
static int handle_op_make_function(KronosVM *vm) {
  // Read parameter count
  uint8_t param_count = read_byte(vm);
  if (vm->last_error_message) {
    return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
  }

  // Read required_param_count
  uint8_t required_param_count = read_byte(vm);
  if (vm->last_error_message) {
    return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
  }

  // Read has_variadic flag
  uint8_t has_variadic = read_byte(vm);
  if (vm->last_error_message) {
    return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
  }

  if (has_variadic > 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Invalid function metadata: has_variadic=%u",
                     (unsigned)has_variadic);
  }
  size_t regular_param_count = (size_t)param_count;
  if (has_variadic) {
    if (param_count == 0) {
      return vm_error(vm, KRONOS_ERR_RUNTIME,
                      "Invalid lambda metadata: variadic function has zero "
                      "parameters");
    }
    regular_param_count--;
  }
  if ((size_t)required_param_count > regular_param_count) {
    return vm_errorf(
        vm, KRONOS_ERR_RUNTIME,
        "Invalid lambda metadata: required_param_count (%u) exceeds "
        "non-variadic parameter count (%zu)",
        (unsigned)required_param_count, regular_param_count);
  }

  // Read parameter names from constant pool
  char **param_names = NULL;
  if (param_count > 0) {
    param_names = malloc(sizeof(char *) * param_count);
    if (!param_names) {
      return vm_error(vm, KRONOS_ERR_INTERNAL,
                      "Failed to allocate parameter names array");
    }

    for (int i = 0; i < param_count; i++) {
      KronosValue *name_val = read_constant(vm);
      if (!name_val || name_val->type != VAL_STRING) {
        // Cleanup and error
        for (int j = 0; j < i; j++) {
          free(param_names[j]);
        }
        free(param_names);
        return vm_error(vm, KRONOS_ERR_INTERNAL,
                        "Invalid parameter name constant");
      }
      param_names[i] = strdup(name_val->as.string.data);
      if (!param_names[i]) {
        // Cleanup and error
        for (int j = 0; j < i; j++) {
          free(param_names[j]);
        }
        free(param_names);
        return vm_error(vm, KRONOS_ERR_INTERNAL,
                        "Failed to allocate parameter name");
      }
    }
  }

  // Calculate number of default values
  size_t num_defaults = regular_param_count - (size_t)required_param_count;

  // Read default value constants
  KronosValue **param_defaults = NULL;
  if (num_defaults > 0) {
    param_defaults = malloc(sizeof(KronosValue *) * param_count);
    if (!param_defaults) {
      for (int i = 0; i < param_count; i++) {
        free(param_names[i]);
      }
      free(param_names);
      return vm_error(vm, KRONOS_ERR_INTERNAL,
                      "Failed to allocate default values array");
    }
    // Initialize all to NULL
    for (size_t i = 0; i < (size_t)param_count; i++) {
      param_defaults[i] = NULL;
    }
    // Read default values
    for (size_t i = 0; i < num_defaults; i++) {
      size_t param_idx = required_param_count + i;
      KronosValue *default_val = read_constant(vm);
      if (!default_val) {
        // Cleanup
        for (size_t j = 0; j < (size_t)param_count; j++) {
          if (param_defaults[j]) value_release(param_defaults[j]);
        }
        free(param_defaults);
        for (int j = 0; j < param_count; j++) {
          free(param_names[j]);
        }
        free(param_names);
        return vm_propagate_error(vm, KRONOS_ERR_INTERNAL);
      }
      value_retain(default_val);
      param_defaults[param_idx] = default_val;
    }
  }

  // Read body length (2 bytes)
  uint8_t high = read_byte(vm);
  if (vm->last_error_message) {
    if (param_defaults) {
      for (size_t i = 0; i < (size_t)param_count; i++) {
        if (param_defaults[i]) value_release(param_defaults[i]);
      }
      free(param_defaults);
    }
    for (int i = 0; i < param_count; i++) {
      free(param_names[i]);
    }
    free(param_names);
    return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
  }
  uint8_t low = read_byte(vm);
  if (vm->last_error_message) {
    if (param_defaults) {
      for (size_t i = 0; i < (size_t)param_count; i++) {
        if (param_defaults[i]) value_release(param_defaults[i]);
      }
      free(param_defaults);
    }
    for (int i = 0; i < param_count; i++) {
      free(param_names[i]);
    }
    free(param_names);
    return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
  }
  uint16_t body_len = (uint16_t)((high << 8) | low);

  if (!vm->bytecode || !vm->bytecode->code ||
      vm->ip < vm->bytecode->code ||
      vm->ip > vm->bytecode->code + vm->bytecode->count) {
    if (param_defaults) {
      for (size_t i = 0; i < (size_t)param_count; i++) {
        if (param_defaults[i])
          value_release(param_defaults[i]);
      }
      free(param_defaults);
    }
    for (int i = 0; i < param_count; i++) {
      free(param_names[i]);
    }
    free(param_names);
    return vm_error(vm, KRONOS_ERR_RUNTIME,
                    "Malformed bytecode: invalid function body pointer");
  }
  size_t remaining_body_bytes =
      (size_t)(vm->bytecode->code + vm->bytecode->count - vm->ip);
  if ((size_t)body_len > remaining_body_bytes) {
    if (param_defaults) {
      for (size_t i = 0; i < (size_t)param_count; i++) {
        if (param_defaults[i])
          value_release(param_defaults[i]);
      }
      free(param_defaults);
    }
    for (int i = 0; i < param_count; i++) {
      free(param_names[i]);
    }
    free(param_names);
    return vm_errorf(
        vm, KRONOS_ERR_RUNTIME,
        "Malformed bytecode: function body length %u exceeds remaining bytes "
        "%zu",
        (unsigned)body_len, remaining_body_bytes);
  }

  // The body bytecode starts at current IP
  uint8_t *body_bytecode = vm->ip;

  // Create the function value with default parameters and variadic info
  KronosValue *func_val = value_new_function(body_bytecode, body_len,
                                              param_count, required_param_count,
                                              has_variadic != 0, param_names,
                                              param_defaults);

  // Free the temporary param_names array (value_new_function made copies)
  for (int i = 0; i < param_count; i++) {
    free(param_names[i]);
  }
  free(param_names);

  // Free temporary param_defaults (value_new_function retained copies)
  if (param_defaults) {
    for (size_t i = 0; i < (size_t)param_count; i++) {
      if (param_defaults[i]) value_release(param_defaults[i]);
    }
    free(param_defaults);
  }

  if (!func_val) {
    return vm_error(vm, KRONOS_ERR_INTERNAL,
                    "Failed to create function value");
  }

  // Push function onto stack
  PUSH_OR_RETURN_WITH_CLEANUP(vm, func_val, value_release(func_val););
  value_release(func_val); // Stack now owns it

  // Skip over the inline body bytecode
  vm->ip += body_len;

  return 0;
}

/**
 * @brief Handle OP_CALL_VALUE - call a function value from stack
 *
 * Bytecode format:
 *   OP_CALL_VALUE [arg_count:1]
 *
 * Stack (before): [func_val] [arg0] [arg1] ... [argN-1]
 * Stack (after): [return_value]
 *
 * Note: This is a placeholder - actual implementation would call the function.
 * For now, function values in variables are called via handle_op_call_func.
 */
static int handle_op_call_value(KronosVM *vm) {
  // This opcode is not currently emitted - function values in variables
  // are called via OP_CALL_FUNC which checks for VAL_FUNCTION variables.
  (void)vm;
  return vm_error(vm, KRONOS_ERR_INTERNAL,
                  "OP_CALL_VALUE not implemented (use OP_CALL_FUNC)");
}

/**
 * @brief Handle OP_TUPLE_NEW opcode
 *
 * Creates a new tuple from N values on the stack.
 *
 * Opcode format:
 *   OP_TUPLE_NEW [count:1]
 *
 * Stack (before): [val0] [val1] ... [valN-1]
 * Stack (after): [tuple]
 */
static int handle_op_tuple_new(KronosVM *vm) {
  uint8_t count = read_byte(vm);

  // Pop values from stack (in reverse order to maintain element order)
  KronosValue **items = NULL;
  if (count > 0) {
    items = malloc(count * sizeof(KronosValue *));
    if (!items) {
      return vm_error(vm, KRONOS_ERR_RUNTIME, "Failed to allocate tuple items");
    }

    // Pop values in reverse order (stack is LIFO)
    for (int i = count - 1; i >= 0; i--) {
      POP_OR_RETURN_WITH_CLEANUP(vm, items[i], {
        // Clean up already-popped items on failure
        for (int j = count - 1; j > i; j--) {
          value_release(items[j]);
        }
        free(items);
      });
    }
  }

  // Create tuple (items are retained by value_new_tuple)
  KronosValue *tuple = value_new_tuple(items, count);

  // Release our references since tuple now owns them
  for (uint8_t i = 0; i < count; i++) {
    value_release(items[i]);
  }
  free(items);

  if (!tuple) {
    return vm_error(vm, KRONOS_ERR_RUNTIME, "Failed to create tuple");
  }

  // Push tuple onto stack
  PUSH_OR_RETURN_WITH_CLEANUP(vm, tuple, value_release(tuple););
  value_release(tuple); // Stack now owns the tuple

  return 0;
}

/**
 * @brief Handle OP_UNPACK opcode
 *
 * Unpacks a tuple or list into N values on the stack.
 *
 * Opcode format:
 *   OP_UNPACK [count:1]
 *
 * Stack (before): [tuple_or_list]
 * Stack (after): [val0] [val1] ... [valN-1]
 *
 * Note: Values are pushed so that the first element is deepest on the stack,
 * allowing OP_STORE_VAR to pop them in reverse declaration order.
 */
static int handle_op_unpack(KronosVM *vm) {
  uint8_t expected_count = read_byte(vm);

  KronosValue *container;
  POP_OR_RETURN(vm, container);

  size_t actual_count = 0;
  KronosValue **items = NULL;

  if (container->type == VAL_TUPLE) {
    actual_count = container->as.tuple.count;
    items = container->as.tuple.items;
  } else if (container->type == VAL_LIST) {
    actual_count = container->as.list.count;
    items = container->as.list.items;
  } else {
    value_release(container);
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Cannot unpack value of type %d (expected tuple or list)",
                     container->type);
  }

  if (actual_count != expected_count) {
    value_release(container);
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Unpack count mismatch: expected %d values, got %zu",
                     expected_count, actual_count);
  }

  // Push values onto stack in order (first element first, deepest on stack)
  for (size_t i = 0; i < actual_count; i++) {
    value_retain(items[i]);
    PUSH_OR_RETURN_WITH_CLEANUP(vm, items[i], {
      value_release(items[i]);
      // Release already-pushed items
      for (size_t j = 0; j < i; j++) {
        KronosValue *val;
        POP_OR_RETURN(vm, val);
        value_release(val);
      }
      value_release(container);
    });
    value_release(items[i]); // Stack now owns the value
  }

  value_release(container);
  return 0;
}

static int handle_op_return_val(KronosVM *vm) {
  // Pop return value from stack
  KronosValue *return_value;

  POP_OR_RETURN(vm, return_value);

  // If we're in a function call, return from it
  if (vm->call_stack_size > 0) {
    CallFrame *frame = &vm->call_stack[vm->call_stack_size - 1];

    // Restore VM state
    // If return_ip is NULL, this is a module function call and we should
    // just break The return value is already on the stack, caller will
    // handle cleanup
    if (frame->return_ip == NULL && frame->return_bytecode == NULL) {
      // This is a module function call - return from vm_execute entirely
      // Push return value back onto stack (it was popped above)
      // push() retains the value (increments refcount), so we release our
      // local reference after pushing
      PUSH_OR_RETURN_WITH_CLEANUP(vm, return_value,
                                  value_release(return_value););
      value_release(return_value); // Release our reference, stack now owns it
      // Don't clean up locals here - caller will handle cleanup
      // Don't decrement call_stack_size here - caller will handle cleanup
      // Don't set current_frame to NULL here - caller needs it for cleanup
      return 0; // Exit vm_execute, return value is on the stack
    }

    // Clean up local variables (only for regular function calls, not module
    // calls)
    for (size_t i = 0; i < frame->local_count; i++) {
      free(frame->locals[i].name);
      value_release(frame->locals[i].value);
      free(frame->locals[i].type_name);
    }

    vm->ip = frame->return_ip;
    vm->bytecode = frame->return_bytecode;

    // Free dynamically allocated bytecode (for lambdas)
    if (frame->owned_bytecode) {
      free(frame->owned_bytecode);
      frame->owned_bytecode = NULL;
    }

    vm->call_stack_size--;

    // Update current frame pointer
    if (vm->call_stack_size > 0) {
      vm->current_frame = &vm->call_stack[vm->call_stack_size - 1];
    } else {
      vm->current_frame = NULL;
    }

    // Push return value onto stack
    // push() retains the value (increments refcount), so we release our
    // local reference after pushing
    PUSH_OR_RETURN_WITH_CLEANUP(vm, return_value, value_release(return_value););
    value_release(return_value); // Release our reference, stack now owns it
  } else {
    // Top-level return (shouldn't happen in normal code)
    // push() retains the value (increments refcount), so we release our
    // local reference after pushing
    PUSH_OR_RETURN_WITH_CLEANUP(vm, return_value, value_release(return_value););
    value_release(return_value); // Release our reference, stack now owns it
  }

  return 0;
}

// Execute bytecode
/**
 * @brief Execute bytecode on the virtual machine
 *
 * Main execution loop. Reads instructions from bytecode and executes them
 * using a stack-based model. Handles all instruction types including:
 * - Stack operations (push, pop)
 * - Variable operations (load, store)
 * - Arithmetic and comparison operations
 * - Control flow (jumps, conditionals)
 * - Function calls and returns
 * - Built-in function invocations
 *
 * @param vm VM instance to execute on
 * @param bytecode Compiled bytecode to execute
 * @return 0 on success, negative error code on failure
 */
int vm_execute(KronosVM *vm, Bytecode *bytecode) {
  if (!vm) {
    return -(int)KRONOS_ERR_INVALID_ARGUMENT;
  }
  if (!bytecode) {
    return vm_error(vm, KRONOS_ERR_INVALID_ARGUMENT,
                    "vm_execute: bytecode must not be NULL");
  }

  // Basic bytecode validation to prevent execution of malformed bytecode
  if (!bytecode->code && bytecode->count > 0) {
    return vm_error(
        vm, KRONOS_ERR_INVALID_ARGUMENT,
        "vm_execute: bytecode has non-zero count but NULL code pointer");
  }
  if (bytecode->code && bytecode->count == 0) {
    // Empty bytecode is valid (e.g., empty function body)
    vm->bytecode = bytecode;
    vm->ip = bytecode->code;
    return 0;
  }
  // Validate constants array if present
  if (bytecode->const_count > 0 && !bytecode->constants) {
    return vm_error(vm, KRONOS_ERR_INVALID_ARGUMENT,
                    "vm_execute: bytecode has non-zero const_count but NULL "
                    "constants array");
  }

  vm->bytecode = bytecode;
  vm->ip = bytecode->code;
  // Note: current_frame should be set by the caller for function execution
  // For top-level code, current_frame is NULL

  // Dispatch table mapping opcodes to handler functions
  // Note: OP_BREAK, OP_CONTINUE, OP_MAP_GET, and OP_RETHROW are reserved but
  // never emitted They will be NULL in the table and handled by the error check
  // below
  static const OpcodeHandler dispatch_table[] = {
      [OP_LOAD_CONST] = handle_op_load_const,
      [OP_LOAD_VAR] = handle_op_load_var,
      [OP_STORE_VAR] = handle_op_store_var,
      [OP_PRINT] = handle_op_print,
      [OP_DEBUG] = handle_op_debug,
      [OP_ADD] = handle_op_add,
      [OP_SUB] = handle_op_sub,
      [OP_MUL] = handle_op_mul,
      [OP_DIV] = handle_op_div,
      [OP_MOD] = handle_op_mod,
      [OP_NEG] = handle_op_neg,
      [OP_EQ] = handle_op_eq,
      [OP_NEQ] = handle_op_neq,
      [OP_GT] = handle_op_gt,
      [OP_LT] = handle_op_lt,
      [OP_GTE] = handle_op_gte,
      [OP_LTE] = handle_op_lte,
      [OP_AND] = handle_op_and,
      [OP_OR] = handle_op_or,
      [OP_NOT] = handle_op_not,
      [OP_JUMP] = handle_op_jump,
      [OP_JUMP_IF_FALSE] = handle_op_jump_if_false,
      [OP_BREAK] = NULL,    // Reserved, never emitted
      [OP_CONTINUE] = NULL, // Reserved, never emitted
      [OP_DEFINE_FUNC] = handle_op_define_func,
      [OP_CALL_FUNC] = handle_op_call_func,
      [OP_RETURN_VAL] = handle_op_return_val,
      [OP_POP] = handle_op_pop,
      [OP_LIST_NEW] = handle_op_list_new,
      [OP_LIST_GET] = handle_op_list_get,
      [OP_LIST_SET] = handle_op_list_set,
      [OP_LIST_APPEND] = handle_op_list_append,
      [OP_LIST_LEN] = handle_op_list_len,
      [OP_LIST_SLICE] = handle_op_list_slice,
      [OP_LIST_ITER] = handle_op_list_iter,
      [OP_LIST_NEXT] = handle_op_list_next,
      [OP_RANGE_NEW] = handle_op_range_new,
      [OP_MAP_NEW] = handle_op_map_new,
      [OP_MAP_SET] = handle_op_map_set,
      [OP_MAP_GET] = NULL, // Reserved, never emitted (uses OP_LIST_GET)
      [OP_DELETE] = handle_op_delete,
      [OP_TRY_ENTER] = handle_op_try_enter,
      [OP_TRY_EXIT] = handle_op_try_exit,
      [OP_CATCH] = handle_op_catch,
      [OP_FINALLY] = handle_op_finally,
      [OP_THROW] = handle_op_throw,
      [OP_RETHROW] = NULL, // Reserved, never emitted
      [OP_IMPORT] = handle_op_import,
      [OP_FORMAT_VALUE] = handle_op_format_value,
      [OP_MAKE_FUNCTION] = handle_op_make_function,
      [OP_CALL_VALUE] = handle_op_call_value,
      [OP_TUPLE_NEW] = handle_op_tuple_new,
      [OP_UNPACK] = handle_op_unpack,
      [OP_HALT] = handle_op_halt,
  };

  bool handling_exception = false;

  while (1) {
    // Check for exceptions before executing next instruction
    // Only check if we're not already handling an exception (to avoid infinite
    // loop)
    if (vm->last_error_code != KRONOS_OK && !handling_exception) {
      if (handle_exception_if_any(vm)) {
        // We're now handling the exception - set flag to allow OP_CATCH to run
        handling_exception = true;
        continue; // Jump to handler, next iteration will execute OP_CATCH
      } else {
        // No handler - propagate the error and stop execution
        return vm_propagate_error(vm, vm->last_error_code);
      }
    }
    // Reset handling_exception after we've executed an instruction
    // This allows OP_CATCH to check for errors and match them
    if (handling_exception) {
      // We're in exception handling mode - don't reset yet, let OP_CATCH handle
      // it Reset will happen after OP_CATCH clears the error
    } else {
      // Normal execution - reset flag (redundant but safe)
      handling_exception = false;
    }

    uint8_t instruction = read_byte(vm);

    // Check for error state after read_byte (it may return OP_HALT on error)
    // If read_byte() encountered an error, it sets vm->last_error_message
    // However, if we're handling an exception (handling_exception is true),
    // the error from OP_THROW is expected and we should continue to execute
    // OP_CATCH
    if (vm->last_error_message && !handling_exception) {
      return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
    }
    // If handling_exception is true, vm->last_error_message is from OP_THROW
    // and we should continue to execute OP_CATCH to handle it

    // Dispatch to handler function using dispatch table.
    // The dispatch table uses designated initializers, so its size is
    // determined by the highest opcode value (currently OP_HALT).
    if (instruction > OP_HALT || dispatch_table[instruction] == NULL) {
      // Unknown or unhandled opcode
      return vm_errorf(
          vm, KRONOS_ERR_INTERNAL,
          "Unknown bytecode instruction: %d (this is a compiler bug)",
          instruction);
    }

    int result = dispatch_table[instruction](vm);
    if (result != 0) {
      return result;
    }

    // Check if we just executed OP_RETURN_VAL for a module function call
    // If so, break out of the loop to avoid reading past the function bytecode
    if (instruction == OP_RETURN_VAL && vm->call_stack_size > 0) {
      CallFrame *frame = &vm->call_stack[vm->call_stack_size - 1];
      if (frame->return_ip == NULL && frame->return_bytecode == NULL) {
        // Module function returned - exit the loop
        break;
      }
    }

    // Check if handler set an error but returned 0 (e.g., OP_THROW)
    // If an exception handler exists, handle it immediately. Otherwise,
    // propagate the error.
    if (vm->last_error_message) {
      // Check if there's an exception handler that can catch this error
      if (vm->exception_handler_count > 0 && !handling_exception) {
        // Exception handler exists - jump to the catch handler immediately
        if (handle_exception_if_any(vm)) {
          // We've jumped to the handler - set flag so exception check at loop
          // start doesn't jump again, but allow OP_CATCH to execute
          handling_exception = true;
          continue; // Go to loop start, but exception check will be skipped
        }
      }
      // No exception handler or already handling - propagate the error and stop
      // execution
      return vm_propagate_error(vm, vm->last_error_code);
    }

    // OP_HALT returns 0 to indicate successful halt - exit the loop
    // Use break to exit the while loop, then return 0 at the end
    if (instruction == OP_HALT) {
      break;
    }
  }

  return 0;
}
