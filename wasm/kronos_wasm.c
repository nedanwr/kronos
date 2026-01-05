/**
 * @file kronos_wasm.c
 * @brief WebAssembly entry point for the Kronos interpreter
 *
 * This file provides a simplified API for running Kronos code in the browser
 * via WebAssembly. It exports functions that can be called from JavaScript.
 *
 * Compilation (requires Emscripten):
 *   emmake make wasm
 *
 * Exported functions:
 *   - kronos_wasm_init(): Initialize the WASM runtime
 *   - kronos_wasm_run(code): Execute Kronos code and return output
 *   - kronos_wasm_cleanup(): Clean up resources
 */

#include <emscripten/emscripten.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Include Kronos headers
#include "../include/kronos.h"
#include "../src/compiler/compiler.h"
#include "../src/core/runtime.h"
#include "../src/frontend/parser.h"
#include "../src/frontend/tokenizer.h"
#include "../src/vm/vm.h"

// Global VM instance for WASM
static KronosVM *g_wasm_vm = NULL;

// Output capture buffer
#define OUTPUT_BUFFER_SIZE (64 * 1024) // 64KB output buffer
static char g_output_buffer[OUTPUT_BUFFER_SIZE];
static size_t g_output_len = 0;

// Warning capture buffer (for compile-time warnings)
#define WARNING_BUFFER_SIZE (8 * 1024) // 8KB warning buffer
static char g_warning_buffer[WARNING_BUFFER_SIZE];
static size_t g_warning_len = 0;

// Error message buffer
#define ERROR_BUFFER_SIZE 4096
static char g_error_buffer[ERROR_BUFFER_SIZE];

/**
 * @brief Custom print function that captures output to buffer
 *
 * This overrides the default print behavior to capture all output
 * so it can be returned to JavaScript.
 */
static void capture_output(const char *str) {
  size_t len = strlen(str);
  if (g_output_len + len < OUTPUT_BUFFER_SIZE - 1) {
    memcpy(g_output_buffer + g_output_len, str, len);
    g_output_len += len;
    g_output_buffer[g_output_len] = '\0';
  }
}

/**
 * @brief Capture warning messages
 *
 * Called by the compiler for compile-time warnings via callback.
 */
static void wasm_warning_callback(const char *str) {
  size_t len = strlen(str);
  if (g_warning_len + len < WARNING_BUFFER_SIZE - 1) {
    memcpy(g_warning_buffer + g_warning_len, str, len);
    g_warning_len += len;
    g_warning_buffer[g_warning_len] = '\0';
  }
}

/**
 * @brief Custom printf for capturing output
 */
static int captured_printf(const char *format, ...) {
  va_list args;
  va_start(args, format);

  // Format to a temporary buffer
  char temp[4096];
  int len = vsnprintf(temp, sizeof(temp), format, args);
  va_end(args);

  if (len > 0) {
    capture_output(temp);
  }

  return len;
}

/**
 * @brief Initialize the Kronos WASM runtime
 *
 * Must be called before running any code.
 *
 * @return 1 on success, 0 on failure
 */
EMSCRIPTEN_KEEPALIVE
int kronos_wasm_init(void) {
  if (g_wasm_vm) {
    // Already initialized
    return 1;
  }

  runtime_init();
  g_wasm_vm = vm_new();

  if (!g_wasm_vm) {
    runtime_cleanup();
    return 0;
  }

  // Set up warning callback to capture compile-time warnings
  compiler_set_warning_callback(wasm_warning_callback);

  // Clear output buffer
  g_output_buffer[0] = '\0';
  g_output_len = 0;
  g_warning_buffer[0] = '\0';
  g_warning_len = 0;
  g_error_buffer[0] = '\0';

  return 1;
}

/**
 * @brief Run Kronos code and return output
 *
 * Executes the provided Kronos source code and returns the captured output.
 * If an error occurs, the error message is returned instead.
 *
 * @param source Kronos source code to execute
 * @return Pointer to output string (valid until next call or cleanup)
 */
