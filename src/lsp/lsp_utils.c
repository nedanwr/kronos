/**
 * @file lsp_utils.c
 * @brief Helper functions for LSP server
 */

#include "../frontend/tokenizer.h"
#include "lsp.h"
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern DocumentState *g_doc;

// Helper structure for counting references
typedef struct {
  const char *symbol_name;
  size_t count;
} ReferenceCountContext;

/**
 * @brief Safely parse an unsigned long from a string
 *
 * Validates input and checks for overflow/underflow errors.
 *
 * @param str String to parse
 * @param out_value Output parameter for parsed value
 * @return true if parsing succeeded, false on error
 */
bool safe_strtoul(const char *str, size_t *out_value) {
  if (!str || !out_value) {
    return false;
  }

  // Skip leading whitespace
  while (*str == ' ' || *str == '\t') {
    str++;
  }

  // Empty string or only whitespace
  if (*str == '\0') {
    return false;
  }

  char *endptr;
  errno = 0;
  unsigned long val = strtoul(str, &endptr, 10);

  // Check for conversion errors
  if (errno == ERANGE) {
    // Value out of range
    return false;
  }

  // Check if entire string was consumed (allow trailing whitespace)
  while (*endptr == ' ' || *endptr == '\t' || *endptr == '\r' ||
         *endptr == '\n') {
    endptr++;
  }
  if (*endptr != '\0') {
    // Invalid characters in input
    return false;
  }

  // Check if no conversion was performed
  if (endptr == str) {
    return false;
  }

  // Check if value fits in size_t
  if (val > SIZE_MAX) {
    return false;
  }

  *out_value = (size_t)val;
  return true;
}

void free_symbols(Symbol *sym) {
  while (sym) {
    Symbol *next = sym->next;
    free(sym->name);
    free(sym->type_name);
    free(sym);
    sym = next;
  }
}

void get_node_position(ASTNode *node, size_t *line, size_t *col) {
  // LIMITATION: This function uses approximate position estimates because
  // the AST doesn't store exact source positions. The parser would need to
  // be modified to track line/column information for each AST node.
  // Current implementation uses indent as a crude estimate, which is
  // inaccurate.
  // TODO: Enhance parser to track source positions (line/column) for each node.
  *line = 1;
  *col = 1;
  if (node && node->indent >= 0) {
    // Use indent as a rough estimate (inaccurate - see limitation above)
    *line = (size_t)(node->indent / 4) + 1;
    *col = (size_t)(node->indent % 4) + 1;
  }
}

void free_imported_modules(ImportedModule *modules) {
  while (modules) {
    ImportedModule *next = modules->next;
    free(modules->name);
    free(modules->file_path);
    free_symbols(modules->exports);
    free(modules);
    modules = next;
  }
}

bool is_module_imported(const char *module_name) {
  if (!g_doc || !module_name)
    return false;

  // Check built-in modules
  if (strcmp(module_name, "math") == 0)
    return true;

  // Check imported modules
  ImportedModule *mod = g_doc->imported_modules;
  while (mod) {
    if (mod->name && strcmp(mod->name, module_name) == 0)
      return true;
    mod = mod->next;
  }

  return false;
}

Symbol *load_module_exports(const char *file_path) {
  if (!file_path)
    return NULL;

  // Read file
  FILE *file = fopen(file_path, "r");
  if (!file)
    return NULL;

  // Determine file size
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }

  long size = ftell(file);
  if (size < 0 || (uintmax_t)size > (uintmax_t)(SIZE_MAX - 1)) {
    fclose(file);
    return NULL;
  }

  if (fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return NULL;
  }

  // Allocate buffer
  size_t length = (size_t)size;
  char *source = malloc(length + 1);
  if (!source) {
    fclose(file);
    return NULL;
  }

  size_t read_size = fread(source, 1, length, file);
  if (ferror(file) || (read_size < length && !feof(file))) {
    free(source);
    fclose(file);
    return NULL;
  }

  source[read_size] = '\0';
  fclose(file);

  // Tokenize and parse
  TokenArray *tokens = tokenize(source, NULL);
  free(source);

  if (!tokens)
    return NULL;

  AST *ast = parse(tokens, NULL);
  token_array_free(tokens);

  if (!ast || ast->count == 0) {
    if (ast)
      ast_free(ast);
    return NULL;
  }

  // Extract top-level symbols (functions and variables)
  // Re-read source for position tracking (we need it for accurate positions)
  file = fopen(file_path, "r");
  char *source_for_pos = NULL;
  if (file) {
    if (fseek(file, 0, SEEK_END) == 0) {
      long size = ftell(file);
      if (size >= 0 && (uintmax_t)size <= (uintmax_t)(SIZE_MAX - 1)) {
        if (fseek(file, 0, SEEK_SET) == 0) {
          size_t len = (size_t)size;
          source_for_pos = malloc(len + 1);
          if (source_for_pos) {
            size_t read_size = fread(source_for_pos, 1, len, file);
            if (!ferror(file) && (read_size == len || feof(file))) {
              source_for_pos[read_size] = '\0';
            } else {
              free(source_for_pos);
              source_for_pos = NULL;
            }
          }
        }
      }
    }
    fclose(file);
  }

  Symbol *exports = NULL;
  Symbol **tail = &exports;

  for (size_t i = 0; i < ast->count; i++) {
    ASTNode *node = ast->statements[i];
    if (!node)
      continue;

    // Extract function definitions
    if (node->type == AST_FUNCTION && node->as.function.name) {
      Symbol *sym = malloc(sizeof(Symbol));
      if (sym) {
        sym->name = strdup(node->as.function.name);
        sym->type = SYMBOL_FUNCTION;
        // Try to find actual position in source
        if (source_for_pos) {
          char pattern[LSP_PATTERN_BUFFER_SIZE];
          int n = snprintf(pattern, sizeof(pattern), "function %s", sym->name);
          if (n >= 0 && (size_t)n < sizeof(pattern)) {
            find_node_position(node, source_for_pos, pattern, &sym->line,
                               &sym->column);
            if (sym->line == 1 && sym->column == 0) {
              // Fallback to approximate
              get_node_position(node, &sym->line, &sym->column);
            }
          } else {
            // Pattern too long, use approximate position
            get_node_position(node, &sym->line, &sym->column);
          }
        } else {
          get_node_position(node, &sym->line, &sym->column);
        }
        sym->type_name = NULL;
        sym->is_mutable = false;
        sym->param_count = node->as.function.param_count;
        sym->written = false;
        sym->read = false;
        sym->next = NULL;
        *tail = sym;
        tail = &sym->next;
      }
    }
    // Extract variable declarations (top-level only)
    else if (node->type == AST_ASSIGN && node->as.assign.name) {
      Symbol *sym = malloc(sizeof(Symbol));
      if (sym) {
        sym->name = strdup(node->as.assign.name);
        sym->type = SYMBOL_VARIABLE;
        // Try to find actual position in source
        if (source_for_pos) {
          char pattern[LSP_PATTERN_BUFFER_SIZE];
          int n = snprintf(pattern, sizeof(pattern), "let %s to", sym->name);
          if (n >= 0 && (size_t)n < sizeof(pattern)) {
            find_node_position(node, source_for_pos, pattern, &sym->line,
                               &sym->column);
            if (sym->line == 1 && sym->column == 0) {
              n = snprintf(pattern, sizeof(pattern), "set %s to", sym->name);
              if (n >= 0 && (size_t)n < sizeof(pattern)) {
                find_node_position(node, source_for_pos, pattern, &sym->line,
                                   &sym->column);
              }
            }
            if (sym->line == 1 && sym->column == 0) {
              // Fallback to approximate
              get_node_position(node, &sym->line, &sym->column);
            }
          } else {
            // Pattern too long, use approximate position
            get_node_position(node, &sym->line, &sym->column);
          }
        } else {
          get_node_position(node, &sym->line, &sym->column);
        }
        sym->type_name = node->as.assign.type_name
                             ? strdup(node->as.assign.type_name)
                             : NULL;
        sym->is_mutable = node->as.assign.is_mutable;
        sym->param_count = 0;
        sym->written = false;
        sym->read = false;
        sym->next = NULL;
        *tail = sym;
        tail = &sym->next;
      }
    }
  }

  free(source_for_pos);

  ast_free(ast);
  return exports;
}

