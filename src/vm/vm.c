/**
 * @file vm.c
 * @brief Virtual machine for executing Kronos bytecode
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
#define NUMBER_STRING_BUFFER_SIZE 64  // Buffer size for converting numbers to strings
#define REGEX_ERROR_BUFFER_SIZE 256   // Buffer size for regex error messages

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
  KronosValue *val_a = *(KronosValue **)a;
  KronosValue *val_b = *(KronosValue **)b;

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
}

// Forward declaration for vm_execute (needed by call_module_function)
int vm_execute(KronosVM *vm, Bytecode *bytecode);

/**
 * @brief Call a function in an external module
 *
 * Handles all the complexity of calling a function defined in another module:
 * - Sets up the call frame in the module's VM
 * - Transfers arguments from the caller's VM
 * - Executes the function
 * - Retrieves the return value
 * - Cleans up all state
 *
 * @param caller_vm The VM making the call
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
    for (int i = 0; i < arg_count; i++) {
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
  if (!vm)
    return;

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
  vm->ip = vm->exception_handlers[idx].handler_ip;

  return true; // Exception handled, continue execution from handler
}

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
  if (!vm)
    return NULL;

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
  if (!vm)
    return;

  // Release all values on stack
  while (vm->stack_top > vm->stack) {
    vm->stack_top--;
    value_release(*vm->stack_top);
  }
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

// Get a module by name
Module *vm_get_module(KronosVM *vm, const char *name) {
  if (!vm || !name)
    return NULL;

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
  if (!module_path)
    return NULL;

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

  // Read file
  FILE *file = fopen(resolved_path, "r");
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

  AST *ast = parse(tokens);
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

  // Check import depth before recursive vm_execute() call to prevent C stack exhaustion
  // loading_count represents the current depth of the import chain
  if (root_vm->loading_count > IMPORT_DEPTH_MAX) {
    vm_free(module_vm);
    free(resolved_path);
    // Remove from root VM's loading stack
    root_vm->loading_count--;
    free(root_vm->loading_modules[root_vm->loading_count]);
    root_vm->loading_modules[root_vm->loading_count] = NULL;
    bytecode_free(bytecode);
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Import depth exceeded maximum (%d). Deep import chains can "
                     "exhaust the C stack.",
                     IMPORT_DEPTH_MAX);
  }

  int exec_result = vm_execute(module_vm, bytecode);
  
  if (exec_result < 0) {
    // Execution failed - clean up resources
    // Clear stack first to release any values that might reference bytecode constants
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

// Helper function to convert a value to a string representation
// Returns a newly allocated string that the caller must free
static char *value_to_string_repr(KronosValue *val) {
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

      len = (size_t)snprintf(str_buf, NUMBER_STRING_BUFFER_SIZE, "%.0f", val->as.number);
    } else {
      len = (size_t)snprintf(str_buf, NUMBER_STRING_BUFFER_SIZE, "%g", val->as.number);
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
    return vm_error(vm, KRONOS_ERR_INVALID_ARGUMENT,
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
                    "vm_execute: bytecode has non-zero const_count but NULL constants array");
  }

  vm->bytecode = bytecode;
  vm->ip = bytecode->code;
  // Note: current_frame should be set by the caller for function execution
  // For top-level code, current_frame is NULL

  bool handling_exception = false;

  while (1) {
    // Check for exceptions before executing next instruction
    // Only check if we're not already handling an exception (to avoid infinite
    // loop)
    if (vm->last_error_code != KRONOS_OK && !handling_exception) {
      if (handle_exception_if_any(vm)) {
        // We're now handling the exception - set flag to allow OP_CATCH to run
        handling_exception = true;
        continue;
      } else {
        // No handler - propagate the error and stop execution
        return vm_propagate_error(vm, vm->last_error_code);
      }
    }
    handling_exception = false; // Reset for next iteration

    uint8_t instruction = read_byte(vm);
    
    // Check for error state after read_byte (it may return OP_HALT on error)
    // If read_byte() encountered an error, it sets vm->last_error_message
    if (vm->last_error_message) {
      return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
    }

    switch (instruction) {
    case OP_LOAD_CONST: {
      KronosValue *constant = read_constant(vm);
      if (!constant) {
        return vm_propagate_error(vm, KRONOS_ERR_INTERNAL);
      }
      if (push(vm, constant) != 0) {
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
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
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
      if (push(vm, value) != 0) {
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
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
        // Numeric addition
        KronosValue *result = value_new_number(a->as.number + b->as.number);
        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
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
          return vm_error(vm, KRONOS_ERR_INTERNAL,
                          "Failed to create string value");
        }

        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        value_release(result);
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
        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
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
        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
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
        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
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

    case OP_MOD: {
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
          int err = vm_error(vm, KRONOS_ERR_RUNTIME, "Cannot modulo by zero");
          value_release(a);
          value_release(b);
          return err;
        }
        // Use fmod for floating-point modulo
        KronosValue *result =
            value_new_number(fmod(a->as.number, b->as.number));
        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
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
      break;
    }

    case OP_NEG: {
      KronosValue *val = pop(vm);
      if (!val) {
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }

      if (val->type == VAL_NUMBER) {
        KronosValue *result = value_new_number(-val->as.number);
        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        value_release(result);
      } else {
        int err = vm_error(vm, KRONOS_ERR_RUNTIME,
                           "Cannot negate - value must be a number");
        value_release(val);
        return err;
      }

      value_release(val);
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
      if (push(vm, res) != 0) {
        value_release(res);
        value_release(a);
        value_release(b);
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
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
      if (push(vm, res) != 0) {
        value_release(res);
        value_release(a);
        value_release(b);
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
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
        if (push(vm, res) != 0) {
          value_release(res);
          value_release(a);
          value_release(b);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
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
        if (push(vm, res) != 0) {
          value_release(res);
          value_release(a);
          value_release(b);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
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
        if (push(vm, res) != 0) {
          value_release(res);
          value_release(a);
          value_release(b);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
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
        if (push(vm, res) != 0) {
          value_release(res);
          value_release(a);
          value_release(b);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
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
      if (push(vm, res) != 0) {
        value_release(res);
        value_release(a);
        value_release(b);
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
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
      if (push(vm, res) != 0) {
        value_release(res);
        value_release(a);
        value_release(b);
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
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
      if (push(vm, res) != 0) {
        value_release(res);
        value_release(a);
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
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

      // Built-in: read_file(path)       // Built-in: read_file(path)
      if (strcmp(func_name, "read_file") == 0) {
        if (arg_count != 1)
          return vm_errorf(vm, KRONOS_ERR_RUNTIME, "Expected 1 argument");
        KronosValue *path_val = pop(vm);
        if (!path_val)
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        if (path_val->type != VAL_STRING) {
          value_release(path_val);
          return vm_errorf(vm, KRONOS_ERR_RUNTIME, "Path must be a string");
        }
        FILE *file = fopen(path_val->as.string.data, "rb");
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
        if (fsize > SIZE_MAX - 1) {
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
        if (push(vm, res) != 0) {
          value_release(res);
          value_release(path_val);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        value_release(res);
        value_release(path_val);
        break;
      }

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
            Function *mod_func =
                vm_get_function(mod->module_vm, actual_func_name);

            if (!mod_func) {
              int err = vm_errorf(vm, KRONOS_ERR_NOT_FOUND,
                                  "Function '%s' not found in module '%s'",
                                  actual_func_name, module_name);
              free(module_name);
              return err;
            }

            // Check parameter count
            if (arg_count != (uint8_t)mod_func->param_count) {
              int err = vm_errorf(
                  vm, KRONOS_ERR_RUNTIME,
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
            int result =
                call_module_function(vm, mod, mod_func, args, arg_count);
            free(args);
            free(module_name);

            if (result < 0) {
              return result;
            }

            break; // Function call completed
          } else {
            int err = vm_errorf(vm, KRONOS_ERR_NOT_FOUND, "Unknown module '%s'",
                                module_name);
            free(module_name);
            return err;
          }
        }
      }

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
          if (push(vm, result) != 0) {
            value_release(result);
            value_release(a);
            value_release(b);
            return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
          }
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
          if (push(vm, result) != 0) {
            value_release(result);
            value_release(a);
            value_release(b);
            return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
          }
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
          if (push(vm, result) != 0) {
            value_release(result);
            value_release(a);
            value_release(b);
            return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
          }
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
            if (push(vm, result) != 0) {
              value_release(result);
              value_release(a);
              return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
            }

            if (push(vm, result) != 0) {
              value_release(result);
              return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
            }
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

      // Built-in: len(list/string/range)
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
          if (push(vm, result) != 0) {
            value_release(result);
            return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
          }

          push(vm, result);
          value_release(result);
        } else if (arg->type == VAL_STRING) {
          KronosValue *result = value_new_number((double)arg->as.string.length);
          if (push(vm, result) != 0) {
            value_release(result);
            value_release(arg);
            return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
          }

          push(vm, result);
          value_release(result);
        } else if (arg->type == VAL_RANGE) {
          // Calculate range length: number of values in range
          double start = arg->as.range.start;
          double end = arg->as.range.end;
          double step = arg->as.range.step;

          if (step == 0.0) {
            value_release(arg);
            return vm_error(vm, KRONOS_ERR_RUNTIME,
                            "Range step cannot be zero");
          }

          // Calculate number of steps
          double diff = end - start;
          double count = floor((diff / step)) + 1.0;

          // Ensure count is non-negative
          if (count < 0) {
            count = 0;
          }

          KronosValue *result = value_new_number(count);
          if (push(vm, result) != 0) {
            value_release(result);
            value_release(arg);
            return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
          }

          push(vm, result);
          value_release(result);
        } else {
          int err = vm_errorf(
              vm, KRONOS_ERR_RUNTIME,
              "Function 'len' requires a list, string, or range argument");
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
          int err =
              vm_errorf(vm, KRONOS_ERR_RUNTIME,
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
          return vm_error(vm, KRONOS_ERR_INTERNAL,
                          "Failed to create string value");
        }
        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
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
          int err =
              vm_errorf(vm, KRONOS_ERR_RUNTIME,
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
          return vm_error(vm, KRONOS_ERR_INTERNAL,
                          "Failed to create string value");
        }
        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
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
        while (start < arg->as.string.length &&
               isspace((unsigned char)arg->as.string.data[start])) {
          start++;
        }

        // Find end (skip trailing whitespace)
        size_t end = arg->as.string.length;
        while (end > start &&
               isspace((unsigned char)arg->as.string.data[end - 1])) {
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
          return vm_error(vm, KRONOS_ERR_INTERNAL,
                          "Failed to create string value");
        }
        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
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
              return vm_error(vm, KRONOS_ERR_INTERNAL,
                              "Failed to create string value");
            }

            // Grow list if needed
            if (result->as.list.count >= result->as.list.capacity) {
              size_t new_cap = result->as.list.capacity * 2;
              KronosValue **new_items = realloc(
                  result->as.list.items, sizeof(KronosValue *) * new_cap);
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
              return vm_error(vm, KRONOS_ERR_INTERNAL,
                              "Failed to allocate memory");
            }
            memcpy(part, str_data + start, part_len);
            part[part_len] = '\0';

            KronosValue *part_val = value_new_string(part, part_len);
            free(part);
            if (!part_val) {
              value_release(result);
              value_release(str);
              value_release(delim);
              return vm_error(vm, KRONOS_ERR_INTERNAL,
                              "Failed to create string value");
            }

            // Grow list if needed
            if (result->as.list.count >= result->as.list.capacity) {
              size_t new_cap = result->as.list.capacity * 2;
              KronosValue **new_items = realloc(
                  result->as.list.items, sizeof(KronosValue *) * new_cap);
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

        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
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
          int err = vm_errorf(
              vm, KRONOS_ERR_RUNTIME,
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
            memcpy(joined + offset, delim->as.string.data,
                   delim->as.string.length);
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
          return vm_error(vm, KRONOS_ERR_INTERNAL,
                          "Failed to create string value");
        }
        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
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
          if (push(vm, arg) != 0) {
            return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
          }

          push(vm, arg);
          value_release(
              arg); // Release the pop reference (push already retained)
          break;
        } else if (arg->type == VAL_NUMBER) {
          // Convert number to string
          // Use a reasonable buffer size
          str_buf = malloc(NUMBER_STRING_BUFFER_SIZE);
          if (!str_buf) {
            value_release(arg);
            return vm_error(vm, KRONOS_ERR_INTERNAL,
                            "Failed to allocate memory");
          }

          // Check if it's a whole number
          double intpart;
          double frac = modf(arg->as.number, &intpart);

          if (frac == 0.0 && fabs(arg->as.number) < 1.0e15) {
            str_len = (size_t)snprintf(str_buf, NUMBER_STRING_BUFFER_SIZE, "%.0f", arg->as.number);
          } else {
            str_len = (size_t)snprintf(str_buf, NUMBER_STRING_BUFFER_SIZE, "%g", arg->as.number);
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
          if (!str_buf) {
            value_release(arg);
            return vm_error(vm, KRONOS_ERR_INTERNAL,
                            "Failed to allocate memory");
          }
        } else if (arg->type == VAL_NIL) {
          str_buf = strdup("null");
          str_len = 4;
          if (!str_buf) {
            value_release(arg);
            return vm_error(vm, KRONOS_ERR_INTERNAL,
                            "Failed to allocate memory");
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
          return vm_error(vm, KRONOS_ERR_INTERNAL,
                          "Failed to create string value");
        }
        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
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
          int err =
              vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'contains' requires two string arguments");
          value_release(str);
          value_release(substring);
          return err;
        }

        // Use strstr to check if substring exists
        bool found =
            (strstr(str->as.string.data, substring->as.string.data) != NULL);
        KronosValue *result = value_new_bool(found);
        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
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
          int err =
              vm_errorf(vm, KRONOS_ERR_RUNTIME,
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
        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
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
          int err =
              vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'ends_with' requires two string arguments");
          value_release(str);
          value_release(suffix);
          return err;
        }

        bool ends = false;
        if (suffix->as.string.length <= str->as.string.length) {
          size_t start_pos = str->as.string.length - suffix->as.string.length;
          ends =
              (memcmp(str->as.string.data + start_pos, suffix->as.string.data,
                      suffix->as.string.length) == 0);
        }
        KronosValue *result = value_new_bool(ends);
        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
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
        if (str->type != VAL_STRING || old_str->type != VAL_STRING ||
            new_str->type != VAL_STRING) {
          int err =
              vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'replace' requires three string arguments");
          value_release(str);
          value_release(old_str);
          value_release(new_str);
          return err;
        }

        // Handle empty old string (return original string)
        if (old_str->as.string.length == 0) {
          value_retain(str);
          if (push(vm, str) != 0) {
            value_release(str);
            value_release(old_str);
            value_release(new_str);
            return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
          }
          value_release(str);
          value_release(old_str);
          value_release(new_str);
          break;
        }

        // Calculate maximum possible result size
        // old_len == 0 is already handled above, so we know old_len > 0 here
        size_t str_len = str->as.string.length;
        size_t old_len = old_str->as.string.length;
        size_t new_len = new_str->as.string.length;
        size_t max_result_len = str_len;

        if (new_len > old_len) {
          // Worst case: maximum non-overlapping occurrences
          // Maximum occurrences is at most str_len / old_len
          // Each occurrence adds (new_len - old_len) characters
          // Safe upper bound: str_len + (str_len / old_len + 1) * (new_len -
          // old_len) But we need to check for overflow
          size_t max_occurrences = str_len / old_len;
          size_t growth_per_occurrence = new_len - old_len;

          // Check for overflow: max_occurrences * growth_per_occurrence
          if (max_occurrences > 0 &&
              growth_per_occurrence > SIZE_MAX / max_occurrences) {
            // Overflow would occur, use a conservative upper bound
            max_result_len = SIZE_MAX;
          } else {
            size_t total_growth = max_occurrences * growth_per_occurrence;
            // Check if str_len + total_growth would overflow
            if (total_growth > SIZE_MAX - str_len) {
              max_result_len = SIZE_MAX;
            } else {
              max_result_len = str_len + total_growth;
            }
          }
        }

        // Check for overflow before malloc (max_result_len + 1)
        // Check if max_result_len + 1 would overflow by checking if max_result_len > SIZE_MAX - 1
        // This avoids the undefined behavior of SIZE_MAX + 1
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
          return vm_error(vm, KRONOS_ERR_INTERNAL,
                          "Failed to create string value");
        }
        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        value_release(result);
        value_release(str);
        value_release(old_str);
        value_release(new_str);
        break;
      }

      // Built-in: sqrt(number)
      if (strcmp(func_name, "sqrt") == 0) {
        if (arg_count != 1) {
          return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                           "Function 'sqrt' expects 1 argument, got %d",
                           arg_count);
        }
        KronosValue *arg = pop(vm);
        if (!arg) {
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
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
        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        value_release(result);
        value_release(arg);
        break;
      }

      // Built-in: power(base, exponent)
      if (strcmp(func_name, "power") == 0) {
        if (arg_count != 2) {
          return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                           "Function 'power' expects 2 arguments, got %d",
                           arg_count);
        }
        KronosValue *exponent = pop(vm);
        if (!exponent) {
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        KronosValue *base = pop(vm);
        if (!base) {
          value_release(exponent);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        if (base->type != VAL_NUMBER || exponent->type != VAL_NUMBER) {
          int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                              "Function 'power' requires two number arguments");
          value_release(base);
          value_release(exponent);
          return err;
        }
        KronosValue *result =
            value_new_number(pow(base->as.number, exponent->as.number));
        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        value_release(result);
        value_release(base);
        value_release(exponent);
        break;
      }

      // Built-in: abs(number)
      if (strcmp(func_name, "abs") == 0) {
        if (arg_count != 1) {
          return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                           "Function 'abs' expects 1 argument, got %d",
                           arg_count);
        }
        KronosValue *arg = pop(vm);
        if (!arg) {
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        if (arg->type != VAL_NUMBER) {
          int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                              "Function 'abs' requires a number argument");
          value_release(arg);
          return err;
        }
        KronosValue *result = value_new_number(fabs(arg->as.number));
        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        value_release(result);
        value_release(arg);
        break;
      }

      // Built-in: round(number)
      if (strcmp(func_name, "round") == 0) {
        if (arg_count != 1) {
          return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                           "Function 'round' expects 1 argument, got %d",
                           arg_count);
        }
        KronosValue *arg = pop(vm);
        if (!arg) {
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        if (arg->type != VAL_NUMBER) {
          int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                              "Function 'round' requires a number argument");
          value_release(arg);
          return err;
        }
        KronosValue *result = value_new_number(round(arg->as.number));
        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        value_release(result);
        value_release(arg);
        break;
      }

      // Built-in: floor(number)
      if (strcmp(func_name, "floor") == 0) {
        if (arg_count != 1) {
          return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                           "Function 'floor' expects 1 argument, got %d",
                           arg_count);
        }
        KronosValue *arg = pop(vm);
        if (!arg) {
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        if (arg->type != VAL_NUMBER) {
          int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                              "Function 'floor' requires a number argument");
          value_release(arg);
          return err;
        }
        KronosValue *result = value_new_number(floor(arg->as.number));
        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        value_release(result);
        value_release(arg);
        break;
      }

      // Built-in: ceil(number)
      if (strcmp(func_name, "ceil") == 0) {
        if (arg_count != 1) {
          return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                           "Function 'ceil' expects 1 argument, got %d",
                           arg_count);
        }
        KronosValue *arg = pop(vm);
        if (!arg) {
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        if (arg->type != VAL_NUMBER) {
          int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                              "Function 'ceil' requires a number argument");
          value_release(arg);
          return err;
        }
        KronosValue *result = value_new_number(ceil(arg->as.number));
        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        value_release(result);
        value_release(arg);
        break;
      }

      // Built-in: rand()
      if (strcmp(func_name, "rand") == 0) {
        if (arg_count != 0) {
          return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                           "Function 'rand' expects 0 arguments, got %d",
                           arg_count);
        }
        // Generate random number between 0.0 and 1.0
        double random_val = (double)rand() / (double)RAND_MAX;
        KronosValue *result = value_new_number(random_val);
        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        value_release(result);
        break;
      }

      // Built-in: min(...)
      if (strcmp(func_name, "min") == 0) {
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
            // Clean up already popped args
            for (int j = i + 1; j < arg_count; j++) {
              value_release(args[j]);
            }
            free(args);
            return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
          }
          if (args[i]->type != VAL_NUMBER) {
            int err = vm_errorf(
                vm, KRONOS_ERR_RUNTIME,
                "Function 'min' requires all arguments to be numbers");
            for (int j = 0; j < arg_count; j++) {
              value_release(args[j]);
            }
            free(args);
            return err;
          }
        }
        // Find minimum
        double min_val = args[0]->as.number;
        for (int i = 1; i < arg_count; i++) {
          if (args[i]->as.number < min_val) {
            min_val = args[i]->as.number;
          }
        }
        // Release all args
        for (int i = 0; i < arg_count; i++) {
          value_release(args[i]);
        }
        free(args);
        KronosValue *result = value_new_number(min_val);
        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        value_release(result);
        break;
      }

      // Built-in: max(...)
      if (strcmp(func_name, "max") == 0) {
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
            // Clean up already popped args
            for (int j = i + 1; j < arg_count; j++) {
              value_release(args[j]);
            }
            free(args);
            return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
          }
          if (args[i]->type != VAL_NUMBER) {
            int err = vm_errorf(
                vm, KRONOS_ERR_RUNTIME,
                "Function 'max' requires all arguments to be numbers");
            for (int j = 0; j < arg_count; j++) {
              value_release(args[j]);
            }
            free(args);
            return err;
          }
        }
        // Find maximum
        double max_val = args[0]->as.number;
        for (int i = 1; i < arg_count; i++) {
          if (args[i]->as.number > max_val) {
            max_val = args[i]->as.number;
          }
        }
        // Release all args
        for (int i = 0; i < arg_count; i++) {
          value_release(args[i]);
        }
        free(args);
        KronosValue *result = value_new_number(max_val);
        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        value_release(result);
        break;
      }

      // Built-in: to_number(string)
      if (strcmp(func_name, "to_number") == 0) {
        if (arg_count != 1) {
          return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                           "Function 'to_number' expects 1 argument, got %d",
                           arg_count);
        }
        KronosValue *arg = pop(vm);
        if (!arg) {
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        if (arg->type == VAL_NUMBER) {
          // Already a number, just return it
          if (push(vm, arg) != 0) {
            return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
          }

          push(vm, arg);
          value_release(
              arg); // Release the pop reference (push already retained)
          break;
        } else if (arg->type == VAL_STRING) {
          // Convert string to number
          char *endptr;
          double num = strtod(arg->as.string.data, &endptr);
          // Check if conversion was successful (endptr should point to end of
          // string)
          if (*endptr != '\0' && *endptr != '\n' && *endptr != '\r') {
            int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                                "Cannot convert string to number: '%s'",
                                arg->as.string.data);
            value_release(arg);
            return err;
          }
          KronosValue *result = value_new_number(num);
          if (push(vm, result) != 0) {
            value_release(result);
            value_release(arg);
            return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
          }

          push(vm, result);
          value_release(result);
          value_release(arg);
          break;
        } else {
          int err = vm_errorf(
              vm, KRONOS_ERR_RUNTIME,
              "Function 'to_number' requires a string or number argument");
          value_release(arg);
          return err;
        }
      }

      // Built-in: to_bool(value)
      if (strcmp(func_name, "to_bool") == 0) {
        if (arg_count != 1) {
          return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                           "Function 'to_bool' expects 1 argument, got %d",
                           arg_count);
        }
        KronosValue *arg = pop(vm);
        if (!arg) {
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        bool bool_val = false;
        if (arg->type == VAL_BOOL) {
          // Already a boolean, just return it
          bool_val = arg->as.boolean;
        } else if (arg->type == VAL_STRING) {
          // Convert string to boolean
          // "true" (case-insensitive) -> true, everything else -> false
          if (arg->as.string.length == 4 &&
              (arg->as.string.data[0] == 't' ||
               arg->as.string.data[0] == 'T') &&
              (arg->as.string.data[1] == 'r' ||
               arg->as.string.data[1] == 'R') &&
              (arg->as.string.data[2] == 'u' ||
               arg->as.string.data[2] == 'U') &&
              (arg->as.string.data[3] == 'e' ||
               arg->as.string.data[3] == 'E')) {
            bool_val = true;
          } else {
            bool_val = false;
          }
        } else if (arg->type == VAL_NUMBER) {
          // Number: 0 -> false, everything else -> true
          bool_val = (arg->as.number != 0.0);
        } else if (arg->type == VAL_NIL) {
          // null -> false
          bool_val = false;
        } else {
          int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                              "Cannot convert type to boolean");
          value_release(arg);
          return err;
        }
        value_release(arg);
        KronosValue *result = value_new_bool(bool_val);
        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        value_release(result);
        break;
      }

      // Built-in: reverse(list)
      if (strcmp(func_name, "reverse") == 0) {
        if (arg_count != 1) {
          return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                           "Function 'reverse' expects 1 argument, got %d",
                           arg_count);
        }
        KronosValue *arg = pop(vm);
        if (!arg) {
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
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
          result->as.list.items[result->as.list.count++] =
              arg->as.list.items[i];
        }
        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        value_release(result);
        value_release(arg);
        break;
      }

      // Built-in: sort(list)
      if (strcmp(func_name, "sort") == 0) {
        if (arg_count != 1) {
          return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                           "Function 'sort' expects 1 argument, got %d",
                           arg_count);
        }
        KronosValue *arg = pop(vm);
        if (!arg) {
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
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
          result->as.list.items[result->as.list.count++] =
              arg->as.list.items[i];
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
          qsort(result->as.list.items, result->as.list.count,
                sizeof(KronosValue *), sort_compare_values);
        }
        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        value_release(result);
        value_release(arg);
        break;
      }

      // Built-in: read_file(path)
      if (strcmp(func_name, "read_file") == 0) {
        if (arg_count != 1) {
          return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                           "Function 'read_file' expects 1 argument, got %d",
                           arg_count);
        }
        KronosValue *path_arg = pop(vm);
        if (!path_arg) {
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        if (path_arg->type != VAL_STRING) {
          int err =
              vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'read_file' requires a string argument");
          value_release(path_arg);
          return err;
        }

        FILE *file = fopen(path_arg->as.string.data, "r");
        if (!file) {
          int err =
              vm_errorf(vm, KRONOS_ERR_RUNTIME, "Failed to open file '%s'",
                        path_arg->as.string.data);
          value_release(path_arg);
          return err;
        }

        // Get file size
        if (fseek(file, 0, SEEK_END) != 0) {
          fclose(file);
          value_release(path_arg);
          return vm_errorf(vm, KRONOS_ERR_RUNTIME, "Failed to seek to end of file");
        }
        long file_size = ftell(file);
        if (fseek(file, 0, SEEK_SET) != 0) {
          fclose(file);
          value_release(path_arg);
          return vm_errorf(vm, KRONOS_ERR_RUNTIME, "Failed to seek to start of file");
        }

        if (file_size < 0) {
          fclose(file);
          int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                              "Failed to determine file size for '%s'",
                              path_arg->as.string.data);
          value_release(path_arg);
          return err;
        }

        // Read file content
        char *content = malloc((size_t)file_size + 1);
        if (!content) {
          fclose(file);
          value_release(path_arg);
          return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate memory");
        }

        size_t bytes_read = fread(content, 1, (size_t)file_size, file);
        content[bytes_read] = '\0';
        fclose(file);

        KronosValue *result = value_new_string(content, bytes_read);
        free(content);
        value_release(path_arg);

        if (!result) {
          return vm_error(vm, KRONOS_ERR_INTERNAL,
                          "Failed to create string value");
        }
        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        value_release(result);
        break;
      }

      // Built-in: write_file(path, content)
      if (strcmp(func_name, "write_file") == 0) {
        if (arg_count != 2) {
          return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                           "Function 'write_file' expects 2 arguments, got %d",
                           arg_count);
        }
        KronosValue *content_arg = pop(vm);
        if (!content_arg) {
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        KronosValue *path_arg = pop(vm);
        if (!path_arg) {
          value_release(content_arg);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        if (path_arg->type != VAL_STRING || content_arg->type != VAL_STRING) {
          int err =
              vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'write_file' requires two string arguments");
          value_release(path_arg);
          value_release(content_arg);
          return err;
        }

        FILE *file = fopen(path_arg->as.string.data, "w");
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
        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        value_release(result);
        value_release(path_arg);
        value_release(content_arg);
        break;
      }

      // Built-in: read_lines(path)
      if (strcmp(func_name, "read_lines") == 0) {
        if (arg_count != 1) {
          return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                           "Function 'read_lines' expects 1 argument, got %d",
                           arg_count);
        }
        KronosValue *path_arg = pop(vm);
        if (!path_arg) {
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        if (path_arg->type != VAL_STRING) {
          int err =
              vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'read_lines' requires a string argument");
          value_release(path_arg);
          return err;
        }

        FILE *file = fopen(path_arg->as.string.data, "r");
        if (!file) {
          int err =
              vm_errorf(vm, KRONOS_ERR_RUNTIME, "Failed to open file '%s'",
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

        while ((read = getline(&line, &line_len, file)) != -1) {
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
            return vm_error(vm, KRONOS_ERR_INTERNAL,
                            "Failed to create string value");
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

        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        value_release(result);
        break;
      }

      // Built-in: file_exists(path)
      if (strcmp(func_name, "file_exists") == 0) {
        if (arg_count != 1) {
          return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                           "Function 'file_exists' expects 1 argument, got %d",
                           arg_count);
        }
        KronosValue *path_arg = pop(vm);
        if (!path_arg) {
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        if (path_arg->type != VAL_STRING) {
          int err =
              vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'file_exists' requires a string argument");
          value_release(path_arg);
          return err;
        }

        struct stat st;
        int exists = (stat(path_arg->as.string.data, &st) == 0);
        value_release(path_arg);

        KronosValue *result = value_new_bool(exists);
        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        value_release(result);
        break;
      }

      // Built-in: list_files(path)
      if (strcmp(func_name, "list_files") == 0) {
        if (arg_count != 1) {
          return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                           "Function 'list_files' expects 1 argument, got %d",
                           arg_count);
        }
        KronosValue *path_arg = pop(vm);
        if (!path_arg) {
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        if (path_arg->type != VAL_STRING) {
          int err =
              vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'list_files' requires a string argument");
          value_release(path_arg);
          return err;
        }

        DIR *dir = opendir(path_arg->as.string.data);
        if (!dir) {
          int err =
              vm_errorf(vm, KRONOS_ERR_RUNTIME, "Failed to open directory '%s'",
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
          if (strcmp(entry->d_name, ".") == 0 ||
              strcmp(entry->d_name, "..") == 0) {
            continue;
          }

          size_t name_len = strlen(entry->d_name);
          KronosValue *name_val = value_new_string(entry->d_name, name_len);
          if (!name_val) {
            closedir(dir);
            value_release(result);
            value_release(path_arg);
            return vm_error(vm, KRONOS_ERR_INTERNAL,
                            "Failed to create string value");
          }

          // Grow list if needed
          if (result->as.list.count >= result->as.list.capacity) {
            size_t new_cap = result->as.list.capacity * 2;
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
          }

          value_retain(name_val);
          result->as.list.items[result->as.list.count++] = name_val;
          value_release(name_val);
        }

        closedir(dir);
        value_release(path_arg);

        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        value_release(result);
        break;
      }

      // Built-in: join_path(path1, path2)
      if (strcmp(func_name, "join_path") == 0) {
        if (arg_count != 2) {
          return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                           "Function 'join_path' expects 2 arguments, got %d",
                           arg_count);
        }
        KronosValue *path2_arg = pop(vm);
        if (!path2_arg) {
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        KronosValue *path1_arg = pop(vm);
        if (!path1_arg) {
          value_release(path2_arg);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        if (path1_arg->type != VAL_STRING || path2_arg->type != VAL_STRING) {
          int err =
              vm_errorf(vm, KRONOS_ERR_RUNTIME,
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
          return vm_error(vm, KRONOS_ERR_INTERNAL,
                          "Failed to create string value");
        }
        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        value_release(result);
        break;
      }

      // Built-in: dirname(path)
      if (strcmp(func_name, "dirname") == 0) {
        if (arg_count != 1) {
          return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                           "Function 'dirname' expects 1 argument, got %d",
                           arg_count);
        }
        KronosValue *path_arg = pop(vm);
        if (!path_arg) {
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
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
            return vm_error(vm, KRONOS_ERR_INTERNAL,
                            "Failed to create string value");
          }
          if (push(vm, result) != 0) {
            value_release(result);
            return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
          }

          push(vm, result);
          value_release(result);
          break;
        }

        // If separator is at start, return "/"
        if (last_sep == 0) {
          KronosValue *result = value_new_string("/", 1);
          value_release(path_arg);
          if (!result) {
            return vm_error(vm, KRONOS_ERR_INTERNAL,
                            "Failed to create string value");
          }
          if (push(vm, result) != 0) {
            value_release(result);
            return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
          }

          push(vm, result);
          value_release(result);
          break;
        }

        // Return path up to (but not including) last separator
        KronosValue *result = value_new_string(path, last_sep);
        value_release(path_arg);
        if (!result) {
          return vm_error(vm, KRONOS_ERR_INTERNAL,
                          "Failed to create string value");
        }
        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        value_release(result);
        break;
      }

      // Built-in: basename(path)
      if (strcmp(func_name, "basename") == 0) {
        if (arg_count != 1) {
          return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                           "Function 'basename' expects 1 argument, got %d",
                           arg_count);
        }
        KronosValue *path_arg = pop(vm);
        if (!path_arg) {
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
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
          if (push(vm, path_arg) != 0) {
            value_release(path_arg);
            return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
          }

          push(vm, path_arg);
          value_release(path_arg);
          break;
        }

        // Return path after last separator
        size_t name_start = last_sep + 1;
        size_t name_len = path_len - name_start;
        KronosValue *result = value_new_string(path + name_start, name_len);
        value_release(path_arg);
        if (!result) {
          return vm_error(vm, KRONOS_ERR_INTERNAL,
                          "Failed to create string value");
        }
        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        value_release(result);
        break;
      }

      // Built-in regex module functions
      // regex.match(string, pattern) - Check if pattern matches entire string
      if (strcmp(func_name, "regex.match") == 0 ||
          strcmp(func_name, "match") == 0) {
        if (arg_count != 2) {
          return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                           "Function 'regex.match' expects 2 arguments, got %d",
                           arg_count);
        }
        KronosValue *pattern_arg = pop(vm);
        if (!pattern_arg) {
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        KronosValue *string_arg = pop(vm);
        if (!pattern_arg) {
          value_release(string_arg);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        if (pattern_arg->type != VAL_STRING || string_arg->type != VAL_STRING) {
          int err =
              vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'regex.match' requires string arguments");
          value_release(pattern_arg);
          value_release(string_arg);
          return err;
        }

        regex_t regex;
        int ret = regcomp(&regex, pattern_arg->as.string.data, REG_EXTENDED);
        if (ret != 0) {
          // regcomp() failed - regex structure is in undefined state
          // regerror() is safe to call with the error code even after failed regcomp()
          // Do NOT call regfree() on a failed regcomp() - it's unsafe
          char errbuf[REGEX_ERROR_BUFFER_SIZE];
          regerror(ret, &regex, errbuf, sizeof(errbuf));
          int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                              "Invalid regex pattern: %s", errbuf);
          value_release(pattern_arg);
          value_release(string_arg);
          return err;
        }

        int match =
            regexec(&regex, string_arg->as.string.data, 0, NULL, 0) == 0;
        regfree(&regex);

        KronosValue *result = value_new_bool(match);
        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        value_release(result);
        value_release(pattern_arg);
        value_release(string_arg);
        break;
      }

      // regex.search(string, pattern) - Find first match in string
      if (strcmp(func_name, "regex.search") == 0 ||
          strcmp(func_name, "search") == 0) {
        if (arg_count != 2) {
          return vm_errorf(
              vm, KRONOS_ERR_RUNTIME,
              "Function 'regex.search' expects 2 arguments, got %d", arg_count);
        }
        KronosValue *pattern_arg = pop(vm);
        if (!pattern_arg) {
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        KronosValue *string_arg = pop(vm);
        if (!string_arg) {
          value_release(pattern_arg);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        if (pattern_arg->type != VAL_STRING || string_arg->type != VAL_STRING) {
          int err =
              vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'regex.search' requires string arguments");
          value_release(pattern_arg);
          value_release(string_arg);
          return err;
        }

        regex_t regex;
        int ret = regcomp(&regex, pattern_arg->as.string.data, REG_EXTENDED);
        if (ret != 0) {
          // regcomp() failed - regex structure is in undefined state
          // regerror() is safe to call with the error code even after failed regcomp()
          // Do NOT call regfree() on a failed regcomp() - it's unsafe
          char errbuf[REGEX_ERROR_BUFFER_SIZE];
          regerror(ret, &regex, errbuf, sizeof(errbuf));
          int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                              "Invalid regex pattern: %s", errbuf);
          value_release(pattern_arg);
          value_release(string_arg);
          return err;
        }

        regmatch_t match;
        int found =
            regexec(&regex, string_arg->as.string.data, 1, &match, 0) == 0;

        KronosValue *result;
        if (found && match.rm_so >= 0) {
          // Extract matched substring
          size_t match_len = (size_t)(match.rm_eo - match.rm_so);
          result = value_new_string(string_arg->as.string.data + match.rm_so,
                                    match_len);
        } else {
          // No match - return nil
          result = value_new_nil();
        }
        regfree(&regex);

        if (!result) {
          value_release(pattern_arg);
          value_release(string_arg);
          return vm_error(vm, KRONOS_ERR_INTERNAL,
                          "Failed to create result value");
        }
        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        value_release(result);
        value_release(pattern_arg);
        value_release(string_arg);
        break;
      }

      // regex.findall(string, pattern) - Find all matches in string
      if (strcmp(func_name, "regex.findall") == 0 ||
          strcmp(func_name, "findall") == 0) {
        if (arg_count != 2) {
          return vm_errorf(
              vm, KRONOS_ERR_RUNTIME,
              "Function 'regex.findall' expects 2 arguments, got %d",
              arg_count);
        }
        KronosValue *pattern_arg = pop(vm);
        if (!pattern_arg) {
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        KronosValue *string_arg = pop(vm);
        if (!string_arg) {
          value_release(pattern_arg);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        if (pattern_arg->type != VAL_STRING || string_arg->type != VAL_STRING) {
          int err =
              vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'regex.findall' requires string arguments");
          value_release(pattern_arg);
          value_release(string_arg);
          return err;
        }

        regex_t regex;
        int ret = regcomp(&regex, pattern_arg->as.string.data, REG_EXTENDED);
        if (ret != 0) {
          // regcomp() failed - regex structure is in undefined state
          // regerror() is safe to call with the error code even after failed regcomp()
          // Do NOT call regfree() on a failed regcomp() - it's unsafe
          char errbuf[REGEX_ERROR_BUFFER_SIZE];
          regerror(ret, &regex, errbuf, sizeof(errbuf));
          int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                              "Invalid regex pattern: %s", errbuf);
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
            return vm_error(vm, KRONOS_ERR_INTERNAL,
                            "Failed to create string value");
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
            // Zero-length match - advance by one character to avoid infinite
            // loop
            offset++;
          }
        }

        regfree(&regex);
        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        value_release(result);
        value_release(pattern_arg);
        value_release(string_arg);
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

        // Restore VM state
        // If return_ip is NULL, this is a module function call and we should
        // just break The return value is already on the stack, caller will
        // handle cleanup
        if (frame->return_ip == NULL && frame->return_bytecode == NULL) {
          // This is a module function call - return from vm_execute entirely
          // Push return value back onto stack (it was popped above)
          // push() retains the value (increments refcount), so we release our
          // local reference after pushing
          if (push(vm, return_value) != 0) {
            value_release(return_value);
            return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
          }
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
        if (push(vm, return_value) != 0) {
          value_release(return_value);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        value_release(return_value); // Release our reference, stack now owns it
      } else {
        // Top-level return (shouldn't happen in normal code)
        // push() retains the value (increments refcount), so we release our
        // local reference after pushing
        if (push(vm, return_value) != 0) {
          value_release(return_value);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
        value_release(return_value); // Release our reference, stack now owns it
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
      if (push(vm, list) != 0) {
        value_release(list);
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
      value_release(list);
      break;
    }

    case OP_RANGE_NEW: {
      // Stack: [start, end, step]
      // Pop step, end, start and create range
      KronosValue *step_val = pop(vm);
      if (!step_val) {
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
      KronosValue *end_val = pop(vm);
      if (!end_val) {
        value_release(step_val);
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
      KronosValue *start_val = pop(vm);
      if (!start_val) {
        value_release(step_val);
        value_release(end_val);
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }

      // All must be numbers
      if (start_val->type != VAL_NUMBER || end_val->type != VAL_NUMBER ||
          step_val->type != VAL_NUMBER) {
        value_release(start_val);
        value_release(end_val);
        value_release(step_val);
        return vm_error(vm, KRONOS_ERR_RUNTIME,
                        "Range start, end, and step must be numbers");
      }

      KronosValue *range = value_new_range(
          start_val->as.number, end_val->as.number, step_val->as.number);
      if (!range) {
        value_release(start_val);
        value_release(end_val);
        value_release(step_val);
        return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create range");
      }

      if (push(vm, range) != 0) {
        value_release(range);
        value_release(start_val);
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }

      push(vm, range);
      value_release(range);
      value_release(start_val);
      value_release(end_val);
      value_release(step_val);
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
      if (push(vm, list) != 0) {
        value_release(list);
        value_release(value);
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }

      push(vm, list);
      value_release(list);
      value_release(value);
      break;
    }

    case OP_MAP_NEW: {
      // Read entry count from bytecode (unused, but kept for consistency)
      uint16_t count = (uint16_t)(read_byte(vm) << 8 | read_byte(vm));
      (void)count; // Unused, maps grow dynamically
      KronosValue *map = value_new_map(0);
      if (!map) {
        return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create map");
      }
      if (push(vm, map) != 0) {
        value_release(map);
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }

      push(vm, map);
      value_release(map);
      break;
    }

    case OP_MAP_SET: {
      // Stack: [map, key, value]
      KronosValue *value = pop(vm);
      if (!value) {
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
      KronosValue *key = pop(vm);
      if (!key) {
        value_release(value);
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
      KronosValue *map = pop(vm);
      if (!map) {
        value_release(key);
        value_release(value);
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }

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

      if (push(vm, map) != 0) {
        value_release(map);
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }

      push(vm, map);
      value_release(map);
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

      // Handle maps first (they accept any key type)
      if (container->type == VAL_MAP) {
        KronosValue *value = map_get(container, index_val);
        if (!value) {
          value_release(index_val);
          value_release(container);
          return vm_error(vm, KRONOS_ERR_RUNTIME, "Map key not found");
        }
        value_retain(value);
        if (push(vm, value) != 0) {
          value_release(value);
          value_release(index_val);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }

        push(vm, value);
        value_release(value);
        value_release(index_val);
        value_release(container);
        break;
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
        if (push(vm, item) != 0) {
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }

        push(vm, item);
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
            return vm_error(vm, KRONOS_ERR_RUNTIME,
                            "Range step cannot be zero");
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
        if (push(vm, result) != 0) {
          value_release(result);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }
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
          return vm_error(vm, KRONOS_ERR_INTERNAL,
                          "Failed to create string value");
        }
        if (push(vm, char_str) != 0) {
          value_release(char_str);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }

        push(vm, char_str);
        value_release(char_str);
      } else {
        // Note: Maps are handled earlier in this function with an early break
        value_release(index_val);
        value_release(container);
        return vm_error(
            vm, KRONOS_ERR_RUNTIME,
            "Indexing only supported for lists, strings, ranges, and maps");
      }

      value_release(index_val);
      value_release(container);
      break;
    }

    case OP_LIST_SET: {
      // Stack: [list, index, value]
      KronosValue *value = pop(vm);
      if (!value) {
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
      KronosValue *index_val = pop(vm);
      if (!index_val) {
        value_release(value);
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
      KronosValue *list = pop(vm);
      if (!list) {
        value_release(index_val);
        value_release(value);
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }

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
      if (push(vm, list) != 0) {
        value_release(list);
        value_release(index_val);
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }

      push(vm, list);
      value_release(list);
      value_release(index_val);
      value_release(value);
      break;
    }

    case OP_DELETE: {
      // Stack: [map, key]
      KronosValue *key = pop(vm);
      if (!key) {
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
      KronosValue *map = pop(vm);
      if (!map) {
        value_release(key);
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }

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
      if (push(vm, map) != 0) {
        value_release(map);
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }

      push(vm, map);
      value_release(map);
      break;
    }

    case OP_TRY_ENTER: {
      // Read exception handler offset
      uint16_t handler_offset = (uint16_t)(read_byte(vm) << 8 | read_byte(vm));

      if (vm->exception_handler_count >= EXCEPTION_HANDLERS_MAX) {
        return vm_error(vm, KRONOS_ERR_RUNTIME, "Too many nested try blocks");
      }

      // Validate handler offset is within bytecode bounds
      uint8_t *handler_ip = vm->ip + handler_offset;
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
      vm->exception_handlers[idx].has_finally =
          false; // Will be set by OP_FINALLY
      vm->exception_handlers[idx].finally_ip = NULL;

      break;
    }

    case OP_TRY_EXIT: {
      // Normal completion of try block - read finally jump offset
      uint16_t finally_offset = (uint16_t)(read_byte(vm) << 8 | read_byte(vm));

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

      break;
    }

    case OP_CATCH: {
      // Read error type constant (0xFFFF means catch all)
      uint16_t error_type_idx = read_uint16(vm);
      // Read catch variable name constant (0xFFFF means no variable)
      uint16_t catch_var_idx = read_uint16(vm);
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
          KronosValue *error_val =
              value_new_string(error_msg, strlen(error_msg));
          if (error_val) {
            if (push(vm, error_val) != 0) {
              value_release(error_val);
              return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
            }
            value_release(error_val);
          } else {
            // Fallback - push empty string
            KronosValue *empty = value_new_string("", 0);
            if (push(vm, empty) != 0) {
              value_release(empty);
              return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
            }
            value_release(empty);
          }

          // Clear error - exception is now handled
          vm_clear_error(vm);
        }
        // If not matched, continue to next OP_CATCH or fall through to finally
      }
      // If no error, this is normal execution - just skip the catch block
      // operands The actual catch block code will be skipped during normal try
      // execution
      break;
    }

    case OP_FINALLY: {
      if (vm->exception_handler_count == 0) {
        return vm_error(vm, KRONOS_ERR_INTERNAL,
                        "OP_FINALLY without matching OP_TRY_ENTER");
      }

      // Mark that finally block exists
      size_t idx = vm->exception_handler_count - 1;
      vm->exception_handlers[idx].has_finally = true;
      vm->exception_handlers[idx].finally_ip = vm->ip;

      break;
    }

    case OP_THROW: {
      // Read error type constant (0xFFFF means generic Error)
      uint16_t error_type_idx = read_uint16(vm);

      // Pop error message from stack
      KronosValue *message_val = pop(vm);
      if (!message_val) {
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }

      // Get error message as string
      const char *message = "Unknown error";
      if (message_val->type == VAL_STRING) {
        message = message_val->as.string.data;
      }

      // Get error type name
      const char *type_name = "Error";
      if (error_type_idx != 0xFFFF &&
          error_type_idx < vm->bytecode->const_count) {
        KronosValue *type_val = vm->bytecode->constants[error_type_idx];
        if (type_val && type_val->type == VAL_STRING) {
          type_name = type_val->as.string.data;
        }
      }

      // Set error with type
      vm_set_error_with_type(vm, KRONOS_ERR_RUNTIME, type_name, message);
      value_release(message_val);

      // Continue to start of loop where handle_exception_if_any will handle it
      continue;
    }

    case OP_LIST_LEN: {
      KronosValue *container = pop(vm);
      if (!container) {
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }

      if (container->type == VAL_LIST) {
        KronosValue *len = value_new_number((double)container->as.list.count);
        if (push(vm, len) != 0) {
          value_release(len);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }

        push(vm, len);
        value_release(len);
      } else if (container->type == VAL_STRING) {
        KronosValue *len =
            value_new_number((double)container->as.string.length);
        if (push(vm, len) != 0) {
          value_release(len);
          value_release(container);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }

        push(vm, len);
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
        if (push(vm, len) != 0) {
          value_release(len);
          value_release(container);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }

        push(vm, len);
        value_release(len);
      } else {
        value_release(container);
        return vm_error(vm, KRONOS_ERR_RUNTIME,
                        "Expected list, string, or range for length");
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
        return vm_error(vm, KRONOS_ERR_RUNTIME,
                        "Slice indices must be numbers");
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

        if (push(vm, slice) != 0) {
          value_release(slice);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
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
          return vm_error(vm, KRONOS_ERR_INTERNAL,
                          "Failed to create string value");
        }

        if (push(vm, slice) != 0) {
          value_release(slice);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }

        push(vm, slice);
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

        if (push(vm, slice) != 0) {
          value_release(slice);
          value_release(container);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }

        push(vm, slice);
        value_release(slice);
      } else {
        value_release(container);
        value_release(start_val);
        value_release(end_val);
        return vm_error(
            vm, KRONOS_ERR_RUNTIME,
            "Slicing only supported for lists, strings, and ranges");
      }

      value_release(container);
      value_release(start_val);
      value_release(end_val);
      break;
    }

    case OP_LIST_ITER: {
      KronosValue *iterable = pop(vm);
      if (!iterable) {
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }

      if (iterable->type == VAL_LIST) {
        // Create iterator (just push the list and current index)
        // Push list back to stack, then push index 0
        if (push(vm, iterable) != 0) {
          value_release(iterable);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }

        push(vm, iterable);
        KronosValue *index = value_new_number(0);
        if (push(vm, index) != 0) {
          value_release(index);
          value_release(iterable);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }

        push(vm, index);
        value_release(index);
        value_release(iterable); // Release our pop reference
      } else if (iterable->type == VAL_RANGE) {
        // For ranges, push the range and current value (start)
        if (push(vm, iterable) != 0) {
          value_release(iterable);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }

        push(vm, iterable);
        KronosValue *current = value_new_number(iterable->as.range.start);
        if (push(vm, current) != 0) {
          value_release(current);
          value_release(iterable);
          return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
        }

        push(vm, current);
        value_release(current);
        value_release(iterable); // Release our pop reference
      } else {
        value_release(iterable);
        return vm_error(vm, KRONOS_ERR_RUNTIME,
                        "Expected list or range for iteration");
      }
      break;
    }

    case OP_LIST_NEXT: {
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
      KronosValue *state_val = pop(vm);
      if (!state_val) {
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }
      KronosValue *iterable = pop(vm);
      if (!iterable) {
        value_release(state_val);
        return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
      }

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
          if (push(vm, iterable) != 0) {
            value_release(iterable);
          }

          push(vm, iterable);
          value_release(iterable);

          // Update and push index
          KronosValue *next_index = value_new_number((double)(idx + 1));
          if (push(vm, next_index) != 0) {
          }

          push(vm, next_index);

          // Push item
          KronosValue *item = iterable->as.list.items[idx];
          value_retain(item);
          if (push(vm, item) != 0) {
          }

          push(vm, item);

          // Push has_more flag (true) on top
          KronosValue *has_more_val = value_new_bool(true);
          if (push(vm, has_more_val) != 0) {
            value_release(iterable);
            return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
          }

          push(vm, has_more_val);
        } else {
          // No more items - push iterator state back, then has_more flag
          // (false) Push list back (for cleanup)
          value_retain(iterable);
          if (push(vm, iterable) != 0) {
            value_release(iterable);
            value_release(state_val);
            return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
          }

          push(vm, iterable);
          value_release(iterable);

          // Push index back (for cleanup)
          value_retain(state_val);
          if (push(vm, state_val) != 0) {
            value_release(state_val);
          }

          push(vm, state_val);
          value_release(state_val);

          // Push has_more flag (false) on top
          KronosValue *has_more_val = value_new_bool(false);
          if (push(vm, has_more_val) != 0) {
            value_release(state_val);
            return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
          }

          push(vm, has_more_val);
        }
      } else if (iterable->type == VAL_RANGE) {
        // Range iteration: state_val is the current value
        if (state_val->type != VAL_NUMBER) {
          value_release(state_val);
          value_release(iterable);
          return vm_error(vm, KRONOS_ERR_RUNTIME, "Invalid iterator state");
        }

        double current = state_val->as.number;
        double end = iterable->as.range.end;
        double step = iterable->as.range.step;

        // Check if we've reached the end
        bool has_more = false;
        if (step > 0) {
          has_more = current <= end;
        } else if (step < 0) {
          has_more = current >= end;
        } else {
          // step == 0, only one value
          has_more = false;
        }

        if (has_more) {
          // Push in order: [range, next_value, current_value, has_more]
          value_retain(iterable);
          if (push(vm, iterable) != 0) {
            value_release(iterable);
          }

          push(vm, iterable);
          value_release(iterable);

          // Calculate and push next value
          double next = current + step;
          KronosValue *next_val = value_new_number(next);
          if (push(vm, next_val) != 0) {
          }

          push(vm, next_val);

          // Push current value (the item to use in loop)
          KronosValue *current_val = value_new_number(current);
          if (push(vm, current_val) != 0) {
          }

          push(vm, current_val);

          // Push has_more flag (true)
          KronosValue *has_more_val = value_new_bool(true);
          if (push(vm, has_more_val) != 0) {
            value_release(iterable);
            return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
          }

          push(vm, has_more_val);
        } else {
          // No more items
          value_retain(iterable);
          if (push(vm, iterable) != 0) {
            value_release(iterable);
            value_release(state_val);
            return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
          }

          push(vm, iterable);
          value_release(iterable);

          value_retain(state_val);
          if (push(vm, state_val) != 0) {
            value_release(state_val);
          }

          push(vm, state_val);
          value_release(state_val);

          KronosValue *has_more_val = value_new_bool(false);
          if (push(vm, has_more_val) != 0) {
            value_release(state_val);
            return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
          }

          push(vm, has_more_val);
        }
      } else {
        value_release(state_val);
        value_release(iterable);
        return vm_error(vm, KRONOS_ERR_RUNTIME, "Invalid iterator type");
      }

      value_release(state_val);
      value_release(iterable);
      break;
    }

    case OP_IMPORT: {
      // Read constant indices for module name and file path (in order of
      // emission)
      uint16_t module_name_idx = read_uint16(vm);
      uint16_t file_path_idx = read_uint16(vm);

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
        int load_result =
            vm_load_module(vm, module_name, file_path, vm->current_file_path,
                           root_vm_for_import);
        if (load_result < 0) {
          return load_result;
        }
      }

      // Constants are owned by bytecode, don't release
      break;
    }

    case OP_HALT: {
      return 0;
    }

    // Reserved/unused opcodes - these should never be emitted by the compiler
    default:
      return vm_errorf(
          vm, KRONOS_ERR_INTERNAL,
          "Unknown bytecode instruction: %d (this is a compiler bug)",
          instruction);
    }
  }

  return 0;
}
