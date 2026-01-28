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
 * @brief Comparison function for qsort on KronosValue arrays
 *
 * Compares two KronosValue pointers for sorting. The comparison type
 * is determined from the values themselves (thread-safe, no global state).
 * This is safe because sort() validates all items are the same type before
 * calling qsort.
 *
 * @param a Pointer to first KronosValue*
 * @param b Pointer to second KronosValue*
 * @return negative if a < b, 0 if equal, positive if a > b
 */
static int sort_compare_values(const void *a, const void *b) {
  const KronosValue *val_a = *(const KronosValue *const *)a;
  const KronosValue *val_b = *(const KronosValue *const *)b;

  // Determine comparison type from values (all items are same type per
  // validation)
  if (val_a->type == VAL_NUMBER) {
    double diff = val_a->as.number - val_b->as.number;
    return (diff > 0) - (diff < 0);
  } else if (val_a->type == VAL_STRING) {
    return strcmp(val_a->as.string.data, val_b->as.string.data);
  }
  // Should not reach here if validation is correct
  return 0;
}

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

// Forward declarations for functions used in call_function_value
static KronosValue *pop(KronosVM *vm);
static int vm_propagate_error(KronosVM *vm, KronosErrorCode fallback);

// Forward declaration for call_function_value (needed by handle_op_call_func)
static int call_function_value(KronosVM *vm, KronosValue *func_val,
                               const char *func_name, uint8_t arg_count);

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
static int call_function_value(KronosVM *vm, KronosValue *func_val,
                               const char *func_name, uint8_t arg_count) {
  // Validate argument count
  if (arg_count != func_val->as.function.arity) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function '%s' expects %d argument%s, but got %d",
                     func_name, func_val->as.function.arity,
                     func_val->as.function.arity == 1 ? "" : "s", arg_count);
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

  // Set parameters as local variables
  for (int i = 0; i < arg_count; i++) {
    const char *param_name = func_val->as.function.param_names[i];
    int arg_status = vm_set_local(vm, frame, param_name, args[i], true, NULL);
    value_release(args[i]);
    if (arg_status != 0) {
      for (size_t j = i + 1; j < arg_count; j++) {
        value_release(args[j]);
      }
      free(args);
      // Clean up locals
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

  // Free bytecode structure
  free(func->bytecode.code);
  for (size_t i = 0; i < func->bytecode.const_count; i++) {
    value_release(func->bytecode.constants[i]);
  }
  free(func->bytecode.constants);

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

  // Add to array (for iteration/debugging)
  vm->functions[vm->function_count++] = func;

  // Add to hash table for O(1) lookup
  if (func->name) {
    size_t index = hash_function_name(func->name);

    // Linear probing to find empty slot
    for (size_t i = 0; i < FUNCTIONS_MAX; i++) {
      size_t idx = (index + i) % FUNCTIONS_MAX;
      if (!vm->function_hash[idx]) {
        // Found empty slot
        vm->function_hash[idx] = func;
        return 0;
      }
      // Check if function already exists (shouldn't happen, but be safe)
      if (vm->function_hash[idx]->name &&
          strcmp(vm->function_hash[idx]->name, func->name) == 0) {
        // Function already exists - this is an error
        return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                         "Function '%s' is already defined", func->name);
      }
    }

    // Hash table full (shouldn't happen if FUNCTIONS_MAX is respected)
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function hash table is full (internal error)");
  }

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
    free(resolved_path);
    return vm_errorf(vm, KRONOS_ERR_NOT_FOUND, "Failed to open module file: %s",
                     file_path);
  }

  // Determine file size
  if (fseek(file, 0, SEEK_END) != 0) {
    free(resolved_path);
    fclose(file);
    return vm_errorf(vm, KRONOS_ERR_IO, "Failed to seek to end of file: %s",
                     resolved_path);
  }

  long size = ftell(file);
  if (size < 0) {
    free(resolved_path);
    fclose(file);
    return vm_errorf(vm, KRONOS_ERR_IO, "Failed to determine file size: %s",
                     resolved_path);
  }

  if ((uintmax_t)size > (uintmax_t)(SIZE_MAX - 1)) {
    free(resolved_path);
    fclose(file);
    return vm_errorf(vm, KRONOS_ERR_IO, "File too large to read: %s",
                     resolved_path);
  }

  if (fseek(file, 0, SEEK_SET) != 0) {
    free(resolved_path);
    fclose(file);
    return vm_errorf(vm, KRONOS_ERR_IO, "Failed to seek to start of file: %s",
                     resolved_path);
  }

  // Allocate buffer
  size_t length = (size_t)size;
  char *source = malloc(length + 1);
  if (!source) {
    free(resolved_path);
    fclose(file);
    return vm_error(vm, KRONOS_ERR_INTERNAL,
                    "Failed to allocate memory for module file");
  }

  size_t read_size = fread(source, 1, length, file);
  if (ferror(file) || (read_size < length && !feof(file))) {
    char *path_copy = strdup(resolved_path);
    free(source);
    free(resolved_path);
    fclose(file);
    int err = vm_errorf(vm, KRONOS_ERR_IO, "Failed to read module file: %s",
                        path_copy);
    free(path_copy);
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

// Built-in function handler type
// Takes VM and argument count, returns 0 on success, negative error code on
// failure
typedef int (*BuiltinHandler)(KronosVM *vm, uint8_t arg_count);

// Forward declarations for all opcode handlers
static int handle_op_load_const(KronosVM *vm);
static int handle_op_load_var(KronosVM *vm);
static int handle_op_store_var(KronosVM *vm);
static int handle_op_print(KronosVM *vm);
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

// Forward declarations for built-in function handlers
static int builtin_read_file(KronosVM *vm, uint8_t arg_count);
static int builtin_add(KronosVM *vm, uint8_t arg_count);
static int builtin_subtract(KronosVM *vm, uint8_t arg_count);
static int builtin_multiply(KronosVM *vm, uint8_t arg_count);
static int builtin_divide(KronosVM *vm, uint8_t arg_count);
static int builtin_len(KronosVM *vm, uint8_t arg_count);
static int builtin_uppercase(KronosVM *vm, uint8_t arg_count);
static int builtin_lowercase(KronosVM *vm, uint8_t arg_count);
static int builtin_trim(KronosVM *vm, uint8_t arg_count);
static int builtin_split(KronosVM *vm, uint8_t arg_count);
static int builtin_join(KronosVM *vm, uint8_t arg_count);
static int builtin_to_string(KronosVM *vm, uint8_t arg_count);
static int builtin_contains(KronosVM *vm, uint8_t arg_count);
static int builtin_starts_with(KronosVM *vm, uint8_t arg_count);
static int builtin_ends_with(KronosVM *vm, uint8_t arg_count);
static int builtin_replace(KronosVM *vm, uint8_t arg_count);
static int builtin_sqrt(KronosVM *vm, uint8_t arg_count);
static int builtin_power(KronosVM *vm, uint8_t arg_count);
static int builtin_abs(KronosVM *vm, uint8_t arg_count);
static int builtin_round(KronosVM *vm, uint8_t arg_count);
static int builtin_floor(KronosVM *vm, uint8_t arg_count);
static int builtin_ceil(KronosVM *vm, uint8_t arg_count);
static int builtin_rand(KronosVM *vm, uint8_t arg_count);
static int builtin_min(KronosVM *vm, uint8_t arg_count);
static int builtin_max(KronosVM *vm, uint8_t arg_count);
static int builtin_to_number(KronosVM *vm, uint8_t arg_count);
static int builtin_to_bool(KronosVM *vm, uint8_t arg_count);
static int builtin_reverse(KronosVM *vm, uint8_t arg_count);
static int builtin_sort(KronosVM *vm, uint8_t arg_count);
static int builtin_write_file(KronosVM *vm, uint8_t arg_count);
static int builtin_read_lines(KronosVM *vm, uint8_t arg_count);
static int builtin_file_exists(KronosVM *vm, uint8_t arg_count);
static int builtin_list_files(KronosVM *vm, uint8_t arg_count);
static int builtin_join_path(KronosVM *vm, uint8_t arg_count);
static int builtin_dirname(KronosVM *vm, uint8_t arg_count);
static int builtin_basename(KronosVM *vm, uint8_t arg_count);
static int builtin_regex_match(KronosVM *vm, uint8_t arg_count);
static int builtin_regex_search(KronosVM *vm, uint8_t arg_count);
static int builtin_regex_findall(KronosVM *vm, uint8_t arg_count);

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

// Built-in function implementations
static int builtin_read_file(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1)
    return vm_errorf(vm, KRONOS_ERR_RUNTIME, "Expected 1 argument");
  KronosValue *path_val;
  POP_OR_RETURN(vm, path_val);
  if (path_val->type != VAL_STRING) {
    value_release(path_val);
    return vm_errorf(vm, KRONOS_ERR_RUNTIME, "Path must be a string");
  }
  FILE *file = portable_fopen(path_val->as.string.data, "rb");
  if (!file) {
    value_release(path_val);
    return vm_errorf(vm, KRONOS_ERR_RUNTIME, "Could not open file");
  }
  if (fseek(file, 0L, SEEK_END) != 0) {
    fclose(file);
    value_release(path_val);
    return vm_errorf(vm, KRONOS_ERR_RUNTIME, "Failed to seek to end of file");
  }
  long fsize = ftell(file);
  if (fsize < 0) {
    fclose(file);
    value_release(path_val);
    return vm_errorf(vm, KRONOS_ERR_RUNTIME, "Failed to get file size");
  }
  rewind(file);
  if ((unsigned long)fsize > SIZE_MAX - 1) {
    fclose(file);
    value_release(path_val);
    return vm_errorf(vm, KRONOS_ERR_RUNTIME, "File too large");
  }
  char *buff = malloc(fsize + 1);
  if (!buff) {
    fclose(file);
    value_release(path_val);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate memory");
  }
  size_t bytes_read = fread(buff, 1, fsize, file);
  if (bytes_read != (size_t)fsize) {
    free(buff);
    fclose(file);
    value_release(path_val);
    return vm_errorf(vm, KRONOS_ERR_RUNTIME, "Failed to read file");
  }
  buff[bytes_read] = '\0';
  fclose(file);
  KronosValue *res = value_new_string(buff, bytes_read);
  free(buff);
  PUSH_OR_RETURN_WITH_CLEANUP(vm, res, value_release(res);
                              value_release(path_val););
  value_release(res);
  value_release(path_val);
  return 0;
}