char *get_module_hover_info(ImportedModule *mod) {
  if (!mod)
    return NULL;

  // Load exports if not already loaded
  if (!mod->exports && mod->file_path) {
    mod->exports = load_module_exports(mod->file_path);
  }

  // Build hover text
  const size_t buffer_size = 4096;
  char *hover_text = malloc(buffer_size);
  if (!hover_text)
    return NULL;

  size_t pos = 0;
  int n;

  // Write module name
  if (pos < buffer_size) {
    size_t available = buffer_size - pos;
    n = snprintf(hover_text + pos, available, "**module** `%s`\n\n", mod->name);
    if (n < 0 || (size_t)n >= available) {
      // Buffer full or error - truncate at current position
      hover_text[buffer_size - 1] = '\0';
      return hover_text;
    }
    pos += (size_t)n;
  }

  if (mod->file_path) {
    if (pos < buffer_size) {
      size_t available = buffer_size - pos;
      n = snprintf(hover_text + pos, available, "**Path:** `%s`\n\n",
                   mod->file_path);
      if (n < 0 || (size_t)n >= available) {
        hover_text[buffer_size - 1] = '\0';
        return hover_text;
      }
      pos += (size_t)n;
    }
  } else {
    if (pos < buffer_size) {
      size_t available = buffer_size - pos;
      n = snprintf(hover_text + pos, available,
                   "**Type:** Built-in module\n\n");
      if (n < 0 || (size_t)n >= available) {
        hover_text[buffer_size - 1] = '\0';
        return hover_text;
      }
      pos += (size_t)n;
    }
  }

  if (mod->exports) {
    if (pos < buffer_size) {
      size_t available = buffer_size - pos;
      n = snprintf(hover_text + pos, available, "**Exports:**\n\n");
      if (n < 0 || (size_t)n >= available) {
        hover_text[buffer_size - 1] = '\0';
        return hover_text;
      }
      pos += (size_t)n;
    }
    Symbol *sym = mod->exports;
    int func_count = 0;
    int var_count = 0;
    while (sym) {
      if (sym->type == SYMBOL_FUNCTION) {
        func_count++;
        if (pos < buffer_size) {
          size_t available = buffer_size - pos;
          n = snprintf(hover_text + pos, available, "• `%s` (function",
                       sym->name);
          if (n < 0 || (size_t)n >= available) {
            hover_text[buffer_size - 1] = '\0';
            return hover_text;
          }
          pos += (size_t)n;
        }
        if (sym->param_count > 0) {
          if (pos < buffer_size) {
            size_t available = buffer_size - pos;
            n = snprintf(hover_text + pos, available, ", %zu parameter%s",
                         sym->param_count, sym->param_count == 1 ? "" : "s");
            if (n < 0 || (size_t)n >= available) {
              hover_text[buffer_size - 1] = '\0';
              return hover_text;
            }
            pos += (size_t)n;
          }
        } else {
          if (pos < buffer_size) {
            size_t available = buffer_size - pos;
            n = snprintf(hover_text + pos, available, ", no parameters");
            if (n < 0 || (size_t)n >= available) {
              hover_text[buffer_size - 1] = '\0';
              return hover_text;
            }
            pos += (size_t)n;
          }
        }
        if (pos < buffer_size) {
          size_t available = buffer_size - pos;
          n = snprintf(hover_text + pos, available, ")\n");
          if (n < 0 || (size_t)n >= available) {
            hover_text[buffer_size - 1] = '\0';
            return hover_text;
          }
          pos += (size_t)n;
        }
      } else if (sym->type == SYMBOL_VARIABLE) {
        var_count++;
        if (pos < buffer_size) {
          size_t available = buffer_size - pos;
          n = snprintf(hover_text + pos, available, "• `%s` (%s variable",
                       sym->name, sym->is_mutable ? "mutable" : "immutable");
          if (n < 0 || (size_t)n >= available) {
            hover_text[buffer_size - 1] = '\0';
            return hover_text;
          }
          pos += (size_t)n;
        }
        if (sym->type_name) {
          if (pos < buffer_size) {
            size_t available = buffer_size - pos;
            n = snprintf(hover_text + pos, available, ", type: `%s`",
                         sym->type_name);
            if (n < 0 || (size_t)n >= available) {
              hover_text[buffer_size - 1] = '\0';
              return hover_text;
            }
            pos += (size_t)n;
          }
        }
        if (pos < buffer_size) {
          size_t available = buffer_size - pos;
          n = snprintf(hover_text + pos, available, ")\n");
          if (n < 0 || (size_t)n >= available) {
            hover_text[buffer_size - 1] = '\0';
            return hover_text;
          }
          pos += (size_t)n;
        }
      }
      sym = sym->next;
    }
    if (func_count == 0 && var_count == 0) {
      if (pos < buffer_size) {
        size_t available = buffer_size - pos;
        n = snprintf(hover_text + pos, available, "No exports found\n");
        if (n < 0 || (size_t)n >= available) {
          hover_text[buffer_size - 1] = '\0';
          return hover_text;
        }
        pos += (size_t)n;
      }
    }
  } else if (mod->file_path) {
    if (pos < buffer_size) {
      size_t available = buffer_size - pos;
      n = snprintf(hover_text + pos, available,
                   "**Exports:** Unable to load\n");
      if (n < 0 || (size_t)n >= available) {
        hover_text[buffer_size - 1] = '\0';
        return hover_text;
      }
      pos += (size_t)n;
    }
  }

  return hover_text;
}

