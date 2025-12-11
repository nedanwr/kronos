/**
 * @file main.c
 * @brief Main entry point for the Kronos interpreter
 *
 * This file provides the public API for executing Kronos programs and manages
 * the REPL (Read-Eval-Print Loop) interface. It orchestrates the compilation
 * pipeline: tokenization -> parsing -> compilation -> execution.
 */

#define _POSIX_C_SOURCE 200809L
#include "include/kronos.h"
#include "src/compiler/compiler.h"
#include "src/core/runtime.h"
#include "src/frontend/parser.h"
#include "src/frontend/tokenizer.h"
#include "src/vm/vm.h"
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Version information
#define KRONOS_VERSION_MAJOR 0
#define KRONOS_VERSION_MINOR 4
#define KRONOS_VERSION_PATCH 0
#define KRONOS_VERSION_STRING "0.4.0"

// Global flag for graceful shutdown on signals
static volatile sig_atomic_t g_signal_received = 0;
static KronosVM *g_repl_vm =
    NULL; // VM instance for REPL (for cleanup on signal)

/**
 * @brief Print usage information
 */
static void print_usage(const char *program_name) {
  printf("Usage: %s [OPTIONS] [FILE...]\n", program_name);
  printf("\n");
  printf("Options:\n");
  printf("  -h, --help          Show this help message and exit\n");
  printf("  -v, --version       Show version information and exit\n");
  printf("  -d, --debug         Enable debug mode (future use)\n");
  printf("  -n, --no-color      Disable colored output (future use)\n");
  printf("\n");
  printf("If FILE is provided, executes the specified Kronos file(s).\n");
  printf("If no FILE is provided, starts the interactive REPL.\n");
  printf("\n");
  printf("Examples:\n");
  printf("  %s                    # Start REPL\n", program_name);
  printf("  %s script.kr          # Execute script.kr\n", program_name);
  printf("  %s file1.kr file2.kr # Execute multiple files\n", program_name);
}

/**
 * @brief Print version information
 */
static void print_version(void) {
  printf("Kronos %s\n", KRONOS_VERSION_STRING);
}

/**
 * @brief Print error message with consistent formatting
 *
 * All errors are prefixed with "Error: " for consistency.
 *
 * @param message Error message to print (must not be NULL)
 */
static void print_error(const char *message) {
  fprintf(stderr, "Error: %s\n", message);
}

/**
 * @brief Print error message with file context
 *
 * Used when an error occurs while processing a specific file.
 *
 * @param filepath Path to the file where error occurred
 * @param message Error message to print
 */
static void print_error_with_file(const char *filepath, const char *message) {
  fprintf(stderr, "Error in %s: %s\n", filepath, message);
}

/**
 * @brief Signal handler for SIGINT (Ctrl+C)
 *
 * Sets a flag to indicate graceful shutdown should occur.
 */
static void handle_sigint(int sig) {
  (void)sig; // Suppress unused parameter warning
  g_signal_received = 1;
  // Print newline to move cursor after ^C
  fprintf(stderr, "\n");
}

/**
 * @brief Signal handler for SIGTERM
 *
 * Sets a flag to indicate graceful shutdown should occur.
 */
static void handle_sigterm(int sig) {
  (void)sig; // Suppress unused parameter warning
  g_signal_received = 1;
}

/**
 * @brief Signal handler for SIGPIPE
 *
 * Handles broken pipe gracefully (e.g., when output is piped and reader
 * closes).
 */
static void handle_sigpipe(int sig) {
  (void)sig; // Suppress unused parameter warning
  // Ignore SIGPIPE - exit gracefully
  // The write operation will fail with EPIPE, which we can handle
  g_signal_received = 1;
}

/**
 * @brief Set up signal handlers for graceful shutdown
 *
 * Registers handlers for SIGINT, SIGTERM, and SIGPIPE.
 */