// Built-in function implementations (extracted from handle_op_call_func)
static int builtin_add(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 2) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'add' expects 2 arguments, got %d", arg_count);
  }
  KronosValue *b;

  POP_OR_RETURN(vm, b);
  KronosValue *a;

  POP_OR_RETURN_WITH_CLEANUP(vm, a, value_release(b));
  if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
    KronosValue *result = value_new_number(a->as.number + b->as.number);
    PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                                value_release(a); value_release(b););
    value_release(result);
  } else {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'add' requires both arguments to be numbers");
    value_release(a);
    value_release(b);
    return err;
  }
  value_release(a);
  value_release(b);
  return 0;
}

static int builtin_subtract(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 2) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'subtract' expects 2 arguments, got %d",
                     arg_count);
  }
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
    int err =
        vm_errorf(vm, KRONOS_ERR_RUNTIME,
                  "Function 'subtract' requires both arguments to be numbers");
    value_release(a);
    value_release(b);
    return err;
  }
  value_release(a);
  value_release(b);
  return 0;
}

static int builtin_multiply(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 2) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'multiply' expects 2 arguments, got %d",
                     arg_count);
  }
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
    int err =
        vm_errorf(vm, KRONOS_ERR_RUNTIME,
                  "Function 'multiply' requires both arguments to be numbers");
    value_release(a);
    value_release(b);
    return err;
  }
  value_release(a);
  value_release(b);
  return 0;
}

static int builtin_divide(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 2) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'divide' expects 2 arguments, got %d",
                     arg_count);
  }
  KronosValue *b;

  POP_OR_RETURN(vm, b);
  KronosValue *a;

  POP_OR_RETURN_WITH_CLEANUP(vm, a, value_release(b));
  if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
    if (b->as.number == 0.0) {
      value_release(a);
      value_release(b);
      return vm_error(vm, KRONOS_ERR_RUNTIME, "Division by zero");
    }
    KronosValue *result = value_new_number(a->as.number / b->as.number);
    PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                                value_release(a); value_release(b););
    value_release(result);
  } else {
    int err =
        vm_errorf(vm, KRONOS_ERR_RUNTIME,
                  "Function 'divide' requires both arguments to be numbers");
    value_release(a);
    value_release(b);
    return err;
  }
  value_release(a);
  value_release(b);
  return 0;
}

static int builtin_len(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'len' expects 1 argument, got %d", arg_count);
  }
  KronosValue *arg;

  POP_OR_RETURN(vm, arg);
  if (arg->type == VAL_LIST) {
    KronosValue *result = value_new_number((double)arg->as.list.count);
    PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                                value_release(arg););
    value_release(result);
  } else if (arg->type == VAL_STRING) {
    KronosValue *result = value_new_number((double)arg->as.string.length);
    PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                                value_release(arg););
    value_release(result);
  } else if (arg->type == VAL_RANGE) {
    // Calculate range length: number of values in range
    double start = arg->as.range.start;
    double end = arg->as.range.end;
    double step = arg->as.range.step;

    if (step == 0.0) {
      value_release(arg);
      return vm_error(vm, KRONOS_ERR_RUNTIME, "Range step cannot be zero");
    }

    // Calculate number of steps
    double diff = end - start;
    double count = floor((diff / step)) + 1.0;

    // Ensure count is non-negative
    if (count < 0) {
      count = 0;
    }

    KronosValue *result = value_new_number(count);
    PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                                value_release(arg););
    value_release(result);
  } else {
    int err =
        vm_errorf(vm, KRONOS_ERR_RUNTIME,
                  "Function 'len' requires a list, string, or range argument");
    value_release(arg);
    return err;
  }
  value_release(arg);
  return 0;
}

static int builtin_uppercase(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'uppercase' expects 1 argument, got %d",
                     arg_count);
  }
  KronosValue *arg;

  POP_OR_RETURN(vm, arg);
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
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(arg););
  value_release(result);
  value_release(arg);
  return 0;
}

static int builtin_lowercase(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'lowercase' expects 1 argument, got %d",
                     arg_count);
  }
  KronosValue *arg;

  POP_OR_RETURN(vm, arg);
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
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(arg););
  value_release(result);
  value_release(arg);
  return 0;
}

static int builtin_trim(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'trim' expects 1 argument, got %d", arg_count);
  }
  KronosValue *arg;

  POP_OR_RETURN(vm, arg);
  if (arg->type != VAL_STRING) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'trim' requires a string argument");
    value_release(arg);
    return err;
  }

  // Find start (skip leading whitespace)
  size_t start = 0;
  while (start < arg->as.string.length &&
         isspace((unsigned char)arg->as.string.data[start])) {
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
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(arg););
  value_release(result);
  value_release(arg);
  return 0;
}