void free_document_state(DocumentState *doc) {
  if (!doc)
    return;
  free(doc->uri);
  free(doc->text);
  free_symbols(doc->symbols);
  free_imported_modules(doc->imported_modules);
  if (doc->ast)
    ast_free(doc->ast);
  free(doc);
}

void process_statements_for_symbols(ASTNode **statements, size_t count,
                                    Symbol ***tail, Symbol **head) {
  if (!statements || !tail)
    return;

  for (size_t i = 0; i < count; i++) {
    ASTNode *node = statements[i];
    if (!node)
      continue;

    Symbol *sym = NULL;
    size_t line = 1, col = 1;

    switch (node->type) {
    case AST_ASSIGN: {
      // Check if symbol already exists (for reassignments)
      Symbol *existing = head ? *head : NULL;
      while (existing) {
        if (existing->name &&
            strcmp(existing->name, node->as.assign.name) == 0 &&
            existing->type == SYMBOL_VARIABLE) {
          // Update existing symbol: mark as written
          existing->written = true;
          break;
        }
        existing = existing->next;
      }

      // Only create new symbol if it doesn't exist
      if (!existing) {
        sym = malloc(sizeof(Symbol));
        if (!sym)
          continue;
        sym->name = strdup(node->as.assign.name);
        sym->type = SYMBOL_VARIABLE;
        sym->is_mutable = node->as.assign.is_mutable;

        // Only use explicit type annotation - don't infer types from values
        // Variables initialized with null or no value can be reassigned to any
        // type
        if (node->as.assign.type_name) {
          sym->type_name = strdup(node->as.assign.type_name);
        } else {
          // No type constraint - variable can be assigned any type
          sym->type_name = NULL;
        }

        sym->param_count = 0;
        sym->written = true; // Initial assignment counts as a write
        sym->read = false;
        get_node_position(node, &line, &col);
        sym->line = line;
        sym->column = col;
        sym->next = NULL;
        **tail = sym;
        *tail = &sym->next;
      }
      break;
    }
    case AST_FUNCTION: {
      sym = malloc(sizeof(Symbol));
      if (!sym)
        continue;
      sym->name = strdup(node->as.function.name);
      sym->type = SYMBOL_FUNCTION;
      sym->is_mutable = false;
      sym->type_name = NULL;
      sym->param_count = node->as.function.param_count;
      sym->written = false;
      sym->read = false;
      get_node_position(node, &line, &col);
      sym->line = line;
      sym->column = col;
      sym->next = NULL;
      **tail = sym;
      *tail = &sym->next;

      // Add parameters as symbols
      for (size_t j = 0; j < node->as.function.param_count; j++) {
        Symbol *param = malloc(sizeof(Symbol));
        if (!param)
          continue;
        param->name = strdup(node->as.function.params[j]);
        param->type = SYMBOL_PARAMETER;
        param->is_mutable = false;
        param->type_name = NULL;
        param->param_count = 0;
        param->written = false; // Parameters are passed in, not written
        param->read = false;
        param->line = line;
        param->column = col;
        param->next = NULL;
        **tail = param;
        *tail = &param->next;
      }

      // Recursively process function body to add local variables
      if (node->as.function.block && node->as.function.block_size > 0) {
        process_statements_for_symbols(
            node->as.function.block, node->as.function.block_size, tail, head);
      }
      break;
    }
    case AST_FOR: {
      // Add loop variable to symbol table
      if (node->as.for_stmt.var) {
        sym = malloc(sizeof(Symbol));
        if (!sym)
          break;
        sym->name = strdup(node->as.for_stmt.var);
        sym->type = SYMBOL_VARIABLE;
        sym->is_mutable =
            false; // Loop variables are immutable (assigned by loop)
        sym->type_name = NULL; // Type depends on what's being iterated
        sym->param_count = 0;
        sym->written = false; // Loop variables are assigned by the loop
        sym->read = false;
        get_node_position(node, &line, &col);
        sym->line = line;
        sym->column = col;
        sym->next = NULL;
        **tail = sym;
        *tail = &sym->next;
      }
      break;
    }
    case AST_TRY: {
      // Add catch variables to symbol table
      for (size_t j = 0; j < node->as.try_stmt.catch_block_count; j++) {
        if (node->as.try_stmt.catch_blocks[j].catch_var) {
          sym = malloc(sizeof(Symbol));
          if (!sym)
            break;
          sym->name = strdup(node->as.try_stmt.catch_blocks[j].catch_var);
          sym->type = SYMBOL_VARIABLE;
          sym->is_mutable = false;           // Catch variables are immutable
          sym->type_name = strdup("string"); // Error messages are strings
          sym->param_count = 0;
          sym->written =
              false; // Catch variables are assigned by exception handler
          sym->read = false;
          get_node_position(node, &line, &col);
          sym->line = line;
          sym->column = col;
          sym->next = NULL;
          **tail = sym;
          *tail = &sym->next;
        }
      }
      break;
    }
    default:
      break;
    }
  }
}

