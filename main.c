#include "include/kronos.h"
#include "src/compiler/compiler.h"
#include "src/core/runtime.h"
#include "src/frontend/parser.h"
#include "src/frontend/tokenizer.h"
#include "src/vm/vm.h"
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

// Run a Kronos source string
int kronos_run_string(KronosVM *vm, const char *source) {
  if (!vm || !source)
    return -1;

  // Tokenize
  TokenArray *tokens = tokenize(source);
  if (!tokens) {
    fprintf(stderr, "Tokenization failed\n");
    return -1;
  }

  // Parse
  AST *ast = parse(tokens);
  token_array_free(tokens);

  if (!ast) {
    fprintf(stderr, "Parsing failed\n");
    return -1;
  }

  // Compile
  Bytecode *bytecode = compile(ast);
  ast_free(ast);

  if (!bytecode) {
    fprintf(stderr, "Compilation failed\n");
    return -1;
  }

  // Execute
  int result = vm_execute(vm, bytecode);
  bytecode_free(bytecode);

  return result;
}

// Run a Kronos file
int kronos_run_file(KronosVM *vm, const char *filepath) {
  if (!vm || !filepath)
    return -1;

  // Read file
  FILE *file = fopen(filepath, "r");
  if (!file) {
    fprintf(stderr, "Failed to open file: %s\n", filepath);
    return -1;
  }

  // Get file size
  if (fseek(file, 0, SEEK_END) != 0) {
    fprintf(stderr, "Error: Failed to seek to end of file: %s\n", filepath);
    fclose(file);
    return -1;
  }

  long size = ftell(file);
  if (size < 0) {
    fprintf(stderr, "Error: Failed to determine file size: %s\n", filepath);
    fclose(file);
    return -1;
  }

  // Check for unreasonably large files (protect against overflow)
  if ((uintmax_t)size > (uintmax_t)(SIZE_MAX - 1)) {
    fprintf(stderr, "Error: File too large to read: %s\n", filepath);
    fclose(file);
    return -1;
  }

  if (fseek(file, 0, SEEK_SET) != 0) {
    fprintf(stderr, "Error: Failed to seek to start of file: %s\n", filepath);
    fclose(file);
    return -1;
  }

  // Read contents - safe to cast after size validation
  size_t length = (size_t)size;
  char *source = malloc(length + 1);
  if (!source) {
    fprintf(stderr, "Error: Failed to allocate memory for file contents\n");
    fclose(file);
    return -1;
  }

  size_t read_size = fread(source, 1, length, file);

  // Check for read errors
  if (ferror(file)) {
    fprintf(stderr, "Error: Failed to read file: %s\n", filepath);
    free(source);
    fclose(file);
    return -1;
  }

  // Check for unexpected partial read (not EOF)
  if (read_size < length && !feof(file)) {
    fprintf(stderr, "Error: Incomplete read from file: %s\n", filepath);
    free(source);
    fclose(file);
    return -1;
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
    kronos_run_string(vm, line);
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
    kronos_vm_free(vm);

    // Normalize exit code: -1 becomes 1, 0 stays 0
    return (result == -1) ? 1 : result;
  } else {
    // Run REPL
    kronos_repl();
    return 0;
  }
}
