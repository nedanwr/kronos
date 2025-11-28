/**
 * @file main.c
 * @brief Main entry point for the Kronos interpreter
 * 
 * This file provides the public API for executing Kronos programs and manages
 * the REPL (Read-Eval-Print Loop) interface. It orchestrates the compilation
 * pipeline: tokenization -> parsing -> compilation -> execution.
 */

#include "include/kronos.h"
#include "src/compiler/compiler.h"
#include "src/core/runtime.h"
#include "src/frontend/parser.h"
#include "src/frontend/tokenizer.h"
#include "src/vm/vm.h"
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Create a new Kronos VM instance
 * 
 * Initializes the runtime system and creates a new virtual machine ready
 * to execute Kronos bytecode. The VM includes the built-in Pi constant.
 * 
 * @return Pointer to the new VM instance, or NULL on failure
 */
KronosVM *kronos_vm_new(void) {
  runtime_init();
  KronosVM *vm = vm_new();
  if (!vm) {
    runtime_cleanup();
    return NULL;
  }
  return vm;
}

/**
 * @brief Free a Kronos VM instance
 * 
 * Releases all resources associated with the VM, including global variables,
 * functions, and the runtime system. After calling this, the VM pointer
 * should not be used.
 * 
 * @param vm The VM instance to free (safe to pass NULL)
 */
void kronos_vm_free(KronosVM *vm) {
  if (!vm)
    return;

  vm_free(vm);
  runtime_cleanup();
}

/**
 * @brief Get the last error message from the VM
 * 
 * Returns a human-readable error message describing the most recent error
 * that occurred during execution. The message is owned by the VM and should
 * not be freed by the caller.
 * 
 * @param vm The VM instance
 * @return Error message string, or NULL if no error or vm is NULL
 */
const char *kronos_get_last_error(KronosVM *vm) {
  if (!vm)
    return NULL;
  return vm->last_error_message;
}

/**
 * @brief Get the last error code from the VM
 * 
 * Returns the error code for the most recent error. Use this to distinguish
 * between different types of errors (tokenization, parsing, compilation, runtime).
 * 
 * @param vm The VM instance
 * @return Error code, or KRONOS_ERR_INVALID_ARGUMENT if vm is NULL
 */
KronosErrorCode kronos_get_last_error_code(KronosVM *vm) {
  if (!vm)
    return KRONOS_ERR_INVALID_ARGUMENT;
  return vm->last_error_code;
}

/**
 * @brief Set an error callback function
 * 
 * Registers a callback that will be invoked whenever an error occurs during
 * execution. This allows custom error handling or logging.
 * 
 * @param vm The VM instance
 * @param callback Function to call on errors, or NULL to disable callbacks
 */
void kronos_set_error_callback(KronosVM *vm, KronosErrorCallback callback) {
  if (!vm)
    return;
  vm->error_callback = callback;
}

/**
 * @brief Execute Kronos source code from a string
 * 
 * Compiles and executes Kronos source code in a single call. This function
 * handles the full pipeline: tokenization, parsing, compilation, and execution.
 * Errors are stored in the VM and can be retrieved with kronos_get_last_error().
 * 
 * @param vm The VM instance to use for execution
 * @param source The Kronos source code to execute (must not be NULL)
 * @return 0 on success, negative error code on failure
 */
int kronos_run_string(KronosVM *vm, const char *source) {
  if (!vm || !source)
    return -(int)KRONOS_ERR_INVALID_ARGUMENT;

  vm_clear_error(vm);

  // Step 1: Tokenize - Convert source code into tokens
  TokenArray *tokens = tokenize(source, NULL);
  if (!tokens) {
    return vm_error(vm, KRONOS_ERR_TOKENIZE, "Tokenization failed");
  }

  // Step 2: Parse - Build Abstract Syntax Tree from tokens
  AST *ast = parse(tokens);
  token_array_free(tokens);

  if (!ast) {
    return vm_error(vm, KRONOS_ERR_PARSE, "Parsing failed");
  }

  // Step 3: Compile - Generate bytecode from AST
  const char *compile_err = NULL;
  Bytecode *bytecode = compile(ast, &compile_err);
  ast_free(ast);

  if (!bytecode) {
    return vm_errorf(vm, KRONOS_ERR_COMPILE, "Compilation failed%s%s",
                     compile_err ? ": " : "", compile_err ? compile_err : "");
  }

  // Step 4: Execute - Run bytecode on the virtual machine
  int result = vm_execute(vm, bytecode);
  bytecode_free(bytecode);

  if (result < 0 && vm->last_error_code == KRONOS_OK) {
    vm_set_error(vm, KRONOS_ERR_RUNTIME, "Runtime execution failed");
  }

  return result;
}

/**
 * @brief Execute a Kronos program from a file
 * 
 * Reads the contents of a file and executes it as Kronos source code.
 * Handles file I/O errors and validates file size to prevent memory issues.
 * 
 * @param vm The VM instance to use for execution
 * @param filepath Path to the .kr file to execute (must not be NULL)
 * @return 0 on success, negative error code on failure
 */