void build_symbol_table(DocumentState *doc, AST *ast, const char *text) {
  if (!doc || !ast)
    return;

  // Clear existing symbols
  free_symbols(doc->symbols);
  doc->symbols = NULL;
  Symbol **tail = &doc->symbols;

  // Clear existing imported modules
  free_imported_modules(doc->imported_modules);
  doc->imported_modules = NULL;
  ImportedModule **import_tail = &doc->imported_modules;

  // Calculate line starts for position lookup
  // Note: If allocation fails, we continue without line_starts - position
  // lookup will be less accurate but the function can still build the symbol
  // table
  size_t *line_starts = NULL;
  size_t line_count = 0;
  size_t capacity = 64;
  line_starts = malloc(capacity * sizeof(size_t));
  if (line_starts) {
    line_starts[0] = 0;
    line_count = 1;
    for (size_t i = 0; text[i] != '\0'; i++) {
      if (text[i] == '\n') {
        if (line_count >= capacity) {
          capacity *= 2;
          size_t *new_starts = realloc(line_starts, capacity * sizeof(size_t));
          if (!new_starts) {
            // Realloc failed - free existing buffer and continue without it
            // This is acceptable because line_starts is only used for position
            // lookup optimization, not critical for symbol table building
            free(line_starts);
            line_starts = NULL;
            break;
          }
          line_starts = new_starts;
        }
        line_starts[line_count++] = i + 1;
      }
    }
  }
  // If line_starts allocation failed, we continue - symbol table building
  // doesn't strictly require it (positions can be calculated on-demand)
  // This is acceptable error handling: the function can still succeed without
  // the optimization, just with less accurate position information

  // Process top-level statements to extract imports and symbols
  for (size_t i = 0; i < ast->count; i++) {
    ASTNode *node = ast->statements[i];
    if (!node)
      continue;

    // Track imported modules
    if (node->type == AST_IMPORT && node->as.import.module_name) {
      ImportedModule *mod = malloc(sizeof(ImportedModule));
      if (mod) {
        mod->name = strdup(node->as.import.module_name);
        mod->file_path = node->as.import.file_path
                             ? strdup(node->as.import.file_path)
                             : NULL;
        mod->exports = NULL; // Will be populated when needed
        mod->next = NULL;
        *import_tail = mod;
        import_tail = &mod->next;
      }
    }
  }

  // Process top-level statements (which will recursively process function
  // bodies)
  process_statements_for_symbols(ast->statements, ast->count, &tail,
                                 &doc->symbols);

  free(line_starts);
}

bool get_constant_number(ASTNode *node, double *value) {
  if (!node)
    return false;
  if (node->type == AST_NUMBER) {
    *value = node->as.number;
    return true;
  }
  return false;
}

void find_node_position(ASTNode *node, const char *text, const char *pattern,
                        size_t *line, size_t *col) {
  // node parameter kept for API consistency but not currently used
  // Could be used in future for more accurate position tracking
  (void)node;
  *line = 1;
  *col = 0;
  if (!text || !pattern)
    return;

  const char *pos = text;
  while ((pos = strstr(pos, pattern)) != NULL) {
    // Check if this is the actual occurrence (not part of a comment)
    const char *check_line_start = pos;
    while (check_line_start > text && *(check_line_start - 1) != '\n') {
      check_line_start--;
    }
    // Skip leading whitespace to find the first non-whitespace character
    const char *first_char = check_line_start;
    while (*first_char == ' ' || *first_char == '\t') {
      first_char++;
    }
    // If the line starts with '#', skip this match (it's in a comment)
    if (*first_char == '#') {
      pos += strlen(pattern);
      continue;
    }

    *line = 1;
    for (const char *p = text; p < pos; p++) {
      if (*p == '\n')
        (*line)++;
    }
    const char *line_start = pos;
    while (line_start > text && *(line_start - 1) != '\n') {
      line_start--;
    }
    *col = (size_t)(pos - line_start);
    return;
  }
}

bool find_assignment_value_position(const char *text, const char *varname,
                                    size_t occurrence, ASTNode *value_node,
                                    size_t *line, size_t *col, size_t *length) {
  *line = 1;
  *col = 0;
  *length = 0;
  if (!text || !varname || !value_node || occurrence == 0)
    return false;

  // First find the assignment statement
  size_t assign_line = 1, assign_col = 0;
  if (!find_nth_occurrence(text, varname, occurrence, &assign_line,
                           &assign_col))
    return false;

  // Find the line in the text
  const char *line_start = text;
  size_t current_line = 1;
  for (const char *p = text; *p != '\0' && current_line < assign_line; p++) {
    if (*p == '\n') {
      current_line++;
      if (current_line == assign_line) {
        line_start = p + 1;
        break;
      }
    }
  }

  // Find "to" keyword after the variable name
  const char *to_pos = line_start + assign_col;
  while (*to_pos != '\0' && *to_pos != '\n' && strncmp(to_pos, "to", 2) != 0) {
    to_pos++;
  }
  if (*to_pos == '\0' || *to_pos == '\n')
    return false;

  // Skip "to" and whitespace
  to_pos += 2;
  while (*to_pos == ' ' || *to_pos == '\t') {
    to_pos++;
  }

  // Now find the value based on its type
  const char *value_start = to_pos;
  const char *value_end = value_start;

  switch (value_node->type) {
  case AST_STRING: {
    // Find the string literal (including quotes)
    if (*value_start == '"' || *value_start == '\'') {
      char quote = *value_start;
      value_end = value_start + 1;
      while (*value_end != '\0' && *value_end != '\n') {
        if (*value_end == quote &&
            (value_end == value_start + 1 || *(value_end - 1) != '\\')) {
          value_end++;
          break;
        }
        value_end++;
      }
    }
    break;
  }
  case AST_NUMBER: {
    // Find the number (digits, decimal point, optional sign)
    value_end = value_start;
    if (*value_end == '-' || *value_end == '+')
      value_end++;
    while ((*value_end >= '0' && *value_end <= '9') || *value_end == '.') {
      value_end++;
    }
    break;
  }
  case AST_BOOL: {
    // Find "true" or "false"
    if (strncmp(value_start, "true", 4) == 0) {
      value_end = value_start + 4;
    } else if (strncmp(value_start, "false", 5) == 0) {
      value_end = value_start + 5;
    }
    break;
  }
  case AST_NULL: {
    // Find "null" or "undefined"
    if (strncmp(value_start, "null", 4) == 0) {
      value_end = value_start + 4;
    } else if (strncmp(value_start, "undefined", 9) == 0) {
      value_end = value_start + 9;
    }
    break;
  }
  case AST_VAR: {
    // Find the variable name
    const char *var_name = value_node->as.var_name;
    size_t var_len = strlen(var_name);
    if (strncmp(value_start, var_name, var_len) == 0) {
      value_end = value_start + var_len;
    }
    break;
  }
  default:
    // For other types, just use a reasonable default
    value_end = value_start;
    while (*value_end != '\0' && *value_end != '\n' && *value_end != ' ' &&
           *value_end != '\t') {
      value_end++;
    }
    break;
  }

  if (value_end <= value_start)
    return false;

  *line = assign_line;
  *col = (size_t)(value_start - line_start);
  *length = (size_t)(value_end - value_start);
  return true;
}