static int builtin_split(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 2) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'split' expects 2 arguments, got %d", arg_count);
  }
  KronosValue *delim;

  POP_OR_RETURN(vm, delim);
  KronosValue *str;

  POP_OR_RETURN_WITH_CLEANUP(vm, str, value_release(delim));
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

  // Handle empty delimiter (split into characters)
  if (delim->as.string.length == 0) {
    for (size_t i = 0; i < str->as.string.length; i++) {
      char ch = str->as.string.data[i];
      KronosValue *char_str = value_new_string(&ch, 1);
      if (!char_str) {
        value_release(result);
        value_release(str);
        value_release(delim);
        return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string");
      }
      // Grow list if needed
      if (result->as.list.count >= result->as.list.capacity) {
        size_t new_cap = result->as.list.capacity * 2;
        KronosValue **new_items =
            realloc(result->as.list.items, sizeof(KronosValue *) * new_cap);
        if (!new_items) {
          value_release(char_str);
          value_release(result);
          value_release(str);
          value_release(delim);
          return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to grow list");
        }
        result->as.list.items = new_items;
        result->as.list.capacity = new_cap;
      }
      value_retain(char_str);
      result->as.list.items[result->as.list.count++] = char_str;
      value_release(char_str);
    }
  } else {
    // Split by delimiter
    const char *str_data = str->as.string.data;
    size_t str_len = str->as.string.length;
    const char *delim_data = delim->as.string.data;
    size_t delim_len = delim->as.string.length;

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

      if (found) {
        // Extract substring from start to pos
        size_t substr_len = pos - start;
        char *substr = malloc(substr_len + 1);
        if (!substr) {
          value_release(result);
          value_release(str);
          value_release(delim);
          return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate memory");
        }
        memcpy(substr, str_data + start, substr_len);
        substr[substr_len] = '\0';

        KronosValue *substr_val = value_new_string(substr, substr_len);
        free(substr);
        if (!substr_val) {
          value_release(result);
          value_release(str);
          value_release(delim);
          return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string");
        }
        // Grow list if needed
        if (result->as.list.count >= result->as.list.capacity) {
          size_t new_cap = result->as.list.capacity * 2;
          KronosValue **new_items =
              realloc(result->as.list.items, sizeof(KronosValue *) * new_cap);
          if (!new_items) {
            value_release(substr_val);
            value_release(result);
            value_release(str);
            value_release(delim);
            return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to grow list");
          }
          result->as.list.items = new_items;
          result->as.list.capacity = new_cap;
        }
        value_retain(substr_val);
        result->as.list.items[result->as.list.count++] = substr_val;
        value_release(substr_val);
        start = pos + delim_len;
      } else {
        // No more delimiters, add remaining string
        size_t substr_len = str_len - start;
        char *substr = malloc(substr_len + 1);
        if (!substr) {
          value_release(result);
          value_release(str);
          value_release(delim);
          return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate memory");
        }
        memcpy(substr, str_data + start, substr_len);
        substr[substr_len] = '\0';

        KronosValue *substr_val = value_new_string(substr, substr_len);
        free(substr);
        if (!substr_val) {
          value_release(result);
          value_release(str);
          value_release(delim);
          return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string");
        }
        // Grow list if needed
        if (result->as.list.count >= result->as.list.capacity) {
          size_t new_cap = result->as.list.capacity * 2;
          KronosValue **new_items =
              realloc(result->as.list.items, sizeof(KronosValue *) * new_cap);
          if (!new_items) {
            value_release(substr_val);
            value_release(result);
            value_release(str);
            value_release(delim);
            return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to grow list");
          }
          result->as.list.items = new_items;
          result->as.list.capacity = new_cap;
        }
        value_retain(substr_val);
        result->as.list.items[result->as.list.count++] = substr_val;
        value_release(substr_val);
        break;
      }
    }
  }

  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(str); value_release(delim););
  value_release(result);
  value_release(str);
  value_release(delim);
  return 0;
}

static int builtin_join(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 2) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'join' expects 2 arguments, got %d", arg_count);
  }
  KronosValue *delim;

  POP_OR_RETURN(vm, delim);
  KronosValue *list;

  POP_OR_RETURN_WITH_CLEANUP(vm, list, value_release(delim));
  if (list->type != VAL_LIST || delim->type != VAL_STRING) {
    int err =
        vm_errorf(vm, KRONOS_ERR_RUNTIME,
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
      return vm_error(vm, KRONOS_ERR_RUNTIME,
                      "All list items must be strings for join");
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
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(list); value_release(delim););
  value_release(result);
  value_release(list);
  value_release(delim);
  return 0;
}

static int builtin_to_string(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'to_string' expects 1 argument, got %d",
                     arg_count);
  }
  KronosValue *arg;

  POP_OR_RETURN(vm, arg);

  char *str_buf = NULL;
  size_t str_len = 0;

  if (arg->type == VAL_STRING) {
    // Already a string, just return it
    PUSH_OR_RETURN_WITH_CLEANUP(vm, arg, value_release(arg););
    value_release(arg); // Release the pop reference (push already retained)
    return 0;
  } else if (arg->type == VAL_NUMBER) {
    // Convert number to string
    str_buf = malloc(NUMBER_STRING_BUFFER_SIZE);
    if (!str_buf) {
      value_release(arg);
      return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate memory");
    }

    // Check if it's a whole number
    double intpart;
    double frac = modf(arg->as.number, &intpart);

    if (frac == 0.0 && fabs(arg->as.number) < 1.0e15) {
      str_len = (size_t)snprintf(str_buf, NUMBER_STRING_BUFFER_SIZE, "%.0f",
                                 arg->as.number);
    } else {
      str_len = (size_t)snprintf(str_buf, NUMBER_STRING_BUFFER_SIZE, "%g",
                                 arg->as.number);
    }
  } else if (arg->type == VAL_BOOL) {
    if (arg->as.boolean) {
      str_buf = strdup("true");
      if (!str_buf) {
        value_release(arg);
        return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate memory");
      }
      str_len = 4;
    } else {
      str_buf = strdup("false");
      if (!str_buf) {
        value_release(arg);
        return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate memory");
      }
      str_len = 5;
    }
  } else if (arg->type == VAL_NIL) {
    str_buf = strdup("null");
    if (!str_buf) {
      value_release(arg);
      return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate memory");
    }
    str_len = 4;
  } else {
    value_release(arg);
    return vm_errorf(vm, KRONOS_ERR_RUNTIME, "Cannot convert type to string");
  }

  KronosValue *result = value_new_string(str_buf, str_len);
  free(str_buf); // Always free our buffer (value_new_string copies it)
  if (!result) {
    value_release(arg);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string value");
  }
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(arg););
  value_release(result);
  value_release(arg);
  return 0;
}

static int builtin_contains(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 2) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'contains' expects 2 arguments, got %d",
                     arg_count);
  }
  KronosValue *substring;

  POP_OR_RETURN(vm, substring);
  KronosValue *str;

  POP_OR_RETURN_WITH_CLEANUP(vm, str, value_release(substring));
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
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(str); value_release(substring););
  value_release(result);
  value_release(str);
  value_release(substring);
  return 0;
}