int kronos_run_file(KronosVM *vm, const char *filepath) {
  if (!vm || !filepath)
    return -(int)KRONOS_ERR_INVALID_ARGUMENT;

  vm_clear_error(vm);

  // Open file for reading
  FILE *file = fopen(filepath, "r");
  if (!file) {
    return vm_errorf(vm, KRONOS_ERR_NOT_FOUND, "Failed to open file: %s",
                     filepath);
  }

  // Determine file size by seeking to end
  if (fseek(file, 0, SEEK_END) != 0) {
    int err = vm_errorf(vm, KRONOS_ERR_IO, "Failed to seek to end of file: %s",
                        filepath);
    fclose(file);
    return err;
  }

  long size = ftell(file);
  if (size < 0) {
    int err = vm_errorf(vm, KRONOS_ERR_IO, "Failed to determine file size: %s",
                        filepath);
    fclose(file);
    return err;
  }

  // Validate file size to prevent integer overflow when allocating buffer
  // We need size+1 bytes for the null terminator
  if ((uintmax_t)size > (uintmax_t)(SIZE_MAX - 1)) {
    int err =
        vm_errorf(vm, KRONOS_ERR_IO, "File too large to read: %s", filepath);
    fclose(file);
    return err;
  }

  if (fseek(file, 0, SEEK_SET) != 0) {
    int err = vm_errorf(vm, KRONOS_ERR_IO,
                        "Failed to seek to start of file: %s", filepath);
    fclose(file);
    return err;
  }

  // Allocate buffer for file contents (size + 1 for null terminator)
  // Safe to cast after size validation above
  size_t length = (size_t)size;
  char *source = malloc(length + 1);
  if (!source) {
    int err = vm_error(vm, KRONOS_ERR_INTERNAL,
                       "Failed to allocate memory for file contents");
    fclose(file);
    return err;
  }

  size_t read_size = fread(source, 1, length, file);

  // Verify file was read successfully
  if (ferror(file)) {
    int err = vm_errorf(vm, KRONOS_ERR_IO, "Failed to read file: %s", filepath);
    free(source);
    fclose(file);
    return err;
  }

  // Ensure we read the complete file (partial reads indicate an error)
  if (read_size < length && !feof(file)) {
    int err =
        vm_errorf(vm, KRONOS_ERR_IO, "Incomplete read from file: %s", filepath);
    free(source);
    fclose(file);
    return err;
  }

  // Null-terminate the string (buffer is length+1, read_size <= length)
  source[read_size] = '\0';
  fclose(file);

  // Execute the source code
  int result = kronos_run_string(vm, source);
  free(source);

  return result;
}

/**
 * @brief Start the Kronos REPL (Read-Eval-Print Loop)
 * 
 * Provides an interactive command-line interface for executing Kronos code.
 * Reads input line by line, executes it, and prints results or errors.
 * Type 'exit' to quit the REPL.
 */
void kronos_repl(void) {
  printf("Kronos REPL - Type 'exit' to quit\n");

  KronosVM *vm = kronos_vm_new();
  if (!vm) {
    fprintf(stderr, "Failed to create VM\n");
    return;
  }

  char line[1024];

  while (1) {
    printf(">>> ");
    fflush(stdout);

    if (!fgets(line, sizeof(line), stdin)) {
      break;
    }

    // Check if input was truncated (line too long for buffer)
    // If no newline found and not at EOF, the line was truncated
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] != '\n' && !feof(stdin)) {
      fprintf(stderr, "Warning: Input line truncated (max %zu chars)\n",
              sizeof(line) - 1);
      int c;
      while ((c = getchar()) != '\n' && c != EOF)
        ;
    }

    // Remove trailing newline character
    if (len > 0 && line[len - 1] == '\n') {
      line[len - 1] = '\0';
    }

    // Check for exit command
    if (strcmp(line, "exit") == 0) {
      break;
    }

    // Skip empty lines (user just pressed Enter)
    if (strlen(line) == 0) {
      continue;
    }

    // Execute the line and print any errors
    if (kronos_run_string(vm, line) < 0) {
      const char *err = kronos_get_last_error(vm);
      if (err && *err) {
        fprintf(stderr, "Error: %s\n", err);
      }
    }
  }

  kronos_vm_free(vm);
}

/**
 * @brief Main entry point for the Kronos interpreter
 * 
 * If a file path is provided as a command-line argument, executes that file.
 * Otherwise, starts the interactive REPL.
 * 
 * @param argc Number of command-line arguments
 * @param argv Command-line arguments (argv[1] is optional file path)
 * @return 0 on success, 1 on error
 */
int main(int argc, char **argv) {
  if (argc > 1) {
    // File execution mode: compile and run the specified file
    KronosVM *vm = kronos_vm_new();
    if (!vm) {
      fprintf(stderr, "Failed to create VM\n");
      return 1;
    }

    int result = kronos_run_file(vm, argv[1]);
    if (result < 0) {
      const char *err = kronos_get_last_error(vm);
      if (err && *err) {
        fprintf(stderr, "Error: %s\n", err);
      }
    }
    kronos_vm_free(vm);

    // Convert negative error codes to standard exit code 1
    return (result < 0) ? 1 : result;
  } else {
    // REPL mode: start interactive interpreter
    kronos_repl();
    return 0;
  }
}
