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

// Create new VM instance
KronosVM *kronos_vm_new(void) {
  runtime_init();
  KronosVM *vm = vm_new();
  if (!vm) {
    runtime_cleanup();
    return NULL;
  }
  return vm;
}

// Free VM instance
void kronos_vm_free(KronosVM *vm) {
  if (!vm)
    return;

  vm_free(vm);
  runtime_cleanup();
}

const char *kronos_get_last_error(KronosVM *vm) {
  if (!vm)
    return NULL;
  return vm->last_error_message;
}

KronosErrorCode kronos_get_last_error_code(KronosVM *vm) {
  if (!vm)
    return KRONOS_ERR_INVALID_ARGUMENT;
  return vm->last_error_code;
}

void kronos_set_error_callback(KronosVM *vm, KronosErrorCallback callback) {
  if (!vm)
    return;
  vm->error_callback = callback;
}

// Run a Kronos source string
int kronos_run_string(KronosVM *vm, const char *source) {
  if (!vm || !source)
    return vm_error(vm, KRONOS_ERR_INVALID_ARGUMENT,
                    "VM and source must be non-null");

  vm_clear_error(vm);

  // Tokenize
  TokenArray *tokens = tokenize(source, NULL);
  if (!tokens) {
    return vm_error(vm, KRONOS_ERR_TOKENIZE, "Tokenization failed");
  }

  // Parse
  AST *ast = parse(tokens);
  token_array_free(tokens);

  if (!ast) {
    return vm_error(vm, KRONOS_ERR_PARSE, "Parsing failed");
  }

  // Compile
  const char *compile_err = NULL;
  Bytecode *bytecode = compile(ast, &compile_err);
  ast_free(ast);

  if (!bytecode) {
    return vm_errorf(vm, KRONOS_ERR_COMPILE, "Compilation failed%s%s",
                     compile_err ? ": " : "", compile_err ? compile_err : "");
  }

  // Execute
  int result = vm_execute(vm, bytecode);
  bytecode_free(bytecode);

  if (result < 0 && vm->last_error_code == KRONOS_OK) {
    vm_set_error(vm, KRONOS_ERR_RUNTIME, "Runtime execution failed");
  }

  return result;
}

// Run a Kronos file
int kronos_run_file(KronosVM *vm, const char *filepath) {
  if (!vm || !filepath)
    return vm_error(vm, KRONOS_ERR_INVALID_ARGUMENT,
                    "VM and filepath must be non-null");

  vm_clear_error(vm);

  // Read file
  FILE *file = fopen(filepath, "r");
  if (!file) {
    return vm_errorf(vm, KRONOS_ERR_NOT_FOUND, "Failed to open file: %s",
                     filepath);
  }

  // Get file size
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

  // Check for unreasonably large files (protect against overflow)
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

  // Read contents - safe to cast after size validation
  size_t length = (size_t)size;
  char *source = malloc(length + 1);
  if (!source) {
    int err = vm_error(vm, KRONOS_ERR_INTERNAL,
                       "Failed to allocate memory for file contents");
    fclose(file);
    return err;
  }

  size_t read_size = fread(source, 1, length, file);

  // Check for read errors
  if (ferror(file)) {
    int err = vm_errorf(vm, KRONOS_ERR_IO, "Failed to read file: %s", filepath);
    free(source);
    fclose(file);
    return err;
  }

  // Check for unexpected partial read (not EOF)
  if (read_size < length && !feof(file)) {
    int err =
        vm_errorf(vm, KRONOS_ERR_IO, "Incomplete read from file: %s", filepath);
    free(source);
    fclose(file);
    return err;
  }

  // Safe to write NUL terminator (buffer is length+1, read_size <= length)
  source[read_size] = '\0';
  fclose(file);

  // Execute
  int result = kronos_run_string(vm, source);
  free(source);

  return result;
}

// REPL mode
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

    // Detect truncated input (no newline and not EOF)
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] != '\n' && !feof(stdin)) {
      fprintf(stderr, "Warning: Input line truncated (max %zu chars)\n",
              sizeof(line) - 1);
      int c;
      while ((c = getchar()) != '\n' && c != EOF)
        ;
    }

    // Remove newline
    if (len > 0 && line[len - 1] == '\n') {
      line[len - 1] = '\0';
    }

    // Check for exit
    if (strcmp(line, "exit") == 0) {
      break;
    }

    // Skip empty lines
    if (strlen(line) == 0) {
      continue;
    }

    // Execute line
    if (kronos_run_string(vm, line) < 0) {
      const char *err = kronos_get_last_error(vm);
      if (err && *err) {
        fprintf(stderr, "Error: %s\n", err);
      }
    }
  }

  kronos_vm_free(vm);
}

int main(int argc, char **argv) {
  if (argc > 1) {
    // Run file
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

    // Normalize exit code: any negative error becomes 1
    return (result < 0) ? 1 : result;
  } else {
    // Run REPL
    kronos_repl();
    return 0;
  }
}