EMSCRIPTEN_KEEPALIVE
const char *kronos_wasm_run(const char *source) {
  if (!g_wasm_vm) {
    snprintf(g_error_buffer, ERROR_BUFFER_SIZE,
             "Error: VM not initialized. Call kronos_wasm_init() first.");
    return g_error_buffer;
  }

  if (!source) {
    snprintf(g_error_buffer, ERROR_BUFFER_SIZE,
             "Error: No source code provided.");
    return g_error_buffer;
  }

  // Clear output buffer for new execution
  g_output_buffer[0] = '\0';
  g_output_len = 0;
  g_error_buffer[0] = '\0';
  g_warning_buffer[0] = '\0';
  g_warning_len = 0;

  // Clear any previous VM error state
  vm_clear_error(g_wasm_vm);

  // Step 1: Tokenize
  TokenizeError *tok_err = NULL;
  TokenArray *tokens = tokenize(source, &tok_err);
  if (!tokens) {
    if (tok_err && tok_err->message) {
      if (tok_err->line > 0) {
        snprintf(g_error_buffer, ERROR_BUFFER_SIZE,
                 "Error: Tokenization failed at line %zu, column %zu: %s",
                 tok_err->line, tok_err->column, tok_err->message);
      } else {
        snprintf(g_error_buffer, ERROR_BUFFER_SIZE,
                 "Error: Tokenization failed: %s", tok_err->message);
      }
      tokenize_error_free(tok_err);
    } else {
      snprintf(g_error_buffer, ERROR_BUFFER_SIZE, "Error: Tokenization failed");
    }
    return g_error_buffer;
  }

  // Step 2: Parse
  ParseError *parse_err = NULL;
  AST *ast = parse(tokens, &parse_err);
  token_array_free(tokens);

  if (!ast) {
    if (parse_err && parse_err->message) {
      if (parse_err->line > 0) {
        snprintf(g_error_buffer, ERROR_BUFFER_SIZE,
                 "Error: Parsing failed at line %zu, column %zu: %s",
                 parse_err->line, parse_err->column, parse_err->message);
      } else {
        snprintf(g_error_buffer, ERROR_BUFFER_SIZE, "Error: Parsing failed: %s",
                 parse_err->message);
      }
      parse_error_free(parse_err);
    } else {
      snprintf(g_error_buffer, ERROR_BUFFER_SIZE, "Error: Parsing failed");
    }
    return g_error_buffer;
  }

  // Step 3: Compile
  const char *compile_err = NULL;
  Bytecode *bytecode = compile(ast, &compile_err);
  ast_free(ast);

  if (!bytecode) {
    snprintf(g_error_buffer, ERROR_BUFFER_SIZE, "Error: Compilation failed%s%s",
             compile_err ? ": " : "", compile_err ? compile_err : "");
    return g_error_buffer;
  }

  // Step 4: Execute
  int result = vm_execute(g_wasm_vm, bytecode);
  vm_clear_stack(g_wasm_vm);
  bytecode_free(bytecode);

  if (result < 0) {
    const char *err = g_wasm_vm->last_error_message;
    if (err && *err) {
      snprintf(g_error_buffer, ERROR_BUFFER_SIZE, "Error: %s", err);
    } else {
      snprintf(g_error_buffer, ERROR_BUFFER_SIZE,
               "Error: Runtime execution failed");
    }
    return g_error_buffer;
  }

  // Return captured output (may be empty if no print statements)
  return g_output_buffer;
}

/**
 * @brief Get the last error message
 *
 * @return Error message string, or empty string if no error
 */
EMSCRIPTEN_KEEPALIVE
const char *kronos_wasm_get_error(void) {
  if (!g_wasm_vm) {
    return "VM not initialized";
  }

  const char *err = g_wasm_vm->last_error_message;
  return err ? err : "";
}

/**
 * @brief Get any warnings generated during compilation
 *
 * @return Warning messages string, or empty string if no warnings
 */
EMSCRIPTEN_KEEPALIVE
const char *kronos_wasm_get_warnings(void) { return g_warning_buffer; }

/**
 * @brief Clean up the Kronos WASM runtime
 *
 * Frees all resources. Call when done using the interpreter.
 */
EMSCRIPTEN_KEEPALIVE
void kronos_wasm_cleanup(void) {
  if (g_wasm_vm) {
    vm_free(g_wasm_vm);
    g_wasm_vm = NULL;
    runtime_cleanup();
  }

  g_output_buffer[0] = '\0';
  g_output_len = 0;
  g_error_buffer[0] = '\0';
}

/**
 * @brief Reset the VM state without full cleanup
 *
 * Clears all variables and functions but keeps the VM alive.
 * Useful for running multiple independent programs.
 */
EMSCRIPTEN_KEEPALIVE
void kronos_wasm_reset(void) {
  if (g_wasm_vm) {
    // Clear the VM state
    vm_clear_stack(g_wasm_vm);
    vm_clear_error(g_wasm_vm);

    // Clear globals and functions
    // Note: This is a simplified reset - full reset would recreate the VM
    for (size_t i = 0; i < g_wasm_vm->global_count; i++) {
      if (g_wasm_vm->globals[i].name) {
        free(g_wasm_vm->globals[i].name);
        g_wasm_vm->globals[i].name = NULL;
      }
      if (g_wasm_vm->globals[i].value) {
        value_release(g_wasm_vm->globals[i].value);
        g_wasm_vm->globals[i].value = NULL;
      }
    }
    g_wasm_vm->global_count = 0;

    // Clear output buffer
    g_output_buffer[0] = '\0';
    g_output_len = 0;
  }
}

/**
 * @brief Get the Kronos version string
 *
 * @return Version string (e.g., "0.4.5")
 */
EMSCRIPTEN_KEEPALIVE
const char *kronos_wasm_version(void) { return "0.4.5"; }

// Main function (required by Emscripten but not used)
int main(void) {
  // WASM is initialized via kronos_wasm_init()
  return 0;
}