int get_builtin_arg_count(const char *func_name) {
  // Zero-argument functions
  if (strcmp(func_name, "rand") == 0) {
    return 0;
  }

  // One-argument functions
  if (strcmp(func_name, "sqrt") == 0 || strcmp(func_name, "abs") == 0 ||
      strcmp(func_name, "round") == 0 || strcmp(func_name, "floor") == 0 ||
      strcmp(func_name, "ceil") == 0 || strcmp(func_name, "len") == 0 ||
      strcmp(func_name, "uppercase") == 0 ||
      strcmp(func_name, "lowercase") == 0 || strcmp(func_name, "trim") == 0 ||
      strcmp(func_name, "to_string") == 0 ||
      strcmp(func_name, "to_number") == 0 ||
      strcmp(func_name, "to_bool") == 0 || strcmp(func_name, "reverse") == 0 ||
      strcmp(func_name, "sort") == 0 || strcmp(func_name, "read_file") == 0 ||
      strcmp(func_name, "read_lines") == 0 ||
      strcmp(func_name, "file_exists") == 0 ||
      strcmp(func_name, "list_files") == 0 ||
      strcmp(func_name, "dirname") == 0 || strcmp(func_name, "basename") == 0) {
    return 1;
  }

  // Two-argument functions
  if (strcmp(func_name, "add") == 0 || strcmp(func_name, "subtract") == 0 ||
      strcmp(func_name, "multiply") == 0 || strcmp(func_name, "divide") == 0 ||
      strcmp(func_name, "power") == 0 || strcmp(func_name, "split") == 0 ||
      strcmp(func_name, "contains") == 0 ||
      strcmp(func_name, "starts_with") == 0 ||
      strcmp(func_name, "ends_with") == 0 ||
      strcmp(func_name, "write_file") == 0 ||
      strcmp(func_name, "join_path") == 0 || strcmp(func_name, "match") == 0 ||
      strcmp(func_name, "search") == 0 || strcmp(func_name, "findall") == 0 ||
      strcmp(func_name, "regex.match") == 0 ||
      strcmp(func_name, "regex.search") == 0 ||
      strcmp(func_name, "regex.findall") == 0) {
    return 2;
  }

  // Three-argument functions
  if (strcmp(func_name, "replace") == 0) {
    return 3;
  }

  // Variable arguments (min, max, join)
  if (strcmp(func_name, "min") == 0 || strcmp(func_name, "max") == 0 ||
      strcmp(func_name, "join") == 0) {
    return -2; // -2 means variable arguments (at least 1)
  }

  return -1; // Not a built-in
}

void find_call_position(const char *text, const char *func_name, size_t *line,
                        size_t *col) {
  *line = 1;
  *col = 0;

  // Search for "call <func_name> with" pattern
  char pattern[256];
  int n = snprintf(pattern, sizeof(pattern), "call %s with", func_name);
  if (n < 0 || (size_t)n >= sizeof(pattern)) {
    // Pattern too long, cannot search
    return;
  }

  const char *pos = text;
  while ((pos = strstr(pos, pattern)) != NULL) {
    // Check if this is the actual call (not part of a comment or string)
    const char *before = pos;
    while (before > text && *(before - 1) != '\n') {
      before--;
    }
    // Skip leading whitespace
    while (before < pos && (*before == ' ' || *before == '\t')) {
      before++;
    }
    // Check if it's a comment
    if (before < pos && *before == '#') {
      pos += strlen(pattern);
      continue;
    }

    // Count lines up to this position
    *line = 1;
    for (const char *p = text; p < pos; p++) {
      if (*p == '\n')
        (*line)++;
    }
    // Count columns
    const char *line_start = pos;
    while (line_start > text && *(line_start - 1) != '\n') {
      line_start--;
    }
    *col = (size_t)(pos - line_start);
    return;
  }
}

