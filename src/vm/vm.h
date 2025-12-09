#ifndef KRONOS_VM_H
#define KRONOS_VM_H

#include "../../include/kronos.h"
#include "../compiler/compiler.h"
#include "../core/runtime.h"
#include <stdbool.h>
#include <stddef.h>

#define STACK_MAX 1024
#define GLOBALS_MAX 256
#define FUNCTIONS_MAX 128
#define CALL_STACK_MAX 256
#define LOCALS_MAX 64
#define MODULES_MAX 64
#define EXCEPTION_HANDLERS_MAX 64

// Function definition
typedef struct {
  char *name;
  char **params;
  size_t param_count;
  Bytecode bytecode; // Full bytecode structure
} Function;

// Module definition (for file-based modules)
typedef struct {
  char *name;      // Module name (namespace)
  char *file_path; // Source file path
  KronosVM *
      module_vm; // VM instance for this module (contains its globals/functions)
  KronosVM
      *root_vm; // Root VM that owns this module (for circular import detection)
  bool is_loaded; // Whether module has been loaded
} Module;

// Call frame for function calls
typedef struct {
  Function *function;
  uint8_t *return_ip;        // Where to return to
  Bytecode *return_bytecode; // Which bytecode to return to
  KronosValue **frame_start; // Start of this frame's stack

  // Local variables (includes parameters)
  struct {
    char *name;
    KronosValue *value;
    bool is_mutable;
    char *type_name; // NULL if no type restriction
  } locals[LOCALS_MAX];
  size_t local_count;
} CallFrame;

// Virtual machine state
typedef struct KronosVM {
  // Value stack
  KronosValue *stack[STACK_MAX];
  KronosValue **stack_top;

  // Call stack
  CallFrame call_stack[CALL_STACK_MAX];
  size_t call_stack_size;
  CallFrame *current_frame;

  // Global variables
  struct {
    char *name;
    KronosValue *value;
    bool is_mutable;
    char *type_name; // NULL if no type restriction
  } globals[GLOBALS_MAX];
  size_t global_count;

  // Functions
  Function *functions[FUNCTIONS_MAX];
  size_t function_count;

  // Modules (file-based modules)
  Module *modules[MODULES_MAX];
  size_t module_count;

  // Module loading tracking (for circular import detection)
  char *loading_modules[MODULES_MAX]; // Stack of modules currently being loaded
  size_t loading_count;

  // Current file path (for relative import resolution)
  char *current_file_path;

  // Root VM reference (for module VMs - points to the VM that created this
  // module)
  KronosVM *root_vm_ref; // NULL for root VM, non-NULL for module VMs

  // Instruction pointer
  uint8_t *ip;

  // Current bytecode
  Bytecode *bytecode;

  // Error tracking
  char *last_error_message;
  char *last_error_type; // Error type name (e.g., "ValueError")
  KronosErrorCode last_error_code;
  KronosErrorCallback error_callback;

  // Exception handler stack (for try/catch/finally)
  struct {
    uint8_t *handler_ip;     // IP of exception handler (catch or finally)
    uint8_t *try_start_ip;   // IP where try block started
    uint8_t *catch_start_ip; // IP where catch blocks start
    size_t catch_count;      // Number of catch blocks
    bool has_finally;        // Whether finally block exists
    uint8_t *finally_ip;     // IP of finally block (if exists)
  } exception_handlers[EXCEPTION_HANDLERS_MAX];
  size_t exception_handler_count;
} KronosVM;

// VM API Error Handling Strategy:
// - All functions return 0 on success, negative KronosErrorCode on failure
// - Error details are stored in vm->last_error_code and vm->last_error_message
// - Use kronos_get_last_error_code() and kronos_get_last_error() (from
//   include/kronos.h) to retrieve error information
// - No functions call exit() - all errors are returned to the caller

/**
 * @brief Create a new Kronos virtual machine instance.
 *
 * Allocates and initializes a new VM with empty stacks, no variables, and no
 * functions. The Pi constant is pre-initialized as an immutable global.
 *
 * @return Pointer to new VM on success, NULL on allocation failure.
 * @note Caller must call vm_free() to release resources.
 * @note Thread-safety: VM is NOT thread-safe. Caller must synchronize access.
 */
KronosVM *vm_new(void);

/**
 * @brief Free a Kronos virtual machine and all associated resources.
 *
 * Releases all values on the stack, all global/local variables, all functions,
 * and the VM structure itself. After calling, the vm pointer is invalid.
 *
 * @param vm VM to free (may be NULL, in which case this is a no-op).
 * @note Thread-safety: VM is NOT thread-safe. Caller must synchronize access.
 */
void vm_free(KronosVM *vm);

/**
 * @brief Clear the VM stack, releasing all values
 *
 * This should be called before freeing bytecode to ensure constants
 * aren't retained on the stack, which would prevent them from being freed.
 *
 * @param vm VM instance
 */
void vm_clear_stack(KronosVM *vm);