static int builtin_starts_with(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 2) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'starts_with' expects 2 arguments, got %d",
                     arg_count);
  }
  KronosValue *prefix;

  POP_OR_RETURN(vm, prefix);
  KronosValue *str;

  POP_OR_RETURN_WITH_CLEANUP(vm, str, value_release(prefix));
  if (str->type != VAL_STRING || prefix->type != VAL_STRING) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'starts_with' requires two string arguments");
    value_release(str);
    value_release(prefix);
    return err;
  }

  bool starts = false;
  if (prefix->as.string.length <= str->as.string.length) {
    starts = (memcmp(str->as.string.data, prefix->as.string.data,
                     prefix->as.string.length) == 0);
  }
  KronosValue *result = value_new_bool(starts);
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(str); value_release(prefix););
  value_release(result);
  value_release(str);
  value_release(prefix);
  return 0;
}

static int builtin_ends_with(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 2) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'ends_with' expects 2 arguments, got %d",
                     arg_count);
  }
  KronosValue *suffix;

  POP_OR_RETURN(vm, suffix);
  KronosValue *str;

  POP_OR_RETURN_WITH_CLEANUP(vm, str, value_release(suffix));
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
    ends = (memcmp(str->as.string.data + start_pos, suffix->as.string.data,
                   suffix->as.string.length) == 0);
  }
  KronosValue *result = value_new_bool(ends);
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(str); value_release(suffix););
  value_release(result);
  value_release(str);
  value_release(suffix);
  return 0;
}

static int builtin_replace(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 3) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'replace' expects 3 arguments, got %d",
                     arg_count);
  }
  KronosValue *new_str;

  POP_OR_RETURN(vm, new_str);
  KronosValue *old_str;

  POP_OR_RETURN_WITH_CLEANUP(vm, old_str, value_release(new_str));
  KronosValue *str;
  POP_OR_RETURN_WITH_CLEANUP(vm, str, value_release(old_str);
                             value_release(new_str));
  if (str->type != VAL_STRING || old_str->type != VAL_STRING ||
      new_str->type != VAL_STRING) {
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
    PUSH_OR_RETURN_WITH_CLEANUP(vm, str, value_release(str);
                                value_release(old_str);
                                value_release(new_str););
    value_release(str);
    value_release(old_str);
    value_release(new_str);
    return 0;
  }

  // Calculate maximum possible result size
  size_t str_len = str->as.string.length;
  size_t old_len = old_str->as.string.length;
  size_t new_len = new_str->as.string.length;
  size_t max_result_len = str_len;

  if (new_len > old_len) {
    size_t max_occurrences = str_len / old_len;
    size_t growth_per_occurrence = new_len - old_len;

    if (max_occurrences > 0 &&
        growth_per_occurrence > SIZE_MAX / max_occurrences) {
      max_result_len = SIZE_MAX;
    } else {
      size_t total_growth = max_occurrences * growth_per_occurrence;
      if (total_growth > SIZE_MAX - str_len) {
        max_result_len = SIZE_MAX;
      } else {
        max_result_len = str_len + total_growth;
      }
    }
  }

  // Check for overflow before malloc
  if (max_result_len > SIZE_MAX - 1) {
    value_release(str);
    value_release(old_str);
    value_release(new_str);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Result string too large");
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
    memcpy(result_buf + result_len, new_str->as.string.data,
           new_str->as.string.length);
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
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(str); value_release(old_str);
                              value_release(new_str););
  value_release(result);
  value_release(str);
  value_release(old_str);
  value_release(new_str);
  return 0;
}

static int builtin_sqrt(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'sqrt' expects 1 argument, got %d", arg_count);
  }
  KronosValue *arg;

  POP_OR_RETURN(vm, arg);
  if (arg->type != VAL_NUMBER) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'sqrt' requires a number argument");
    value_release(arg);
    return err;
  }
  if (arg->as.number < 0) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'sqrt' requires a non-negative number");
    value_release(arg);
    return err;
  }
  KronosValue *result = value_new_number(sqrt(arg->as.number));
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(arg););
  value_release(result);
  value_release(arg);
  return 0;
}

static int builtin_power(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 2) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'power' expects 2 arguments, got %d", arg_count);
  }
  KronosValue *exponent;

  POP_OR_RETURN(vm, exponent);
  KronosValue *base;

  POP_OR_RETURN_WITH_CLEANUP(vm, base, value_release(exponent));
  if (base->type != VAL_NUMBER || exponent->type != VAL_NUMBER) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'power' requires two number arguments");
    value_release(base);
    value_release(exponent);
    return err;
  }
  KronosValue *result =
      value_new_number(pow(base->as.number, exponent->as.number));
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(base); value_release(exponent););
  value_release(result);
  value_release(base);
  value_release(exponent);
  return 0;
}

static int builtin_abs(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'abs' expects 1 argument, got %d", arg_count);
  }
  KronosValue *arg;

  POP_OR_RETURN(vm, arg);
  if (arg->type != VAL_NUMBER) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'abs' requires a number argument");
    value_release(arg);
    return err;
  }
  KronosValue *result = value_new_number(fabs(arg->as.number));
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(arg););
  value_release(result);
  value_release(arg);
  return 0;
}

static int builtin_round(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'round' expects 1 argument, got %d", arg_count);
  }
  KronosValue *arg;

  POP_OR_RETURN(vm, arg);
  if (arg->type != VAL_NUMBER) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'round' requires a number argument");
    value_release(arg);
    return err;
  }
  KronosValue *result = value_new_number(round(arg->as.number));
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(arg););
  value_release(result);
  value_release(arg);
  return 0;
}

static int builtin_floor(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'floor' expects 1 argument, got %d", arg_count);
  }
  KronosValue *arg;

  POP_OR_RETURN(vm, arg);
  if (arg->type != VAL_NUMBER) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'floor' requires a number argument");
    value_release(arg);
    return err;
  }
  KronosValue *result = value_new_number(floor(arg->as.number));
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(arg););
  value_release(result);
  value_release(arg);
  return 0;
}

static int builtin_ceil(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'ceil' expects 1 argument, got %d", arg_count);
  }
  KronosValue *arg;

  POP_OR_RETURN(vm, arg);
  if (arg->type != VAL_NUMBER) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'ceil' requires a number argument");
    value_release(arg);
    return err;
  }
  KronosValue *result = value_new_number(ceil(arg->as.number));
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(arg););
  value_release(result);
  value_release(arg);
  return 0;
}

static int builtin_rand(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 0) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'rand' expects 0 arguments, got %d", arg_count);
  }
  // Generate random number between 0.0 and 1.0
  double random_val = (double)rand() / (double)RAND_MAX;
  KronosValue *result = value_new_number(random_val);
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result););
  value_release(result);
  return 0;
}

static int builtin_min(KronosVM *vm, uint8_t arg_count) {
  if (arg_count < 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'min' expects at least 1 argument, got %d",
                     arg_count);
  }

  // Pop all arguments
  KronosValue **args = malloc(sizeof(KronosValue *) * arg_count);
  if (!args) {
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate memory");
  }

  for (int i = arg_count - 1; i >= 0; i--) {
    args[i] = pop(vm);
    if (!args[i]) {
      for (int j = i + 1; j < arg_count; j++) {
        value_release(args[j]);
      }
      free(args);
      return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
    }
  }

  // Validate all are numbers
  for (size_t i = 0; i < arg_count; i++) {
    if (args[i]->type != VAL_NUMBER) {
      int err =
          vm_errorf(vm, KRONOS_ERR_RUNTIME,
                    "Function 'min' requires all arguments to be numbers");
      for (size_t j = 0; j < arg_count; j++) {
        value_release(args[j]);
      }
      free(args);
      return err;
    }
  }

  // Find minimum
  double min_val = args[0]->as.number;
  for (size_t i = 1; i < arg_count; i++) {
    if (args[i]->as.number < min_val) {
      min_val = args[i]->as.number;
    }
  }

  // Release all arguments
  for (size_t i = 0; i < arg_count; i++) {
    value_release(args[i]);
  }
  free(args);

  KronosValue *result = value_new_number(min_val);
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result););
  value_release(result);
  return 0;
}