bool find_call_argument_position(const char *text, const char *func_name,
                                 ASTNode *arg_node, size_t *line, size_t *col,
                                 size_t *length) {
  *line = 1;
  *col = 0;
  *length = 0;
  if (!text || !func_name || !arg_node)
    return false;

  // Find "call <func_name> with" pattern
  char pattern[256];
  int n = snprintf(pattern, sizeof(pattern), "call %s with", func_name);
  if (n < 0 || (size_t)n >= sizeof(pattern)) {
    // Pattern too long, cannot search
    return false;
  }
  const char *with_pos = strstr(text, pattern);
  if (!with_pos)
    return false;

  // Skip "with" and whitespace
  const char *arg_start = with_pos + strlen(pattern);
  while (*arg_start == ' ' || *arg_start == '\t') {
    arg_start++;
  }

  // Find the end of the argument based on its type
  const char *arg_end = arg_start;

  switch (arg_node->type) {
  case AST_STRING: {
    // Find the string literal (including quotes)
    if (*arg_end == '"' || *arg_end == '\'') {
      char quote = *arg_end;
      arg_end++;
      while (*arg_end != '\0' && *arg_end != '\n') {
        if (*arg_end == quote &&
            (arg_end == arg_start + 1 || *(arg_end - 1) != '\\')) {
          arg_end++;
          break;
        }
        arg_end++;
      }
    }
    break;
  }
  case AST_NUMBER: {
    // Find the number (digits, decimal point, optional sign)
    if (*arg_end == '-' || *arg_end == '+')
      arg_end++;
    while ((*arg_end >= '0' && *arg_end <= '9') || *arg_end == '.') {
      arg_end++;
    }
    break;
  }
  case AST_BOOL: {
    // Find "true" or "false"
    if (strncmp(arg_end, "true", 4) == 0) {
      arg_end += 4;
    } else if (strncmp(arg_end, "false", 5) == 0) {
      arg_end += 5;
    }
    break;
  }
  case AST_NULL: {
    // Find "null"
    if (strncmp(arg_end, "null", 4) == 0) {
      arg_end += 4;
    }
    break;
  }
  case AST_LIST: {
    // Find "list" keyword and the list elements
    if (strncmp(arg_end, "list", 4) == 0) {
      arg_end += 4;
      // Skip whitespace
      while (*arg_end == ' ' || *arg_end == '\t') {
        arg_end++;
      }
      // Find the end of the list (until newline or end of statement)
      while (*arg_end != '\0' && *arg_end != '\n') {
        arg_end++;
      }
    }
    break;
  }
  case AST_MAP: {
    // Find "map" keyword and the map entries
    if (strncmp(arg_end, "map", 3) == 0) {
      arg_end += 3;
      // Skip whitespace
      while (*arg_end == ' ' || *arg_end == '\t') {
        arg_end++;
      }
      // Find the end of the map (until newline or end of statement)
      while (*arg_end != '\0' && *arg_end != '\n') {
        arg_end++;
      }
    }
    break;
  }
  case AST_VAR: {
    // Find the variable name
    const char *var_name = arg_node->as.var_name;
    size_t var_len = strlen(var_name);
    if (strncmp(arg_end, var_name, var_len) == 0) {
      arg_end += var_len;
    }
    break;
  }
  default:
    // For other types, find until whitespace or newline
    while (*arg_end != '\0' && *arg_end != '\n' && *arg_end != ' ' &&
           *arg_end != '\t') {
      arg_end++;
    }
    break;
  }

  if (arg_end <= arg_start)
    return false;

  // Calculate line and column
  *line = 1;
  for (const char *p = text; p < arg_start; p++) {
    if (*p == '\n')
      (*line)++;
  }
  const char *line_start = arg_start;
  while (line_start > text && *(line_start - 1) != '\n') {
    line_start--;
  }
  *col = (size_t)(arg_start - line_start);
  *length = (size_t)(arg_end - arg_start);
  return true;
}

bool grow_diagnostics_buffer(char **diagnostics, size_t *capacity, size_t pos,
                             size_t needed) {
  if (pos + needed < *capacity) {
    return true; // Already enough space
  }

  // Grow buffer by at least 2x or to accommodate needed space
  size_t new_capacity = *capacity * 2;
  if (new_capacity < pos + needed + 1024) {
    new_capacity = pos + needed + 1024; // Add extra padding
  }

  // Limit maximum size to prevent excessive memory usage
  const size_t MAX_DIAGNOSTICS_SIZE = 1024 * 1024; // 1MB max
  if (new_capacity > MAX_DIAGNOSTICS_SIZE) {
    return false; // Would exceed maximum
  }

  char *new_buffer = realloc(*diagnostics, new_capacity);
  if (!new_buffer) {
    return false; // Allocation failed
  }

  *diagnostics = new_buffer;
  *capacity = new_capacity;
  return true;
}

bool find_nth_occurrence(const char *text, const char *varname, size_t n,
                         size_t *line, size_t *col) {
  *line = 1;
  *col = 0;
  if (!text || !varname || n == 0)
    return false;

  // Build patterns: both "let <varname> to" and "set <varname> to"
  char pattern_let[256];
  char pattern_set[256];
  int n_let = snprintf(pattern_let, sizeof(pattern_let), "let %s to", varname);
  int n_set = snprintf(pattern_set, sizeof(pattern_set), "set %s to", varname);

  // Check if patterns were truncated
  if (n_let < 0 || (size_t)n_let >= sizeof(pattern_let) || n_set < 0 ||
      (size_t)n_set >= sizeof(pattern_set)) {
    // Pattern too long, cannot search
    return false;
  }

  size_t pattern_let_len = strlen(pattern_let);
  size_t pattern_set_len = strlen(pattern_set);

  // Collect all matches with their positions
  typedef struct {
    const char *pos;
    size_t len;
  } Match;
  const size_t MAX_MATCHES = 256; // Prevent stack overflow
  Match matches[MAX_MATCHES];
  size_t match_count = 0;

  // Search for "let" pattern
  const char *pos = text;
  while ((pos = strstr(pos, pattern_let)) != NULL &&
         match_count < MAX_MATCHES) {
    matches[match_count].pos = pos;
    matches[match_count].len = pattern_let_len;
    match_count++;
    pos += pattern_let_len;
  }

  // Search for "set" pattern
  pos = text;
  while ((pos = strstr(pos, pattern_set)) != NULL &&
         match_count < MAX_MATCHES) {
    matches[match_count].pos = pos;
    matches[match_count].len = pattern_set_len;
    match_count++;
    pos += pattern_set_len;
  }

  // Sort matches by position in text
  for (size_t i = 0; i < match_count; i++) {
    for (size_t j = i + 1; j < match_count; j++) {
      if (matches[j].pos < matches[i].pos) {
        Match temp = matches[i];
        matches[i] = matches[j];
        matches[j] = temp;
      }
    }
  }

  // Now process matches in order
  size_t count = 0;
  for (size_t i = 0; i < match_count; i++) {
    pos = matches[i].pos;
    size_t pattern_len = matches[i].len;
    // Find the start of the line containing this match
    const char *line_start = pos;
    while (line_start > text && *(line_start - 1) != '\n') {
      line_start--;
    }

    // Find the first non-whitespace character on this line
    const char *first_char = line_start;
    while (*first_char == ' ' || *first_char == '\t') {
      first_char++;
    }

    // CRITICAL: Skip if this line is a comment (starts with #)
    if (*first_char == '#') {
      continue; // Skip comment lines - pos will be set from next match
    }

    // Skip if the pattern is inside a string (check for quotes before it on the
    // same line)
    bool in_string = false;
    char string_char = 0;
    for (const char *p = line_start; p < pos; p++) {
      if (!in_string && (*p == '"' || *p == '\'')) {
        in_string = true;
        string_char = *p;
      } else if (in_string && *p == string_char &&
                 (p == line_start || *(p - 1) != '\\')) {
        in_string = false;
      }
    }
    if (in_string) {
      continue; // Skip patterns inside strings - pos will be set from next
                // match
    }

    // Make sure the pattern starts at word boundary (after whitespace or start
    // of line) and is followed by whitespace or end of line
    bool valid_start =
        (pos == line_start) ||
        (pos > text &&
         (*(pos - 1) == ' ' || *(pos - 1) == '\t' || *(pos - 1) == '\n'));
    bool valid_end = (pos[pattern_len] == '\0') ||
                     (pos[pattern_len] == ' ' || pos[pattern_len] == '\t' ||
                      pos[pattern_len] == '\n');

    if (valid_start && valid_end) {
      count++;
      if (count == n) {
        *line = 1;
        for (const char *p = text; p < pos; p++) {
          if (*p == '\n')
            (*line)++;
        }
        *col = (size_t)(pos - line_start);
        return true;
      }
    }

    pos += pattern_len;
  }

  return false; // Not found
}