/**
 * @brief Execute compiled bytecode in the VM.
 *
 * Runs the bytecode instruction-by-instruction until completion or error.
 * The bytecode remains owned by the caller and is not modified.
 *
 * @param vm VM instance to execute on (must not be NULL).
 * @param bytecode Compiled bytecode to execute (must not be NULL, caller
 * retains ownership).
 * @return 0 on successful execution, negative KronosErrorCode on error.
 * @note On error, use kronos_get_last_error_code() and kronos_get_last_error()
 * (from include/kronos.h) to retrieve detailed error information.
 * @note Thread-safety: VM is NOT thread-safe. Caller must synchronize access.
 */
int vm_execute(KronosVM *vm, Bytecode *bytecode);

/**
 * @brief Set or update a global variable in the VM.
 *
 * If the variable already exists:
 * - Checks mutability (errors if reassigning an immutable variable).
 * - Checks type (errors if type_name was set and new value doesn't match).
 * If the variable is new, it is created.
 *
 * @param vm VM instance (must not be NULL).
 * @param name Variable name (copied internally via strdup, caller retains
 * ownership).
 * @param value Value to store. On success the VM retains the value (increments
 * refcount); on failure, ownership stays with the caller.
 * @param is_mutable true for mutable (let), false for immutable (set).
 * @param type_name Type constraint ("number", "string", "boolean", or NULL for
 * untyped). Copied internally via strdup if non-NULL; on failure, caller
 * retains ownership.
 * @return 0 on success, negative KronosErrorCode on failure.
 * @note Thread-safety: VM is NOT thread-safe. Caller must synchronize access.
 */
int vm_set_global(KronosVM *vm, const char *name, KronosValue *value,
                  bool is_mutable, const char *type_name);

/**
 * @brief Get a global variable from the VM.
 *
 * @param vm VM instance (must not be NULL).
 * @param name Variable name to look up (must not be NULL).
 * @return Pointer to the value if found, NULL if not found.
 * @note Returned value is NOT owned by caller (do not free). It remains in the
 * VM.
 * @note To use the value beyond the current scope, call value_retain().
 * @note Thread-safety: VM is NOT thread-safe. Caller must synchronize access.
 */
KronosValue *vm_get_global(KronosVM *vm, const char *name);

/**
 * @brief Set or update a local variable in a call frame.
 *
 * Similar to vm_set_global but operates on function-local variables.
 * Checks mutability and type constraints if the variable already exists.
 *
 * @param vm VM instance for error reporting (must not be NULL).
 * @param frame Call frame (must not be NULL).
 * @param name Variable name (copied internally via strdup, caller retains
 * ownership).
 * @param value Value to store. On success the frame retains the value
 * (increments refcount); on failure, ownership stays with the caller.
 * @param is_mutable true for mutable (let), false for immutable (set).
 * @param type_name Type constraint or NULL (copied internally via strdup if
 * non-NULL; ownership stays with caller on failure).
 * @return 0 on success, negative KronosErrorCode on failure.
 * @note Thread-safety: VM is NOT thread-safe. Caller must synchronize access.
 */
int vm_set_local(KronosVM *vm, CallFrame *frame, const char *name,
                 KronosValue *value, bool is_mutable, const char *type_name);

/**
 * @brief Get a local variable from a call frame.
 *
 * @param frame Call frame (must not be NULL).
 * @param name Variable name to look up (must not be NULL).
 * @return Pointer to the value if found, NULL if not found.
 * @note Returned value is NOT owned by caller (do not free). It remains in the
 * frame.
 * @note To use the value beyond the current scope, call value_retain().
 * @note Thread-safety: VM is NOT thread-safe. Caller must synchronize access.
 */
KronosValue *vm_get_local(CallFrame *frame, const char *name);

/**
 * @brief Get a variable by name, checking local scope first, then global.
 *
 * If inside a function call (vm->current_frame != NULL), searches locals first.
 * Falls back to globals if not found locally.
 *
 * @param vm VM instance (must not be NULL).
 * @param name Variable name to look up (must not be NULL).
 * @return Pointer to the value if found, NULL if not found in either scope.
 * @note Returned value is NOT owned by caller (do not free).
 * @note To use the value beyond the current scope, call value_retain().
 * @note Thread-safety: VM is NOT thread-safe. Caller must synchronize access.
 */
KronosValue *vm_get_variable(KronosVM *vm, const char *name);

/**
 * @brief Define a new function in the VM.
 *
 * Registers a function so it can be called by name. If a function with the
 * same name exists, it is replaced (no error).
 *
 * @param vm VM instance (must not be NULL).
 * @param func Function to register. On success the VM takes ownership; on
 * failure, the caller retains ownership and must free the Function.
 * @return 0 on success, negative error code on failure (e.g., -ENOSPC if the
 * function table is full, -EINVAL for invalid arguments).
 * @note Thread-safety: VM is NOT thread-safe. Caller must synchronize access.
 */
int vm_define_function(KronosVM *vm, Function *func);

