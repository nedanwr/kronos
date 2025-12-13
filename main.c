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
// linenoise - Line editing library for REPL (BSD License)
// Copyright (c) 2010-2023, Salvatore Sanfilippo <antirez at gmail dot com>
// Copyright (c) 2010-2013, Pieter Noordhuis <pcnoordhuis at gmail dot com>
// See linenoise.h and linenoise.c for full license and copyright information
#include "linenoise.h"
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
#include <strings.h>
#include <unistd.h>

// Version information
#define KRONOS_VERSION_MAJOR 0
#define KRONOS_VERSION_MINOR 4
#define KRONOS_VERSION_PATCH 0
#define KRONOS_VERSION_STRING "0.4.0"

// Global flag for graceful shutdown on signals
static volatile sig_atomic_t g_signal_received = 0;
static KronosVM *g_repl_vm =
    NULL; // VM instance for REPL (for cleanup on signal)

// Kronos keywords for tab completion
static const char *kronos_keywords[] = {
    "set",     "let",     "to",       "as",       "if",    "else",   "for",
    "while",   "break",   "continue", "in",       "range", "list",   "map",
    "at",      "from",    "end",      "function", "with",  "call",   "return",
    "import",  "true",    "false",    "null",     "is",    "equal",  "not",
    "greater", "less",    "than",     "and",      "or",    "print",  "plus",
    "minus",   "times",   "divided",  "by",       "mod",   "delete", "try",
    "catch",   "finally", "raise",    NULL};

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
  printf("  -e, --execute CODE  Execute CODE as Kronos code (can be used "
         "multiple times)\n");
  printf("\n");
  printf("If FILE is provided, executes the specified Kronos file(s).\n");
  printf("If -e is provided, executes the code and exits (does not start "
         "REPL).\n");
  printf("If no FILE or -e is provided, starts the interactive REPL.\n");
  printf("\n");
  printf("Examples:\n");
  printf("  %s                    # Start REPL\n", program_name);
  printf("  %s script.kr          # Execute script.kr\n", program_name);
  printf("  %s -e \"print 42\"      # Execute code without entering REPL\n",
         program_name);
  printf("  %s -e \"set x to 10\" -e \"print x\"  # Execute multiple -e "
         "commands\n",
         program_name);
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

  // Open file for reading (need to open first to canonicalize path)
  FILE *file = fopen(filepath, "r");
  if (!file) {
    return vm_errorf(vm, KRONOS_ERR_NOT_FOUND, "Failed to open file: %s",
                     filepath);
  }

  // Canonicalize the file path (resolve . and .. components, symlinks, etc.)
  // This ensures consistent paths for relative imports
  char *canonical_path = realpath(filepath, NULL);
  if (!canonical_path) {
    // realpath failed (e.g., file was deleted between open and realpath)
    // Fall back to original path, but this shouldn't happen in normal usage
    fclose(file);
    return vm_errorf(vm, KRONOS_ERR_IO, "Failed to canonicalize file path: %s",
                     filepath);
  }

  // Set current file path for relative imports (use canonicalized path)
  // Free previous path if it exists (safe to free NULL, but check for clarity)
  if (vm->current_file_path) {
    free(vm->current_file_path);
    vm->current_file_path = NULL;
  }
  vm->current_file_path = canonical_path; // realpath already allocated this

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
 * @brief Tab completion callback for linenoise
 *
 * Provides completions for Kronos keywords, function names, and variable names.
 * Only works in TTY mode (interactive terminal).
 */
static void completion_callback(const char *buf, linenoiseCompletions *lc) {
  // Only provide completions in TTY mode
  if (!isatty(STDIN_FILENO)) {
    return;
  }

  // Get the VM instance for function/variable names
  KronosVM *vm = g_repl_vm;
  if (!vm) {
    return;
  }

  // Find the last word boundary (space, newline, or start of string)
  const char *word_start = buf;
  const char *p = buf + strlen(buf);
  while (p > buf && p[-1] != ' ' && p[-1] != '\n' && p[-1] != '\t') {
    p--;
  }
  word_start = p;
  size_t word_len = strlen(buf) - (word_start - buf);

  // Complete keywords
  for (size_t i = 0; kronos_keywords[i] != NULL; i++) {
    if (strncmp(word_start, kronos_keywords[i], word_len) == 0) {
      linenoiseAddCompletion(lc, kronos_keywords[i]);
    }
  }

  // Complete function names
  for (size_t i = 0; i < vm->function_count; i++) {
    if (vm->functions[i] && vm->functions[i]->name) {
      if (strncmp(word_start, vm->functions[i]->name, word_len) == 0) {
        linenoiseAddCompletion(lc, vm->functions[i]->name);
      }
    }
  }

  // Complete global variable names
  for (size_t i = 0; i < vm->global_count; i++) {
    if (vm->globals[i].name) {
      if (strncmp(word_start, vm->globals[i].name, word_len) == 0) {
        linenoiseAddCompletion(lc, vm->globals[i].name);
      }
    }
  }
}