ASTNode *find_variable_assignment(AST *ast, const char *var_name) {
  if (!ast || !ast->statements)
    return NULL;

  for (size_t i = 0; i < ast->count; i++) {
    ASTNode *node = ast->statements[i];
    if (node && node->type == AST_ASSIGN) {
      if (strcmp(node->as.assign.name, var_name) == 0) {
        return node;
      }
    }
  }
  return NULL;
}

bool is_loop_variable(Symbol *sym, AST *ast) {
  if (!sym || !ast || sym->type != SYMBOL_VARIABLE)
    return false;

  // Check if this variable is declared in a FOR statement
  for (size_t i = 0; i < ast->count; i++) {
    ASTNode *node = ast->statements[i];
    if (!node || node->type != AST_FOR)
      continue;

    if (node->as.for_stmt.var &&
        strcmp(node->as.for_stmt.var, sym->name) == 0) {
      return true;
    }

    // Check nested FOR statements in the loop body
    if (node->as.for_stmt.block) {
      AST temp_ast = {node->as.for_stmt.block, node->as.for_stmt.block_size,
                      node->as.for_stmt.block_size};
      if (is_loop_variable(sym, &temp_ast))
        return true;
    }
  }

  return false;
}

Symbol *find_symbol(const char *const name) {
  if (!g_doc || !name)
    return NULL;
  Symbol *sym = g_doc->symbols;
  while (sym) {
    if (sym->name && strcmp(sym->name, name) == 0)
      return sym;
    sym = sym->next;
  }
  return NULL;
}

char *get_word_at_position(const char *source, size_t line, size_t character) {
  if (!source)
    return NULL;

  // Find the line
  size_t current_line = 0;
  const char *line_start = source;
  const char *pos = source;

  while (current_line < line && *pos != '\0') {
    if (*pos == '\n') {
      current_line++;
      if (current_line == line) {
        line_start = pos + 1;
        break;
      }
    }
    pos++;
  }

  if (current_line != line)
    return NULL;

  // Find character position on the line
  pos = line_start;
  size_t col = 0;
  while (col < character && *pos != '\0' && *pos != '\n') {
    col++;
    pos++;
  }

  // Find word boundaries
  const char *word_start = pos;
  const char *word_end = pos;

  // Move back to start of word
  while (word_start > line_start &&
         (isalnum((unsigned char)*(word_start - 1)) ||
          *(word_start - 1) == '_' || *(word_start - 1) == '.')) {
    word_start--;
  }

  // Move forward to end of word
  while (*word_end != '\0' && *word_end != '\n' &&
         (isalnum((unsigned char)*word_end) || *word_end == '_' ||
          *word_end == '.')) {
    word_end++;
  }

  if (word_end <= word_start)
    return NULL;

  size_t len = (size_t)(word_end - word_start);
  char *word = malloc(len + 1);
  if (!word)
    return NULL;

  memcpy(word, word_start, len);
  word[len] = '\0';
  return word;
}

const char *get_module_description(const char *module_name) {
  if (strcmp(module_name, "math") == 0) {
    return "Mathematical functions module\n\n"
           "Provides mathematical operations and utilities:\n\n"
           "• `sqrt(number)` - Square root  \n"
           "• `power(base, exponent)` - Exponentiation  \n"
           "• `abs(number)` - Absolute value  \n"
           "• `round(number)` - Round to nearest integer  \n"
           "• `floor(number)` - Round down  \n"
           "• `ceil(number)` - Round up  \n"
           "• `rand()` - Random number between 0.0 and 1.0  \n"
           "• `min(...)` - Minimum of numbers  \n"
           "• `max(...)` - Maximum of numbers  \n\n"
           "**Usage:** `import math` then `call math.sqrt with 16`";
  }
  if (strcmp(module_name, "regex") == 0) {
    return "Regular expressions module\n\n"
           "Provides pattern matching using POSIX extended regular "
           "expressions:\n\n"
           "• `match(string, pattern)` - Returns true if pattern matches "
           "entire string  \n"
           "• `search(string, pattern)` - Returns first matched substring or "
           "null  \n"
           "• `findall(string, pattern)` - Returns list of all matched "
           "substrings  \n\n"
           "**Usage:** `import regex` then `call regex.match with \"hello\", "
           "\"h.*o\"`";
  }
  return NULL;
}

