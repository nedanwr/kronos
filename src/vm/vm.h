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

// Function definition
typedef struct {
  char *name;
  char **params;
  size_t param_count;
  Bytecode bytecode; // Full bytecode structure
} Function;

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

  // Instruction pointer
  uint8_t *ip;

  // Current bytecode
  Bytecode *bytecode;

  // Error tracking
  char *last_error_message;
  KronosErrorCode last_error_code;
  KronosErrorCallback error_callback;
} KronosVM;

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
 * @brief Execute compiled bytecode in the VM.
 *
 * Runs the bytecode instruction-by-instruction until completion or error.
 * The bytecode remains owned by the caller and is not modified.
 *
 * @param vm VM instance to execute on (must not be NULL).
 * @param bytecode Compiled bytecode to execute (must not be NULL, caller
 * retains ownership).
 * @return 0 on successful execution, negative KronosErrorCode on runtime error.
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
void vm_set_error(KronosVM *vm, KronosErrorCode code, const char *message);
void vm_set_errorf(KronosVM *vm, KronosErrorCode code, const char *fmt, ...);
int vm_error(KronosVM *vm, KronosErrorCode code, const char *message);
int vm_errorf(KronosVM *vm, KronosErrorCode code, const char *fmt, ...);
void vm_clear_error(KronosVM *vm);

#endif // KRONOS_VM_H