static int builtin_max(KronosVM *vm, uint8_t arg_count) {
  if (arg_count < 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'max' expects at least 1 argument, got %d",
                     arg_count);
  }

  // Pop all arguments
  KronosValue **args = malloc(sizeof(KronosValue *) * arg_count);
  if (!args) {
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate memory");
  }

  for (int i = arg_count - 1; i >= 0; i--) {
    args[i] = pop(vm);
    if (!args[i]) {
      for (int j = i + 1; j < arg_count; j++) {
        value_release(args[j]);
      }
      free(args);
      return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
    }
  }

  // Validate all are numbers
  for (size_t i = 0; i < arg_count; i++) {
    if (args[i]->type != VAL_NUMBER) {
      int err =
          vm_errorf(vm, KRONOS_ERR_RUNTIME,
                    "Function 'max' requires all arguments to be numbers");
      for (size_t j = 0; j < arg_count; j++) {
        value_release(args[j]);
      }
      free(args);
      return err;
    }
  }

  // Find maximum
  double max_val = args[0]->as.number;
  for (size_t i = 1; i < arg_count; i++) {
    if (args[i]->as.number > max_val) {
      max_val = args[i]->as.number;
    }
  }

  // Release all arguments
  for (size_t i = 0; i < arg_count; i++) {
    value_release(args[i]);
  }
  free(args);

  KronosValue *result = value_new_number(max_val);
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result););
  value_release(result);
  return 0;
}

static int builtin_to_number(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'to_number' expects 1 argument, got %d",
                     arg_count);
  }
  KronosValue *arg;

  POP_OR_RETURN(vm, arg);

  if (arg->type == VAL_NUMBER) {
    // Already a number, just return it
    PUSH_OR_RETURN_WITH_CLEANUP(vm, arg, value_release(arg););
    value_release(arg); // Release the pop reference (push already retained)
    return 0;
  } else if (arg->type == VAL_STRING) {
    // Try to parse string as number
    char *endptr;
    double num = strtod(arg->as.string.data, &endptr);
    // Check if conversion was successful (endptr should point to end of string)
    if (*endptr != '\0' && *endptr != '\n' && *endptr != '\r') {
      int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                          "Cannot convert string to number: '%s'",
                          arg->as.string.data);
      value_release(arg);
      return err;
    }
    value_release(arg);
    KronosValue *result = value_new_number(num);
    PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result););
    value_release(result);
    return 0;
  } else {
    int err =
        vm_errorf(vm, KRONOS_ERR_RUNTIME,
                  "Function 'to_number' requires a string or number argument");
    value_release(arg);
    return err;
  }
}

static int builtin_to_bool(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'to_bool' expects 1 argument, got %d",
                     arg_count);
  }
  KronosValue *arg;

  POP_OR_RETURN(vm, arg);

  bool bool_val = false;
  if (arg->type == VAL_BOOL) {
    bool_val = arg->as.boolean;
  } else if (arg->type == VAL_NUMBER) {
    bool_val = (arg->as.number != 0.0);
  } else if (arg->type == VAL_STRING) {
    bool_val = (arg->as.string.length > 0);
  } else if (arg->type == VAL_LIST) {
    bool_val = (arg->as.list.count > 0);
  } else if (arg->type == VAL_NIL) {
    bool_val = false;
  } else {
    bool_val = true; // Other types are truthy
  }

  value_release(arg);
  KronosValue *result = value_new_bool(bool_val);
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result););
  value_release(result);
  return 0;
}

static int builtin_reverse(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'reverse' expects 1 argument, got %d",
                     arg_count);
  }
  KronosValue *arg;

  POP_OR_RETURN(vm, arg);
  if (arg->type != VAL_LIST) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'reverse' requires a list argument");
    value_release(arg);
    return err;
  }
  // Create new list with reversed items
  KronosValue *result = value_new_list(arg->as.list.count);
  if (!result) {
    value_release(arg);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create list");
  }
  // Copy items in reverse order
  for (int i = (int)arg->as.list.count - 1; i >= 0; i--) {
    value_retain(arg->as.list.items[i]);
    // Grow list if needed
    if (result->as.list.count >= result->as.list.capacity) {
      size_t new_cap = result->as.list.capacity * 2;
      KronosValue **new_items =
          realloc(result->as.list.items, sizeof(KronosValue *) * new_cap);
      if (!new_items) {
        value_release(arg->as.list.items[i]);
        value_release(result);
        value_release(arg);
        return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to grow list");
      }
      result->as.list.items = new_items;
      result->as.list.capacity = new_cap;
    }
    result->as.list.items[result->as.list.count++] = arg->as.list.items[i];
  }
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(arg););
  value_release(result);
  value_release(arg);
  return 0;
}

static int builtin_sort(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'sort' expects 1 argument, got %d", arg_count);
  }
  KronosValue *arg;

  POP_OR_RETURN(vm, arg);
  if (arg->type != VAL_LIST) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'sort' requires a list argument");
    value_release(arg);
    return err;
  }
  // Create new list with sorted items
  KronosValue *result = value_new_list(arg->as.list.count);
  if (!result) {
    value_release(arg);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create list");
  }
  // Copy items
  for (size_t i = 0; i < arg->as.list.count; i++) {
    value_retain(arg->as.list.items[i]);
    // Grow list if needed
    if (result->as.list.count >= result->as.list.capacity) {
      size_t new_cap = result->as.list.capacity * 2;
      KronosValue **new_items =
          realloc(result->as.list.items, sizeof(KronosValue *) * new_cap);
      if (!new_items) {
        value_release(arg->as.list.items[i]);
        value_release(result);
        value_release(arg);
        return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to grow list");
      }
      result->as.list.items = new_items;
      result->as.list.capacity = new_cap;
    }
    result->as.list.items[result->as.list.count++] = arg->as.list.items[i];
  }
  // Sort the new list in-place using qsort (O(n log n) average)
  // First, validate all elements are the same type
  if (result->as.list.count > 0) {
    ValueType first_type = result->as.list.items[0]->type;
    if (first_type != VAL_NUMBER && first_type != VAL_STRING) {
      int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                          "Function 'sort' requires list items to be "
                          "all numbers or all strings");
      value_release(result);
      value_release(arg);
      return err;
    }

    // Check all elements are the same type
    for (size_t i = 1; i < result->as.list.count; i++) {
      if (result->as.list.items[i]->type != first_type) {
        int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                            "Function 'sort' requires list items to be "
                            "all numbers or all strings");
        value_release(result);
        value_release(arg);
        return err;
      }
    }

    // Sort using thread-safe comparison (no global state needed)
    // All items are validated to be the same type, so comparison
    // function can determine type from the values themselves
    qsort(result->as.list.items, result->as.list.count, sizeof(KronosValue *),
          sort_compare_values);
  }
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(arg););
  value_release(result);
  value_release(arg);
  return 0;
}