// Internal recursive version with depth tracking
static void count_references_in_node_recursive(ASTNode *node, void *ctx_ptr,
                                               int depth) {
  // Prevent stack overflow from deeply nested AST structures
  if (depth > MAX_AST_DEPTH) {
    return;
  }

  ReferenceCountContext *ctx = (ReferenceCountContext *)ctx_ptr;
  if (!node)
    return;

  switch (node->type) {
  case AST_ASSIGN:
    if (node->as.assign.name &&
        strcmp(node->as.assign.name, ctx->symbol_name) == 0) {
      ctx->count++; // Definition counts as a reference
    }
    if (node->as.assign.value) {
      count_references_in_node_recursive(node->as.assign.value, ctx, depth + 1);
    }
    break;

  case AST_VAR:
    if (node->as.var_name && strcmp(node->as.var_name, ctx->symbol_name) == 0) {
      ctx->count++;
    }
    break;

  case AST_CALL:
    if (node->as.call.name &&
        strcmp(node->as.call.name, ctx->symbol_name) == 0) {
      ctx->count++;
    }
    for (size_t i = 0; i < node->as.call.arg_count; i++) {
      if (node->as.call.args[i]) {
        count_references_in_node_recursive(node->as.call.args[i], ctx,
                                           depth + 1);
      }
    }
    break;

  case AST_FUNCTION:
    if (node->as.function.name &&
        strcmp(node->as.function.name, ctx->symbol_name) == 0) {
      ctx->count++; // Definition counts as a reference
    }
    for (size_t i = 0; i < node->as.function.block_size; i++) {
      if (node->as.function.block[i]) {
        count_references_in_node_recursive(node->as.function.block[i], ctx,
                                           depth + 1);
      }
    }
    break;

  case AST_BINOP:
    if (node->as.binop.left) {
      count_references_in_node_recursive(node->as.binop.left, ctx, depth + 1);
    }
    if (node->as.binop.right) {
      count_references_in_node_recursive(node->as.binop.right, ctx, depth + 1);
    }
    break;

  case AST_IF:
    if (node->as.if_stmt.condition) {
      count_references_in_node_recursive(node->as.if_stmt.condition, ctx,
                                         depth + 1);
    }
    for (size_t i = 0; i < node->as.if_stmt.block_size; i++) {
      if (node->as.if_stmt.block[i]) {
        count_references_in_node_recursive(node->as.if_stmt.block[i], ctx,
                                           depth + 1);
      }
    }
    if (node->as.if_stmt.else_block) {
      for (size_t i = 0; i < node->as.if_stmt.else_block_size; i++) {
        if (node->as.if_stmt.else_block[i]) {
          count_references_in_node_recursive(node->as.if_stmt.else_block[i],
                                             ctx, depth + 1);
        }
      }
    }
    break;

  case AST_FOR:
    if (node->as.for_stmt.var &&
        strcmp(node->as.for_stmt.var, ctx->symbol_name) == 0) {
      ctx->count++;
    }
    if (node->as.for_stmt.iterable) {
      count_references_in_node_recursive(node->as.for_stmt.iterable, ctx,
                                         depth + 1);
    }
    for (size_t i = 0; i < node->as.for_stmt.block_size; i++) {
      if (node->as.for_stmt.block[i]) {
        count_references_in_node_recursive(node->as.for_stmt.block[i], ctx,
                                           depth + 1);
      }
    }
    break;

  case AST_WHILE:
    if (node->as.while_stmt.condition) {
      count_references_in_node_recursive(node->as.while_stmt.condition, ctx,
                                         depth + 1);
    }
    for (size_t i = 0; i < node->as.while_stmt.block_size; i++) {
      if (node->as.while_stmt.block[i]) {
        count_references_in_node_recursive(node->as.while_stmt.block[i], ctx,
                                           depth + 1);
      }
    }
    break;

  case AST_RETURN:
    if (node->as.return_stmt.value) {
      count_references_in_node_recursive(node->as.return_stmt.value, ctx,
                                         depth + 1);
    }
    break;

  case AST_INDEX:
    if (node->as.index.list_expr) {
      count_references_in_node_recursive(node->as.index.list_expr, ctx,
                                         depth + 1);
    }
    if (node->as.index.index) {
      count_references_in_node_recursive(node->as.index.index, ctx, depth + 1);
    }
    break;

  case AST_SLICE:
    if (node->as.slice.list_expr) {
      count_references_in_node_recursive(node->as.slice.list_expr, ctx,
                                         depth + 1);
    }
    if (node->as.slice.start) {
      count_references_in_node_recursive(node->as.slice.start, ctx, depth + 1);
    }
    if (node->as.slice.end) {
      count_references_in_node_recursive(node->as.slice.end, ctx, depth + 1);
    }
    break;

  case AST_LIST:
    for (size_t i = 0; i < node->as.list.element_count; i++) {
      if (node->as.list.elements[i]) {
        count_references_in_node_recursive(node->as.list.elements[i], ctx,
                                           depth + 1);
      }
    }
    break;

  case AST_MAP:
    for (size_t i = 0; i < node->as.map.entry_count; i++) {
      if (node->as.map.keys[i]) {
        count_references_in_node_recursive(node->as.map.keys[i], ctx,
                                           depth + 1);
      }
      if (node->as.map.values[i]) {
        count_references_in_node_recursive(node->as.map.values[i], ctx,
                                           depth + 1);
      }
    }
    break;

  case AST_FSTRING:
    for (size_t i = 0; i < node->as.fstring.part_count; i++) {
      if (node->as.fstring.parts[i]) {
        count_references_in_node_recursive(node->as.fstring.parts[i], ctx,
                                           depth + 1);
      }
    }
    break;

  case AST_TRY:
    for (size_t i = 0; i < node->as.try_stmt.try_block_size; i++) {
      if (node->as.try_stmt.try_block[i]) {
        count_references_in_node_recursive(node->as.try_stmt.try_block[i], ctx,
                                           depth + 1);
      }
    }
    for (size_t i = 0; i < node->as.try_stmt.catch_block_count; i++) {
      if (node->as.try_stmt.catch_blocks[i].catch_var &&
          strcmp(node->as.try_stmt.catch_blocks[i].catch_var,
                 ctx->symbol_name) == 0) {
        ctx->count++;
      }
      for (size_t j = 0; j < node->as.try_stmt.catch_blocks[i].catch_block_size;
           j++) {
        if (node->as.try_stmt.catch_blocks[i].catch_block[j]) {
          count_references_in_node_recursive(
              node->as.try_stmt.catch_blocks[i].catch_block[j], ctx, depth + 1);
        }
      }
    }
    if (node->as.try_stmt.finally_block) {
      for (size_t i = 0; i < node->as.try_stmt.finally_block_size; i++) {
        if (node->as.try_stmt.finally_block[i]) {
          count_references_in_node_recursive(node->as.try_stmt.finally_block[i],
                                             ctx, depth + 1);
        }
      }
    }
    break;

  default:
    break;
  }
}

// Public wrapper that starts with depth 0
void count_references_in_node(ASTNode *node, void *ctx_ptr) {
  count_references_in_node_recursive(node, ctx_ptr, 0);
}

size_t count_symbol_references(const char *symbol_name, AST *ast) {
  if (!symbol_name || !ast || !ast->statements)
    return 0;

  ReferenceCountContext ctx = {symbol_name, 0};

  // Count references in all top-level statements
  for (size_t i = 0; i < ast->count; i++) {
    if (ast->statements[i]) {
      count_references_in_node_recursive(ast->statements[i], &ctx, 0);
    }
  }

  return ctx.count;
}