static void setup_signal_handlers(void) {
  signal(SIGINT, handle_sigint);
  signal(SIGTERM, handle_sigterm);
  signal(SIGPIPE, handle_sigpipe);
}

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
 * between different types of errors (tokenization, parsing, compilation,
 * runtime).
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
 * Errors are stored in the VM and can be retrieved with
 * kronos_get_last_error().
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
  AST *ast = parse(tokens, NULL);
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
  // Clear stack before freeing bytecode to ensure constants aren't retained
  vm_clear_stack(vm);
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

  // Set current file path for relative imports
  // Free previous path if it exists (safe to free NULL, but check for clarity)
  if (vm->current_file_path) {
    free(vm->current_file_path);
    vm->current_file_path = NULL;
  }
  vm->current_file_path = strdup(filepath);
  if (!vm->current_file_path) {
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to set current file path");
  }

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

  // Strip shebang line if present (e.g., #!/usr/bin/env kronos)
  // Shebang must be the first line and start with #!
  if (read_size >= 2 && source[0] == '#' && source[1] == '!') {
    // Find the end of the shebang line (first newline or end of string)
    char *shebang_end = strchr(source, '\n');
    if (shebang_end) {
      // Skip the shebang line including the newline
      size_t shebang_len = (size_t)(shebang_end - source) + 1;
      size_t remaining_len = read_size - shebang_len;

      // Move the remaining content to the start of the buffer
      memmove(source, source + shebang_len, remaining_len);
      source[remaining_len] = '\0';
    } else {
      // No newline found - entire file is shebang, set to empty string
      source[0] = '\0';
    }
  }

  // Execute the source code
  int result = kronos_run_string(vm, source);
  free(source);

  return result;
}

/**
 * @brief Read a line from stdin with dynamic buffer allocation
 *
 * Reads a complete line from stdin, allocating a buffer that grows as needed.
 * The caller is responsible for freeing the returned buffer.
 *
 * @return Pointer to allocated buffer containing the line (without newline),
 *         or NULL on EOF or allocation failure. Empty lines return empty
 * string.
 */
static char *read_line_dynamic(void) {
  size_t capacity = 256;
  size_t len = 0;
  char *buffer = malloc(capacity);
  if (!buffer)
    return NULL;

  int c;
  while ((c = getchar()) != EOF && c != '\n') {
    if (len + 1 >= capacity) {
      // Double capacity when buffer is full
      size_t new_capacity = capacity * 2;
      char *new_buffer = realloc(buffer, new_capacity);
      if (!new_buffer) {
        free(buffer);
        return NULL;
      }
      buffer = new_buffer;
      capacity = new_capacity;
    }
    buffer[len++] = (char)c;
  }

  if (len == 0 && c == EOF) {
    // EOF without any input
    free(buffer);
    return NULL;
  }

  buffer[len] = '\0';
  return buffer;
}

/**
 * @brief Read multi-line input until user finishes (empty line or EOF)
 *
 * Reads lines from stdin, accumulating them. Shows continuation prompts
 * ("... ") for additional lines. User finishes input by pressing Enter
 * on an empty line.
 *
 * @return Pointer to allocated buffer containing the complete input, or NULL on
 * EOF
 */
static char *read_multiline_input(void) {
  size_t capacity = 512;
  size_t len = 0;
  char *buffer = malloc(capacity);
  if (!buffer)
    return NULL;

  bool first_line = true;
  bool got_empty_line = false;

  while (1) {
    // Show prompt
    if (first_line) {
      printf(">>> ");
      first_line = false;
    } else {
      printf("... ");
    }
    fflush(stdout);

    // Read a line
    char *line = read_line_dynamic();
    if (!line) {
      // EOF - return what we have (might be empty)
      if (len == 0) {
        free(buffer);
        return NULL;
      }
      break;
    }

    // Check for exit command (only on first line)
    if (len == 0 && strcmp(line, "exit") == 0) {
      free(line);
      free(buffer);
      return NULL; // Signal to exit REPL
    }

    size_t line_len = strlen(line);

    // If we get an empty line after having content, that signals end of input
    if (line_len == 0 && len > 0) {
      free(line);
      break;
    }

    // If we got an empty line previously and this is also empty, break
    if (line_len == 0 && got_empty_line) {
      free(line);
      break;
    }

    if (line_len == 0) {
      got_empty_line = true;
      free(line);
      continue;
    }

    got_empty_line = false;

    // Calculate space needed: current buffer + new line + newline + null
    // terminator
    size_t needed = len + line_len + 2; // +2 for newline and null terminator

    // Expand buffer if needed
    if (needed >= capacity) {
      size_t new_capacity = capacity;
      while (new_capacity < needed) {
        new_capacity *= 2;
      }
      char *new_buffer = realloc(buffer, new_capacity);
      if (!new_buffer) {
        free(line);
        free(buffer);
        return NULL;
      }
      buffer = new_buffer;
      capacity = new_capacity;
    }

    // Append line to buffer
    if (len > 0) {
      buffer[len++] = '\n'; // Add newline between lines
    }
    memcpy(buffer + len, line, line_len);
    len += line_len;
    buffer[len] = '\0';

    free(line);
  }

  return buffer;
}