static int builtin_write_file(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 2) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'write_file' expects 2 arguments, got %d",
                     arg_count);
  }
  KronosValue *content_arg;

  POP_OR_RETURN(vm, content_arg);
  KronosValue *path_arg;

  POP_OR_RETURN_WITH_CLEANUP(vm, path_arg, value_release(content_arg));
  if (path_arg->type != VAL_STRING || content_arg->type != VAL_STRING) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'write_file' requires two string arguments");
    value_release(path_arg);
    value_release(content_arg);
    return err;
  }

  FILE *file = portable_fopen(path_arg->as.string.data, "w");
  if (!file) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Failed to open file '%s' for writing",
                        path_arg->as.string.data);
    value_release(path_arg);
    value_release(content_arg);
    return err;
  }

  size_t bytes_written = fwrite(content_arg->as.string.data, 1,
                                content_arg->as.string.length, file);
  fclose(file);

  if (bytes_written != content_arg->as.string.length) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Failed to write all content to file '%s'",
                        path_arg->as.string.data);
    value_release(path_arg);
    value_release(content_arg);
    return err;
  }

  // Return nil (success)
  KronosValue *result = value_new_nil();
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(path_arg);
                              value_release(content_arg););
  value_release(result);
  value_release(path_arg);
  value_release(content_arg);
  return 0;
}

static int builtin_read_lines(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'read_lines' expects 1 argument, got %d",
                     arg_count);
  }
  KronosValue *path_arg;

  POP_OR_RETURN(vm, path_arg);
  if (path_arg->type != VAL_STRING) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'read_lines' requires a string argument");
    value_release(path_arg);
    return err;
  }

  FILE *file = portable_fopen(path_arg->as.string.data, "r");
  if (!file) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME, "Failed to open file '%s'",
                        path_arg->as.string.data);
    value_release(path_arg);
    return err;
  }

  KronosValue *result = value_new_list(16);
  if (!result) {
    fclose(file);
    value_release(path_arg);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create list");
  }

  char *line = NULL;
  size_t line_len = 0;
  ssize_t read;

  // Use portable getline() implementation for cross-platform compatibility
  while ((read = KRONOS_GETLINE(&line, &line_len, file)) != -1) {
    // Remove trailing newline if present
    if (read > 0 && line[read - 1] == '\n') {
      read--;
      line[read] = '\0';
    }

    KronosValue *line_val = value_new_string(line, (size_t)read);
    if (!line_val) {
      free(line);
      fclose(file);
      value_release(result);
      value_release(path_arg);
      return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string value");
    }

    // Grow list if needed
    if (result->as.list.count >= result->as.list.capacity) {
      size_t new_cap = result->as.list.capacity * 2;
      KronosValue **new_items =
          realloc(result->as.list.items, sizeof(KronosValue *) * new_cap);
      if (!new_items) {
        value_release(line_val);
        free(line);
        fclose(file);
        value_release(result);
        value_release(path_arg);
        return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to grow list");
      }
      result->as.list.items = new_items;
      result->as.list.capacity = new_cap;
    }

    value_retain(line_val);
    result->as.list.items[result->as.list.count++] = line_val;
    value_release(line_val);
  }

  free(line);
  fclose(file);
  value_release(path_arg);

  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result););
  value_release(result);
  return 0;
}

static int builtin_file_exists(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'file_exists' expects 1 argument, got %d",
                     arg_count);
  }
  KronosValue *path_arg;

  POP_OR_RETURN(vm, path_arg);
  if (path_arg->type != VAL_STRING) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'file_exists' requires a string argument");
    value_release(path_arg);
    return err;
  }

  struct stat st;
  int exists = (stat(path_arg->as.string.data, &st) == 0);
  value_release(path_arg);

  KronosValue *result = value_new_bool(exists);
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result););
  value_release(result);
  return 0;
}

static int builtin_list_files(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'list_files' expects 1 argument, got %d",
                     arg_count);
  }
  KronosValue *path_arg;

  POP_OR_RETURN(vm, path_arg);
  if (path_arg->type != VAL_STRING) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'list_files' requires a string argument");
    value_release(path_arg);
    return err;
  }

  DIR *dir = opendir(path_arg->as.string.data);
  if (!dir) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME, "Failed to open directory '%s'",
                        path_arg->as.string.data);
    value_release(path_arg);
    return err;
  }

  KronosValue *result = value_new_list(16);
  if (!result) {
    closedir(dir);
    value_release(path_arg);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create list");
  }

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    // Skip . and ..
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    size_t name_len = strlen(entry->d_name);
    KronosValue *name_val = value_new_string(entry->d_name, name_len);
    if (!name_val) {
      closedir(dir);
      value_release(result);
      value_release(path_arg);
      return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string value");
    }

    // Grow list if needed
    if (result->as.list.count >= result->as.list.capacity) {
      size_t old_cap = result->as.list.capacity;
      size_t new_cap = old_cap * 2;
      KronosValue **new_items =
          realloc(result->as.list.items, sizeof(KronosValue *) * new_cap);
      if (!new_items) {
        value_release(name_val);
        closedir(dir);
        value_release(result);
        value_release(path_arg);
        return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to grow list");
      }
      result->as.list.items = new_items;
      result->as.list.capacity = new_cap;
      // Initialize new slots to NULL (realloc doesn't zero new memory)
      memset(&new_items[old_cap], 0,
             (new_cap - old_cap) * sizeof(KronosValue *));
    }

    value_retain(name_val);
    result->as.list.items[result->as.list.count++] = name_val;
    value_release(name_val);
  }

  closedir(dir);
  value_release(path_arg);

  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result););
  value_release(result);
  return 0;
}

static int builtin_join_path(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 2) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'join_path' expects 2 arguments, got %d",
                     arg_count);
  }
  KronosValue *path2_arg;

  POP_OR_RETURN(vm, path2_arg);
  KronosValue *path1_arg;

  POP_OR_RETURN_WITH_CLEANUP(vm, path1_arg, value_release(path2_arg));
  if (path1_arg->type != VAL_STRING || path2_arg->type != VAL_STRING) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'join_path' requires two string arguments");
    value_release(path1_arg);
    value_release(path2_arg);
    return err;
  }

  const char *path1 = path1_arg->as.string.data;
  const char *path2 = path2_arg->as.string.data;
  size_t path1_len = path1_arg->as.string.length;
  size_t path2_len = path2_arg->as.string.length;

  // Calculate result length
  size_t result_len = path1_len + path2_len + 1; // +1 for separator
  char *joined = malloc(result_len + 1);
  if (!joined) {
    value_release(path1_arg);
    value_release(path2_arg);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate memory");
  }

  // Copy first path
  memcpy(joined, path1, path1_len);
  size_t offset = path1_len;

  // Add separator if needed
  if (path1_len > 0 && path1[path1_len - 1] != '/' && path2_len > 0 &&
      path2[0] != '/') {
    joined[offset++] = '/';
  }

  // Copy second path
  memcpy(joined + offset, path2, path2_len);
  offset += path2_len;
  joined[offset] = '\0';

  KronosValue *result = value_new_string(joined, offset);
  free(joined);
  value_release(path1_arg);
  value_release(path2_arg);

  if (!result) {
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string value");
  }
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result););
  value_release(result);
  return 0;
}