/**
 * @brief Read multi-line input using linenoise until user finishes (empty line
 * or EOF)
 *
 * Reads lines from stdin using linenoise for line editing and history,
 * accumulating them. Shows continuation prompts ("... ") for additional lines.
 * User finishes input by pressing Enter on an empty line.
 *
 * @return Pointer to allocated buffer containing the complete input, or NULL on
 * EOF. Caller must free the returned buffer.
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
    // Show prompt using linenoise
    const char *prompt = first_line ? ">>> " : "... ";
    char *line = linenoise(prompt);

    if (!line) {
      // EOF (Ctrl+D) - return what we have (might be empty)
      if (len == 0) {
        free(buffer);
        return NULL;
      }
      break;
    }

    // Check for exit command (only on first line) - case-insensitive
    // Support both "exit" and "quit" commands
    if (len == 0 &&
        (strcasecmp(line, "exit") == 0 || strcasecmp(line, "quit") == 0)) {
      linenoiseFree(line);
      free(buffer);
      return NULL; // Signal to exit REPL
    }

    size_t line_len = strlen(line);

    // If we get an empty line after having content, that signals end of input
    if (line_len == 0 && len > 0) {
      linenoiseFree(line);
      break;
    }

    // If we got an empty line previously and this is also empty, break
    if (line_len == 0 && got_empty_line) {
      linenoiseFree(line);
      break;
    }

    if (line_len == 0) {
      got_empty_line = true;
      linenoiseFree(line);
      continue;
    }

    got_empty_line = false;

    // Add non-empty line to history (only first line to avoid duplicates)
    // Only add to history if we're in TTY mode (interactive terminal)
    if (first_line && isatty(STDIN_FILENO)) {
      linenoiseHistoryAdd(line);
    }

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
        linenoiseFree(line);
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

    linenoiseFree(line);
    first_line = false;
  }

  return buffer;
}

/**
 * @brief Execute Kronos code as an expression and return the result value
 *
 * Attempts to parse and execute the source code as a single expression.
 * If execution succeeds, returns the evaluated value. The caller is responsible
 * for releasing the returned value.
 *
 * @param vm The VM instance to use for execution
 * @param source The Kronos source code to execute (must not be NULL)
 * @return Pointer to the result value if an expression was evaluated, NULL
 *         otherwise (not an expression or error). Caller must call
 *         value_release() on the returned value.
 */
static KronosValue *kronos_run_expression(KronosVM *vm, const char *source) {
  if (!vm || !source)
    return NULL;

  vm_clear_error(vm);

  // Step 1: Tokenize - Convert source code into tokens
  TokenArray *tokens = tokenize(source, NULL);
  if (!tokens) {
    return NULL;
  }

  // Step 2: Parse as expression - Build AST node for the expression
  ASTNode *expr_node = parse_expression_only(tokens, NULL);
  token_array_free(tokens);

  if (!expr_node) {
    // Not a valid expression
    return NULL;
  }

  // Step 3: Compile - Generate bytecode from expression AST node
  // We need to create a minimal AST with just this expression node
  AST *ast = malloc(sizeof(AST));
  if (!ast) {
    ast_node_free(expr_node);
    return NULL;
  }
  ast->capacity = 1;
  ast->count = 1;
  ast->statements = malloc(sizeof(ASTNode *));
  if (!ast->statements) {
    free(ast);
    ast_node_free(expr_node);
    return NULL;
  }
  ast->statements[0] = expr_node;

  const char *compile_err = NULL;
  Bytecode *bytecode = compile(ast, &compile_err);
  ast_free(ast); // This will free expr_node too

  if (!bytecode) {
    return NULL;
  }

  // Step 4: Execute - Run bytecode on the virtual machine
  int result = vm_execute(vm, bytecode);
  if (result < 0) {
    // Execution failed - clear stack and return NULL
    vm_clear_stack(vm);
    bytecode_free(bytecode);
    return NULL;
  }

  // Check if there's a value on the stack (expression result)
  KronosValue *expr_result = NULL;
  if (vm->stack_top > vm->stack) {
    // There's a value on the stack - this was an expression
    expr_result = vm->stack_top[-1];
    vm->stack_top--;
    // Retain the value since we're taking ownership
    value_retain(expr_result);
  }

  // Clear any remaining stack values
  vm_clear_stack(vm);
  bytecode_free(bytecode);

  return expr_result;
}

/**
 * @brief Start the Kronos REPL (Read-Eval-Print Loop)
 *
 * Provides an interactive command-line interface for executing Kronos code.
 * Supports multi-line input with continuation prompts. Reads input until a
 * complete statement is formed, then executes it.
 * Automatically prints expression results (like Python's interactive shell).
 * Type 'exit' to quit the REPL.
 */