/**
 * @brief Start the Kronos REPL (Read-Eval-Print Loop)
 *
 * Provides an interactive command-line interface for executing Kronos code.
 * Supports multi-line input with continuation prompts. Reads input until a
 * complete statement is formed, then executes it.
 * Type 'exit' to quit the REPL.
 */
void kronos_repl(void) {
  printf("Kronos REPL - Type 'exit' to quit (or Ctrl+C)\n");

  KronosVM *vm = kronos_vm_new();
  if (!vm) {
    print_error("Failed to create VM");
    return;
  }

  // Store VM pointer for signal handler cleanup
  g_repl_vm = vm;

  while (1) {
    // Check for signal
    if (g_signal_received) {
      fprintf(stderr, "\nInterrupted. Exiting...\n");
      break;
    }

    // Read multi-line input
    char *input = read_multiline_input();
    if (!input) {
      // EOF or exit command
      break;
    }

    // Check for signal again after reading input
    if (g_signal_received) {
      free(input);
      fprintf(stderr, "\nInterrupted. Exiting...\n");
      break;
    }

    // Skip empty input
    if (strlen(input) == 0) {
      free(input);
      continue;
    }

    // Execute the input (should be complete at this point)
    int result = kronos_run_string(vm, input);
    if (result < 0) {
      const char *err = kronos_get_last_error(vm);
      if (err && *err) {
        print_error(err);
      }
    }

    free(input);
  }

  g_repl_vm = NULL;
  kronos_vm_free(vm);
}

/**
 * @brief Main entry point for the Kronos interpreter
 *
 * Parses command-line arguments and either executes files or starts the REPL.
 *
 * @param argc Number of command-line arguments
 * @param argv Command-line arguments
 * @return 0 on success, 1 on error
 */
int main(int argc, char **argv) {
  // Set up signal handlers for graceful shutdown
  setup_signal_handlers();

  // Command-line options
  static struct option long_options[] = {{"help", no_argument, 0, 'h'},
                                         {"version", no_argument, 0, 'v'},
                                         {"debug", no_argument, 0, 'd'},
                                         {"no-color", no_argument, 0, 'n'},
                                         {0, 0, 0, 0}};

  int opt;
  int option_index = 0;
  // Flags for future use (currently parsed but not implemented)
  __attribute__((unused)) bool debug_mode = false;
  __attribute__((unused)) bool no_color = false;

  // Parse command-line options
  while ((opt = getopt_long(argc, argv, "hvdn", long_options, &option_index)) !=
         -1) {
    switch (opt) {
    case 'h':
      print_usage(argv[0]);
      return 0;
    case 'v':
      print_version();
      return 0;
    case 'd':
      debug_mode = true;
      // TODO: Implement debug mode
      break;
    case 'n':
      no_color = true;
      // TODO: Implement no-color mode
      break;
    case '?':
      // Invalid option - getopt already printed error message
      return 1;
    default:
      return 1;
    }
  }

  // Remaining arguments are file paths
  int file_count = argc - optind;

  if (file_count == 0) {
    // REPL mode: start interactive interpreter
    kronos_repl();
    return 0;
  }

  // File execution mode: execute each specified file
  KronosVM *vm = kronos_vm_new();
  if (!vm) {
    print_error("Failed to create VM");
    return 1;
  }

  int exit_code = 0;
  for (int i = optind; i < argc; i++) {
    // Check for signal before processing each file
    if (g_signal_received) {
      fprintf(stderr, "\nInterrupted. Cleaning up...\n");
      exit_code = 130; // Standard exit code for SIGINT
      break;
    }

    int result = kronos_run_file(vm, argv[i]);
    if (result < 0) {
      const char *err = kronos_get_last_error(vm);
      if (err && *err) {
        print_error_with_file(argv[i], err);
      }
      exit_code = 1;
    }

    // Check for signal after processing each file
    if (g_signal_received) {
      fprintf(stderr, "\nInterrupted. Cleaning up...\n");
      exit_code = 130; // Standard exit code for SIGINT
      break;
    }
  }

  kronos_vm_free(vm);
  return exit_code;
}