static int builtin_dirname(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'dirname' expects 1 argument, got %d",
                     arg_count);
  }
  KronosValue *path_arg;

  POP_OR_RETURN(vm, path_arg);
  if (path_arg->type != VAL_STRING) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'dirname' requires a string argument");
    value_release(path_arg);
    return err;
  }

  const char *path = path_arg->as.string.data;
  size_t path_len = path_arg->as.string.length;

  // Find last separator
  size_t last_sep = path_len;
  for (size_t i = path_len; i > 0; i--) {
    if (path[i - 1] == '/') {
      last_sep = i - 1;
      break;
    }
  }

  // If no separator found, return "."
  if (last_sep == path_len) {
    KronosValue *result = value_new_string(".", 1);
    value_release(path_arg);
    if (!result) {
      return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string value");
    }
    PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result););
    value_release(result);
    return 0;
  }

  // If separator is at start, return "/"
  if (last_sep == 0) {
    KronosValue *result = value_new_string("/", 1);
    value_release(path_arg);
    if (!result) {
      return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string value");
    }
    PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result););
    value_release(result);
    return 0;
  }

  // Return path up to (but not including) last separator
  KronosValue *result = value_new_string(path, last_sep);
  value_release(path_arg);
  if (!result) {
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string value");
  }
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result););
  value_release(result);
  return 0;
}

static int builtin_basename(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'basename' expects 1 argument, got %d",
                     arg_count);
  }
  KronosValue *path_arg;

  POP_OR_RETURN(vm, path_arg);
  if (path_arg->type != VAL_STRING) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'basename' requires a string argument");
    value_release(path_arg);
    return err;
  }

  const char *path = path_arg->as.string.data;
  size_t path_len = path_arg->as.string.length;

  // Find last separator
  size_t last_sep = path_len;
  for (size_t i = path_len; i > 0; i--) {
    if (path[i - 1] == '/') {
      last_sep = i - 1;
      break;
    }
  }

  // If no separator found, return entire path
  if (last_sep == path_len) {
    value_retain(path_arg);
    PUSH_OR_RETURN_WITH_CLEANUP(vm, path_arg, value_release(path_arg););
    value_release(path_arg);
    return 0;
  }

  // Return path after last separator
  size_t name_start = last_sep + 1;
  size_t name_len = path_len - name_start;
  KronosValue *result = value_new_string(path + name_start, name_len);
  value_release(path_arg);
  if (!result) {
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string value");
  }
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result););
  value_release(result);
  return 0;
}

static int builtin_regex_match(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 2) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'regex.match' expects 2 arguments, got %d",
                     arg_count);
  }
  KronosValue *pattern_arg;

  POP_OR_RETURN(vm, pattern_arg);
  KronosValue *string_arg;

  POP_OR_RETURN_WITH_CLEANUP(vm, string_arg, value_release(pattern_arg));
  if (pattern_arg->type != VAL_STRING || string_arg->type != VAL_STRING) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'regex.match' requires string arguments");
    value_release(pattern_arg);
    value_release(string_arg);
    return err;
  }

  regex_t regex;
  int ret = regcomp(&regex, pattern_arg->as.string.data, REG_EXTENDED);
  if (ret != 0) {
    // regcomp() failed - regex structure is in undefined state
    // regerror() is safe to call with the error code even after failed
    // regcomp() Do NOT call regfree() on a failed regcomp() - it's unsafe
    char errbuf[REGEX_ERROR_BUFFER_SIZE];
    regerror(ret, &regex, errbuf, sizeof(errbuf));
    int err =
        vm_errorf(vm, KRONOS_ERR_RUNTIME, "Invalid regex pattern: %s", errbuf);
    value_release(pattern_arg);
    value_release(string_arg);
    return err;
  }

  int match = regexec(&regex, string_arg->as.string.data, 0, NULL, 0) == 0;
  regfree(&regex);

  KronosValue *result = value_new_bool(match);
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(pattern_arg);
                              value_release(string_arg););
  value_release(result);
  value_release(pattern_arg);
  value_release(string_arg);
  return 0;
}

static int builtin_regex_search(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 2) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'regex.search' expects 2 arguments, got %d",
                     arg_count);
  }
  KronosValue *pattern_arg;

  POP_OR_RETURN(vm, pattern_arg);
  KronosValue *string_arg;

  POP_OR_RETURN_WITH_CLEANUP(vm, string_arg, value_release(pattern_arg));
  if (pattern_arg->type != VAL_STRING || string_arg->type != VAL_STRING) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'regex.search' requires string arguments");
    value_release(pattern_arg);
    value_release(string_arg);
    return err;
  }

  regex_t regex;
  int ret = regcomp(&regex, pattern_arg->as.string.data, REG_EXTENDED);
  if (ret != 0) {
    // regcomp() failed - regex structure is in undefined state
    // regerror() is safe to call with the error code even after failed
    // regcomp() Do NOT call regfree() on a failed regcomp() - it's unsafe
    char errbuf[REGEX_ERROR_BUFFER_SIZE];
    regerror(ret, &regex, errbuf, sizeof(errbuf));
    int err =
        vm_errorf(vm, KRONOS_ERR_RUNTIME, "Invalid regex pattern: %s", errbuf);
    value_release(pattern_arg);
    value_release(string_arg);
    return err;
  }

  regmatch_t match;
  int found = regexec(&regex, string_arg->as.string.data, 1, &match, 0) == 0;

  KronosValue *result;
  if (found && match.rm_so >= 0) {
    // Extract matched substring
    size_t match_len = (size_t)(match.rm_eo - match.rm_so);
    result =
        value_new_string(string_arg->as.string.data + match.rm_so, match_len);
  } else {
    // No match - return nil
    result = value_new_nil();
  }
  regfree(&regex);

  if (!result) {
    value_release(pattern_arg);
    value_release(string_arg);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create result value");
  }
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(pattern_arg);
                              value_release(string_arg););
  value_release(result);
  value_release(pattern_arg);
  value_release(string_arg);
  return 0;
}

static int builtin_regex_findall(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 2) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'regex.findall' expects 2 arguments, got %d",
                     arg_count);
  }
  KronosValue *pattern_arg;

  POP_OR_RETURN(vm, pattern_arg);
  KronosValue *string_arg;

  POP_OR_RETURN_WITH_CLEANUP(vm, string_arg, value_release(pattern_arg));
  if (pattern_arg->type != VAL_STRING || string_arg->type != VAL_STRING) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'regex.findall' requires string arguments");
    value_release(pattern_arg);
    value_release(string_arg);
    return err;
  }

  regex_t regex;
  int ret = regcomp(&regex, pattern_arg->as.string.data, REG_EXTENDED);
  if (ret != 0) {
    // regcomp() failed - regex structure is in undefined state
    // regerror() is safe to call with the error code even after failed
    // regcomp() Do NOT call regfree() on a failed regcomp() - it's unsafe
    char errbuf[REGEX_ERROR_BUFFER_SIZE];
    regerror(ret, &regex, errbuf, sizeof(errbuf));
    int err =
        vm_errorf(vm, KRONOS_ERR_RUNTIME, "Invalid regex pattern: %s", errbuf);
    value_release(pattern_arg);
    value_release(string_arg);
    return err;
  }

  KronosValue *result = value_new_list(16);
  if (!result) {
    regfree(&regex);
    value_release(pattern_arg);
    value_release(string_arg);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create list");
  }

  const char *search_str = string_arg->as.string.data;
  size_t search_len = string_arg->as.string.length;
  size_t offset = 0;
  regmatch_t match;

  while (offset < search_len) {
    int found = regexec(&regex, search_str + offset, 1, &match, 0) == 0;
    if (!found || match.rm_so < 0) {
      break;
    }

    // Adjust match positions to absolute offsets
    size_t match_start = offset + (size_t)match.rm_so;
    size_t match_end = offset + (size_t)match.rm_eo;
    size_t match_len = match_end - match_start;

    // Extract matched substring
    KronosValue *match_val =
        value_new_string(search_str + match_start, match_len);
    if (!match_val) {
      regfree(&regex);
      value_release(result);
      value_release(pattern_arg);
      value_release(string_arg);
      return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string value");
    }

    // Grow list if needed
    if (result->as.list.count >= result->as.list.capacity) {
      size_t new_cap = result->as.list.capacity * 2;
      KronosValue **new_items =
          realloc(result->as.list.items, sizeof(KronosValue *) * new_cap);
      if (!new_items) {
        value_release(match_val);
        regfree(&regex);
        value_release(result);
        value_release(pattern_arg);
        value_release(string_arg);
        return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to grow list");
      }
      result->as.list.items = new_items;
      result->as.list.capacity = new_cap;
    }

    value_retain(match_val);
    result->as.list.items[result->as.list.count++] = match_val;
    value_release(match_val);

    // Move offset past this match
    if (match.rm_eo > match.rm_so) {
      offset = match_end;
    } else {
      // Zero-length match - advance by one character to avoid infinite loop
      offset++;
    }
  }

  regfree(&regex);
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(pattern_arg);
                              value_release(string_arg););
  value_release(result);
  value_release(pattern_arg);
  value_release(string_arg);
  return 0;
}