void kronos_repl(void) {
  printf("Kronos REPL - Type 'exit' or 'quit' to quit (or Ctrl+C)\n");

  KronosVM *vm = kronos_vm_new();
  if (!vm) {
    print_error("Failed to create VM");
    return;
  }

  // Store VM pointer for signal handler cleanup and completion callback
  g_repl_vm = vm;

  // Set up linenoise only if stdin is a TTY (interactive terminal)
  // When stdin is a pipe (e.g., in CI), linenoise will handle it automatically
  // but we should only set up history/completion for interactive use
  bool is_tty = isatty(STDIN_FILENO) != 0;
  if (is_tty) {
    linenoiseSetCompletionCallback(completion_callback);
    linenoiseHistorySetMaxLen(100); // Store up to 100 history entries

    // Try to load history from file (ignore errors if file doesn't exist)
    linenoiseHistoryLoad(".kronos_history");
  }

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

    // Try to execute as an expression first (for REPL auto-printing)
    KronosValue *expr_result = kronos_run_expression(vm, input);
    if (expr_result) {
      // Successfully evaluated as expression - print result
      value_fprint(stdout, expr_result);
      printf("\n");
      value_release(expr_result);
    } else {
      // Not an expression - try as statement
      // Clear any error state from expression attempt
      vm_clear_error(vm);
      int result = kronos_run_string(vm, input);
      if (result < 0) {
        // Statement execution also failed - show error
        const char *err = kronos_get_last_error(vm);
        if (err && *err) {
          print_error(err);
        }
      }
      // If statement execution succeeded, no need to print (statements don't
      // return values)
    }

    free(input);
  }

  // Save history before exiting (only if we're in TTY mode)
  if (isatty(STDIN_FILENO)) {
    linenoiseHistorySave(".kronos_history");
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
  static struct option long_options[] = {
      {"help", no_argument, 0, 'h'},          {"version", no_argument, 0, 'v'},
      {"debug", no_argument, 0, 'd'},         {"no-color", no_argument, 0, 'n'},
      {"execute", required_argument, 0, 'e'}, {0, 0, 0, 0}};

  int opt;
  int option_index = 0;
  // Flags for future use (currently parsed but not implemented)
  __attribute__((unused)) bool debug_mode = false;
  __attribute__((unused)) bool no_color = false;

  // Collect -e arguments (can have multiple)
  char **execute_args = NULL;
  size_t execute_count = 0;
  size_t execute_capacity = 0;

  // Parse command-line options
  while ((opt = getopt_long(argc, argv, "hvdne:", long_options,
                            &option_index)) != -1) {
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
    case 'e':
      // Collect execute argument
      if (execute_count >= execute_capacity) {
        size_t new_capacity = execute_capacity == 0 ? 4 : execute_capacity * 2;
        char **new_args = realloc(execute_args, new_capacity * sizeof(char *));
        if (!new_args) {
          fprintf(stderr,
                  "Error: Failed to allocate memory for -e arguments\n");
          if (execute_args) {
            free(execute_args);
          }
          return 1;
        }
        execute_args = new_args;
        execute_capacity = new_capacity;
      }
      execute_args[execute_count++] = optarg;
      break;
    case '?':
      // Invalid option - getopt already printed error message
      if (execute_args) {
        free(execute_args);
      }
      return 1;
    default:
      if (execute_args) {
        free(execute_args);
      }
      return 1;
    }
  }

  // If -e flags were provided, execute them and exit
  if (execute_count > 0) {
    KronosVM *vm = kronos_vm_new();
    if (!vm) {
      print_error("Failed to create VM");
      if (execute_args) {
        free(execute_args);
      }
      return 1;
    }

    int exit_code = 0;
    for (size_t i = 0; i < execute_count; i++) {
      // Check for signal before processing each -e argument
      if (g_signal_received) {
        fprintf(stderr, "\nInterrupted. Cleaning up...\n");
        exit_code = 130; // Standard exit code for SIGINT
        break;
      }

      int result = kronos_run_string(vm, execute_args[i]);
      if (result < 0) {
        const char *err = kronos_get_last_error(vm);
        if (err && *err) {
          fprintf(stderr, "Error executing -e argument %zu: %s\n", i + 1, err);
        }
        exit_code = 1;
      }

      // Check for signal after processing each -e argument
      if (g_signal_received) {
        fprintf(stderr, "\nInterrupted. Cleaning up...\n");
        exit_code = 130; // Standard exit code for SIGINT
        break;
      }
    }

    kronos_vm_free(vm);
    if (execute_args) {
      free(execute_args);
    }
    return exit_code;
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
    if (result != 0) {
      // Handle any non-zero result (errors return negative, but handle positive
      // values defensively)
      const char *err = kronos_get_last_error(vm);
      if (err && *err) {
        print_error_with_file(argv[i], err);
      }
      // Convert any non-zero result to standard exit code 1
      // (kronos_run_file should only return 0 or negative, but be defensive)
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
  if (execute_args) {
    free(execute_args);
  }
  return exit_code;
}