/**
 * @brief Get a function by name from the VM.
 *
 * @param vm VM instance (must not be NULL).
 * @param name Function name to look up (must not be NULL).
 * @return Pointer to the Function if found, NULL if not found.
 * @note Returned function is NOT owned by caller (do not free). It remains in
 * the VM.
 * @note Thread-safety: VM is NOT thread-safe. Caller must synchronize access.
 */
Function *vm_get_function(KronosVM *vm, const char *name);

/**
 * @brief Free a Function and all associated resources.
 *
 * Releases the function's parameter list, bytecode, and the Function structure
 * itself.
 *
 * @param func Function to free (may be NULL, in which case this is a no-op).
 * @note Called internally by vm_free(). Only call manually if you created the
 * Function outside the VM and never registered it.
 * @note Thread-safety: VM is NOT thread-safe. Caller must synchronize access.
 */
void function_free(Function *func);

// Error helpers

/**
 * @brief Set an error state on the VM without returning a value.
 *
 * Stores an error code and message in the VM's error tracking fields. The
 * message string is copied internally via strdup(), so the caller retains
 * ownership of the original message parameter.
 *
 * @param vm VM instance (may be NULL, in which case this is a no-op).
 * @param code Error code to store.
 * @param message Error message string (may be NULL). Copied internally; caller
 * retains ownership of the original.
 * @note If a previous error message exists, it is freed before storing the new
 * one.
 * @note If an error callback is registered and code != KRONOS_OK, the callback
 * is invoked with the error details.
 * @note Thread-safety: NOT thread-safe. Caller must synchronize access.
 */
void vm_set_error(KronosVM *vm, KronosErrorCode code, const char *message);

/**
 * @brief Set an error state using a printf-style format string.
 *
 * Formats an error message using vsnprintf() and stores it along with the error
 * code. The formatted message is allocated internally and owned by the VM.
 *
 * @param vm VM instance (may be NULL, in which case this is a no-op).
 * @param code Error code to store.
 * @param fmt printf-style format string (must not be NULL). Format specifiers
 * must match the provided arguments.
 * @param ... Variable arguments matching the format string.
 * @note The formatted message is allocated via malloc() and owned by the VM.
 * @note If formatting fails (e.g., invalid format string), the error code is
 * still stored but the message may be NULL or a fallback.
 * @note If a previous error message exists, it is freed before storing the new
 * one.
 * @note If an error callback is registered and code != KRONOS_OK, the callback
 * is invoked with the error details.
 * @note Thread-safety: NOT thread-safe. Caller must synchronize access.
 */
void vm_set_errorf(KronosVM *vm, KronosErrorCode code, const char *fmt, ...);

/**
 * @brief Set an error state and return an integer error code.
 *
 * Convenience function that sets the VM error state and returns a value
 * suitable for direct return from functions. Returns 0 for KRONOS_OK, negative
 * error code otherwise.
 *
 * @param vm VM instance (may be NULL, in which case error is not stored but
 * return value is still computed).
 * @param code Error code to store.
 * @param message Error message string (may be NULL). Copied internally via
 * strdup(); caller retains ownership of the original.
 * @return 0 if code == KRONOS_OK, otherwise -(int)code.
 * @note Useful for returning errors directly: `return vm_error(vm, code, msg);`
 * @note The message is copied internally and owned by the VM.
 * @note Thread-safety: NOT thread-safe. Caller must synchronize access.
 */
int vm_error(KronosVM *vm, KronosErrorCode code, const char *message);

/**
 * @brief Set an error state using printf-style formatting and return an error
 * code.
 *
 * Formats an error message and stores it along with the error code, then
 * returns a value suitable for direct return from functions.
 *
 * @param vm VM instance (may be NULL, in which case error is not stored but
 * return value is still computed).
 * @param code Error code to store.
 * @param fmt printf-style format string (must not be NULL). Format specifiers
 * must match the provided arguments.
 * @param ... Variable arguments matching the format string.
 * @return 0 if code == KRONOS_OK, otherwise -(int)code.
 * @note Useful for returning formatted errors: `return vm_errorf(vm, code,
 * "Failed: %s", reason);`
 * @note The formatted message is allocated internally and owned by the VM.
 * @note Thread-safety: NOT thread-safe. Caller must synchronize access.
 */
int vm_errorf(KronosVM *vm, KronosErrorCode code, const char *fmt, ...);

/**
 * @brief Clear the VM's error state.
 *
 * Frees any stored error message and resets the error code to KRONOS_OK. After
 * calling, the VM has no recorded error.
 *
 * @param vm VM instance (may be NULL, in which case this is a no-op).
 * @note Frees the stored error message (if any) via free().
 * @note Resets vm->last_error_code to KRONOS_OK.
 * @note Does not invoke the error callback.
 * @note Thread-safety: NOT thread-safe. Caller must synchronize access.
 */
void vm_clear_error(KronosVM *vm);

#endif // KRONOS_VM_H
