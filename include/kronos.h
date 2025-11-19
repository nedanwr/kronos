#ifndef KRONOS_H
#define KRONOS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Public API for Kronos runtime
// This header provides the main interface for executing Kronos programs

// Forward declarations
typedef struct KronosVM KronosVM;

// Error reporting
typedef enum {
  KRONOS_OK = 0,
  KRONOS_ERR_INVALID_ARGUMENT,
  KRONOS_ERR_NOT_FOUND,
  KRONOS_ERR_IO,
  KRONOS_ERR_TOKENIZE,
  KRONOS_ERR_PARSE,
  KRONOS_ERR_COMPILE,
  KRONOS_ERR_RUNTIME,
  KRONOS_ERR_INTERNAL,
} KronosErrorCode;

typedef void (*KronosErrorCallback)(KronosVM *vm, KronosErrorCode code,
                                    const char *message);

/**
 * Retrieve the last error message recorded on a VM.
 *
 * Parameters:
 *   vm - VM instance (may be NULL).
 * Returns: Pointer to error string owned by the VM or NULL if none.
 */
const char *kronos_get_last_error(KronosVM *vm);

/**
 * Retrieve the last error code recorded on a VM.
 *
 * Parameters:
 *   vm - VM instance (may be NULL).
 * Returns: KronosErrorCode describing the last failure (KRONOS_OK if none).
 */
KronosErrorCode kronos_get_last_error_code(KronosVM *vm);

/**
 * Register a callback invoked whenever the VM records a new error.
 *
 * Parameters:
 *   vm - VM instance (must not be NULL).
 *   callback - Function pointer to invoke (NULL to clear).
 */
void kronos_set_error_callback(KronosVM *vm, KronosErrorCallback callback);

/**
 * Create a new Kronos virtual machine instance.
 *
 * Returns: Pointer to new VM on success, NULL on allocation failure.
 * Thread-safety: NOT thread-safe. Each VM instance must be used by a single
 * thread.
 */
KronosVM *kronos_vm_new(void);

/**
 * Free a Kronos virtual machine instance and all associated resources.
 *
 * Parameters:
 *   vm - VM to free (may be NULL, in which case this is a no-op).
 * Thread-safety: NOT thread-safe. Do not call concurrently on the same VM.
 */
void kronos_vm_free(KronosVM *vm);

/**
 * Execute a Kronos source file.
 *
 * Parameters:
 *   vm       - VM instance (must not be NULL).
 *   filepath - Path to .kr file to execute (must not be NULL).
 * Returns:
 *   0 on success.
 *   Negative error code on failure (e.g., -KRONOS_ERR_NOT_FOUND,
 *   -KRONOS_ERR_PARSE, -KRONOS_ERR_RUNTIME).
 *   All errors return negative values for legacy compatibility.
 * Thread-safety: NOT thread-safe. Do not call concurrently on the same VM.
 */
int kronos_run_file(KronosVM *vm, const char *filepath);

/**
 * Execute a Kronos source string.
 *
 * Parameters:
 *   vm     - VM instance (must not be NULL).
 *   source - Kronos source code to execute (must not be NULL).
 * Returns:
 *   0 on success.
 *   Negative error code on failure (e.g., -KRONOS_ERR_PARSE,
 *   -KRONOS_ERR_RUNTIME).
 *   All errors return negative values for legacy compatibility.
 * Thread-safety: NOT thread-safe. Do not call concurrently on the same VM.
 */
int kronos_run_string(KronosVM *vm, const char *source);

/**
 * Start an interactive Read-Eval-Print Loop (REPL).
 *
 * Reads lines from stdin, executes them, and continues until "exit" is entered
 * or EOF is reached. Creates its own VM instance internally.
 *
 * Thread-safety: NOT thread-safe. Should only be called from main thread.
 */
void kronos_repl(void);

#ifdef __cplusplus
}
#endif

#endif // KRONOS_H