// Built-in function dispatch table entry
typedef struct {
  const char *name;
  BuiltinHandler handler;
} BuiltinEntry;

// Comparison function for binary search in builtin dispatch table
static int builtin_compare(const void *a, const void *b) {
  const BuiltinEntry *entry_a = (const BuiltinEntry *)a;
  const BuiltinEntry *entry_b = (const BuiltinEntry *)b;
  return strcmp(entry_a->name, entry_b->name);
}

// Built-in function dispatch table (sorted alphabetically for binary search)
static const BuiltinEntry builtin_table[] = {
    {"abs", builtin_abs},
    {"add", builtin_add},
    {"basename", builtin_basename},
    {"ceil", builtin_ceil},
    {"contains", builtin_contains},
    {"dirname", builtin_dirname},
    {"divide", builtin_divide},
    {"ends_with", builtin_ends_with},
    {"file_exists", builtin_file_exists},
    {"findall", builtin_regex_findall},
    {"floor", builtin_floor},
    {"join", builtin_join},
    {"join_path", builtin_join_path},
    {"len", builtin_len},
    {"list_files", builtin_list_files},
    {"lowercase", builtin_lowercase},
    {"match", builtin_regex_match},
    {"max", builtin_max},
    {"min", builtin_min},
    {"multiply", builtin_multiply},
    {"power", builtin_power},
    {"rand", builtin_rand},
    {"read_file", builtin_read_file},
    {"read_lines", builtin_read_lines},
    {"replace", builtin_replace},
    {"reverse", builtin_reverse},
    {"round", builtin_round},
    {"search", builtin_regex_search},
    {"sort", builtin_sort},
    {"split", builtin_split},
    {"sqrt", builtin_sqrt},
    {"starts_with", builtin_starts_with},
    {"subtract", builtin_subtract},
    {"to_bool", builtin_to_bool},
    {"to_number", builtin_to_number},
    {"to_string", builtin_to_string},
    {"trim", builtin_trim},
    {"uppercase", builtin_uppercase},
    {"write_file", builtin_write_file},
};
static const size_t builtin_table_size =
    sizeof(builtin_table) / sizeof(builtin_table[0]);

// Look up built-in function by name using binary search
static BuiltinHandler find_builtin(const char *name) {
  BuiltinEntry key = {name, NULL};
  BuiltinEntry *result =
      (BuiltinEntry *)bsearch(&key, builtin_table, builtin_table_size,
                              sizeof(BuiltinEntry), builtin_compare);
  return result ? result->handler : NULL;
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

  // Check for built-in functions first
  const char *func_name = name_val->as.string.data;

  // Check for module.function syntax (e.g., math.sqrt)
  const char *dot = strchr(func_name, '.');
  if (dot) {
    // Split module and function name
    size_t module_len = (size_t)(dot - func_name);
    char *module_name = malloc(module_len + 1);
    if (!module_name) {
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
          return err;
        }

        // Check parameter count
        if (arg_count != (uint8_t)mod_func->param_count) {
          int err =
              vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function '%s.%s' expects %zu argument%s, but got %d",
                        module_name, actual_func_name, mod_func->param_count,
                        mod_func->param_count == 1 ? "" : "s", arg_count);
          free(module_name);
          return err;
        }

        // Pop arguments from current VM
        KronosValue **args = NULL;
        if (arg_count > 0) {
          args = malloc(sizeof(KronosValue *) * arg_count);
          if (!args) {
            free(module_name);
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
              return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
            }
          }
        }

        // Call the module function using helper
        int result = call_module_function(vm, mod, mod_func, args, arg_count);
        free(args);
        free(module_name);

        if (result < 0) {
          return result;
        }

        return 0; // Function call completed
      } else {
        int err = vm_errorf(vm, KRONOS_ERR_NOT_FOUND, "Unknown module '%s'",
                            module_name);
        free(module_name);
        return err;
      }
    }
  }

  // Try to find built-in function using dispatch table
  BuiltinHandler builtin = find_builtin(func_name);
  if (builtin) {
    return builtin(vm, arg_count);
  }

  // Try variable containing a function value (lambda)
  KronosValue *var_val = vm_get_variable(vm, func_name);
  if (var_val && var_val->type == VAL_FUNCTION) {
    // Call the function value
    return call_function_value(vm, var_val, func_name, arg_count);
  }
  // If vm_get_variable set an error (variable not found), clear it because
  // we're going to try looking up a named function instead
  if (!var_val) {
    vm_clear_error(vm);
  }

  // Try user-defined function
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
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to copy function name");
  }

  func->param_count = param_count;
  func->params = param_count > 0 ? malloc(sizeof(char *) * param_count) : NULL;
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
  // [OP_DEFINE_FUNC][name_idx:2][param_count:1][params:2*N][body_start:2][OP_JUMP][skip_offset:2]
  read_byte(vm); // body_start high byte
  if (vm->last_error_message) {
    // Cleanup already done above
    return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
  }
  read_byte(vm); // body_start low byte
  if (vm->last_error_message) {
    // Cleanup already done above
    return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
  }

  // Consume OP_JUMP instruction byte (part of bytecode format)
  read_byte(vm);
  if (vm->last_error_message) {
    // Cleanup already done above
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
    // Cleanup already done above
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
 *   OP_MAKE_FUNCTION [param_count:1] [param_name_idx:2*N] [body_len:2] [body:N]
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

  // Read body length (2 bytes)
  uint8_t high = read_byte(vm);
  if (vm->last_error_message) {
    for (int i = 0; i < param_count; i++) {
      free(param_names[i]);
    }
    free(param_names);
    return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
  }
  uint8_t low = read_byte(vm);
  if (vm->last_error_message) {
    for (int i = 0; i < param_count; i++) {
      free(param_names[i]);
    }
    free(param_names);
    return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
  }
  uint16_t body_len = (uint16_t)((high << 8) | low);

  // The body bytecode starts at current IP
  uint8_t *body_bytecode = vm->ip;

  // Create the function value
  KronosValue *func_val = value_new_function(body_bytecode, body_len,
                                              param_count, param_names);

  // Free the temporary param_names array (value_new_function made copies)
  for (int i = 0; i < param_count; i++) {
    free(param_names[i]);
  }
  free(param_names);

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

    // Dispatch to handler function using dispatch table
    // The dispatch table uses designated initializers, so its size is
    // determined by the highest index (OP_HALT = 44). Check bounds and NULL
    // handlers.
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
