/**
 * @file lsp_server.c
 * @brief Language Server Protocol implementation for Kronos
 *
 * Provides comprehensive IDE support for Kronos through the Language Server
 * Protocol. Communicates with editors via stdin/stdout using JSON-RPC 2.0.
 *
 * Features:
 * - Code completion (keywords, built-ins, variables, functions)
 * - Diagnostics (syntax errors, undefined variables, type mismatches)
 * - Go-to-definition for variables and functions
 * - Hover information with types and documentation
 * - Document symbols / outline view
 * - Semantic token highlighting
 * - Context-aware completions
 *
 * Usage: ./kronos-lsp
 * The server reads JSON-RPC messages from stdin and writes responses to stdout.
 */

#include "../frontend/parser.h"
#include "../frontend/tokenizer.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/**
 * Symbol types in the symbol table
 */
typedef enum {
  SYMBOL_VARIABLE,  /**< Variable declaration (set/let) */
  SYMBOL_FUNCTION,  /**< Function definition */
  SYMBOL_PARAMETER, /**< Function parameter */
} SymbolType;

/**
 * Symbol information structure
 *
 * Represents a symbol (variable, function, or parameter) in the source code.
 * Symbols are stored in a linked list for the document's symbol table.
 */
typedef struct Symbol {
  char *name;      /**< Symbol name */
  SymbolType type; /**< Type of symbol */
  size_t line;     /**< 1-based line number where symbol is defined */
  size_t column;   /**< 1-based column number where symbol is defined */
  char *type_name; /**< Optional type annotation (e.g., "number", "string") */
  bool is_mutable; /**< For variables: true for 'let', false for 'set' */
  size_t param_count;  /**< For functions: number of parameters */
  bool used;           /**< @deprecated Use read/written instead */
  bool written;        /**< Track if variable has been assigned to */
  bool read;           /**< Track if variable has been read from */
  struct Symbol *next; /**< Next symbol in linked list */
} Symbol;

/**
 * Imported module information
 */
typedef struct ImportedModule {
  char *name;      /**< Module name (e.g., "utils") */
  char *file_path; /**< File path if importing from file (NULL for built-in) */
  struct ImportedModule *next; /**< Next imported module */
} ImportedModule;

/**
 * Document state structure
 *
 * Maintains the current state of an open document, including its text,
 * parsed AST, and symbol table.
 */
typedef struct {
  char *uri;       /**< Document URI (file path) */
  char *text;      /**< Full document text */
  Symbol *symbols; /**< Linked list of symbols in the document */
  AST *ast;        /**< Parsed Abstract Syntax Tree */
  ImportedModule *imported_modules; /**< Linked list of imported modules */
} DocumentState;

/** Global document state (currently supports single file) */
static DocumentState *g_doc = NULL;

/**
 * @section JSON Parsing Utilities
 *
 * Lightweight JSON parsing functions for extracting values from LSP messages.
 * These functions handle nested objects, arrays, and string escaping.
 */

/**
 * @brief Skip whitespace characters
 *
 * @param s String to skip whitespace in
 * @return Pointer to first non-whitespace character
 */
static const char *skip_ws(const char *s) {
  while (s && *s && isspace((unsigned char)*s)) {
    s++;
  }
  return s;
}

/**
 * @brief Find the start of a JSON value for a given key
 *
 * Searches for a key-value pair in JSON and returns a pointer to the
 * start of the value (after the colon).
 *
 * @param json JSON string to search
 * @param key Key to find
 * @return Pointer to value start, or NULL if not found
 */
static const char *find_value_start(const char *json, const char *key) {
  if (!json || !key)
    return NULL;

  char pattern[128];
  int written = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  if (written <= 0 || (size_t)written >= sizeof(pattern))
    return NULL;

  const char *pos = json;
  size_t pattern_len = (size_t)written;
  while ((pos = strstr(pos, pattern)) != NULL) {
    pos += pattern_len;
    pos = skip_ws(pos);
    if (*pos != ':')
      continue;
    pos++;
    return skip_ws(pos);
  }
  return NULL;
}

/**
 * @brief Extract a string value from JSON
 *
 * Finds a key in JSON and extracts its string value, handling escape sequences.
 * The returned string must be freed by the caller.
 *
 * @param json JSON string to parse
 * @param key Key to look up
 * @return Extracted string (caller must free), or NULL if not found
 */
static char *json_get_string_value(const char *json, const char *key) {
  const char *value_start = find_value_start(json, key);
  if (!value_start || *value_start != '"')
    return NULL;

  value_start++; // skip opening quote
  const char *cursor = value_start;
  while (*cursor) {
    if (*cursor == '"') {
      const char *back = cursor - 1;
      bool escaped = false;
      while (back >= value_start && *back == '\\') {
        escaped = !escaped;
        back--;
      }
      if (!escaped)
        break;
    }
    cursor++;
  }
  if (*cursor != '"')
    return NULL;

  size_t raw_len = (size_t)(cursor - value_start);
  char *result = malloc(raw_len + 1);
  if (!result)
    return NULL;

  size_t out_idx = 0;
  for (const char *iter = value_start; iter < cursor; ++iter) {
    if (*iter == '\\' && (iter + 1) < cursor) {
      ++iter;
      switch (*iter) {
      case '"':
        result[out_idx++] = '"';
        break;
      case '\\':
        result[out_idx++] = '\\';
        break;
      case '/':
        result[out_idx++] = '/';
        break;
      case 'b':
        result[out_idx++] = '\b';
        break;
      case 'f':
        result[out_idx++] = '\f';
        break;
      case 'n':
        result[out_idx++] = '\n';
        break;
      case 'r':
        result[out_idx++] = '\r';
        break;
      case 't':
        result[out_idx++] = '\t';
        break;
      default:
        result[out_idx++] = *iter;
        break;
      }
    } else {
      result[out_idx++] = *iter;
    }
  }
  result[out_idx] = '\0';
  return result;
}

/**
 * @brief Extract an unquoted value from JSON
 *
 * Extracts numeric, boolean, or null values (not strings).
 * The returned string must be freed by the caller.
 *
 * @param json JSON string to parse
 * @param key Key to look up
 * @return Extracted value as string (caller must free), or NULL if not found
 */
static char *json_get_unquoted_value(const char *json, const char *key) {
  const char *value_start = find_value_start(json, key);
  if (!value_start || *value_start == '"' || *value_start == '\0')
    return NULL;

  const char *cursor = value_start;
  while (*cursor && !isspace((unsigned char)*cursor) && *cursor != ',' &&
         *cursor != '}' && *cursor != ']') {
    cursor++;
  }

  size_t len = (size_t)(cursor - value_start);
  if (len == 0)
    return NULL;

  char *result = malloc(len + 1);
  if (!result)
    return NULL;
  memcpy(result, value_start, len);
  result[len] = '\0';
  return result;
}

/**
 * @brief Extract the "id" field from a JSON-RPC message
 *
 * Handles both string and numeric IDs. The returned value is wrapped
 * in quotes if it was a string, or returned as-is if numeric.
 *
 * @param json JSON-RPC message
 * @return ID value (caller must free), or NULL if not found
 */
static char *json_get_id_value(const char *json) {
  char *string_id = json_get_string_value(json, "id");
  if (string_id) {
    size_t len = strlen(string_id);
    char *wrapped = malloc(len + 3);
    if (!wrapped) {
      free(string_id);
      return NULL;
    }
    wrapped[0] = '"';
    memcpy(wrapped + 1, string_id, len);
    wrapped[len + 1] = '"';
    wrapped[len + 2] = '\0';
    free(string_id);
    return wrapped;
  }
  return json_get_unquoted_value(json, "id");
}

/**
 * @brief Extract a nested JSON value using a dot-separated path
 *
 * Supports paths like "params.position.line" or "params.contentChanges.0.text".
 * Array indices are specified as numbers in the path.
 *
 * @param json JSON string to parse
 * @param path Dot-separated path (e.g., "params.textDocument.uri")
 * @return Extracted value (caller must free), or NULL if not found
 */
static char *json_get_nested_value(const char *json, const char *path) {
  if (!json || !path)
    return NULL;

  const char *current = json;
  char path_copy[256];
  strncpy(path_copy, path, sizeof(path_copy) - 1);
  path_copy[sizeof(path_copy) - 1] = '\0';

  char *saveptr;
  char *token = strtok_r(path_copy, ".", &saveptr);
  while (token) {
    // Check if token is a number (array index)
    if (isdigit((unsigned char)token[0])) {
      // Array index - find the nth element
      int index = atoi(token);
      current = skip_ws(current);
      if (*current != '[')
        return NULL;
      current++; // skip '['
      current = skip_ws(current);

      // Find the index-th element
      int current_index = 0;
      while (current_index < index && *current != '\0' && *current != ']') {
        // Skip current element
        if (*current == '"') {
          // String value
          current++;
          while (*current != '\0' && *current != '"') {
            if (*current == '\\' && *(current + 1) != '\0')
              current += 2;
            else
              current++;
          }
          if (*current == '"')
            current++;
        } else if (*current == '{') {
          // Object - find matching '}'
          int depth = 1;
          current++;
          while (depth > 0 && *current != '\0') {
            if (*current == '{')
              depth++;
            else if (*current == '}')
              depth--;
            else if (*current == '"') {
              current++;
              while (*current != '\0' && *current != '"') {
                if (*current == '\\' && *(current + 1) != '\0')
                  current += 2;
                else
                  current++;
              }
            }
            if (*current != '\0')
              current++;
          }
        } else if (*current == '[') {
          // Array - find matching ']'
          int depth = 1;
          current++;
          while (depth > 0 && *current != '\0') {
            if (*current == '[')
              depth++;
            else if (*current == ']')
              depth--;
            else if (*current == '"') {
              current++;
              while (*current != '\0' && *current != '"') {
                if (*current == '\\' && *(current + 1) != '\0')
                  current += 2;
                else
                  current++;
              }
            }
            if (*current != '\0')
              current++;
          }
        } else {
          // Simple value - skip to comma or ]
          while (*current != '\0' && *current != ',' && *current != ']') {
            current++;
          }
        }
        current = skip_ws(current);
        if (*current == ',') {
          current++;
          current = skip_ws(current);
          current_index++;
        } else if (*current == ']') {
          break;
        }
      }
      if (current_index < index)
        return NULL;
      current = skip_ws(current);
    } else {
      // Object key
      current = find_value_start(current, token);
      if (!current)
        return NULL;
      current = skip_ws(current);
      if (*current == ':') {
        current++;
        current = skip_ws(current);
      }
    }
    token = strtok_r(NULL, ".", &saveptr);
  }

  if (!current)
    return NULL;

  // Extract the value at current position
  current = skip_ws(current);
  if (*current == '"') {
    // String value
    const char *value_start = current + 1;
    const char *cursor = value_start;
    while (*cursor) {
      if (*cursor == '"') {
        const char *back = cursor - 1;
        bool escaped = false;
        while (back >= value_start && *back == '\\') {
          escaped = !escaped;
          back--;
        }
        if (!escaped)
          break;
      }
      cursor++;
    }
    if (*cursor != '"')
      return NULL;

    size_t raw_len = (size_t)(cursor - value_start);
    char *result = malloc(raw_len + 1);
    if (!result)
      return NULL;

    size_t out_idx = 0;
    for (const char *iter = value_start; iter < cursor; ++iter) {
      if (*iter == '\\' && (iter + 1) < cursor) {
        ++iter;
        switch (*iter) {
        case '"':
          result[out_idx++] = '"';
          break;
        case '\\':
          result[out_idx++] = '\\';
          break;
        case '/':
          result[out_idx++] = '/';
          break;
        case 'b':
          result[out_idx++] = '\b';
          break;
        case 'f':
          result[out_idx++] = '\f';
          break;
        case 'n':
          result[out_idx++] = '\n';
          break;
        case 'r':
          result[out_idx++] = '\r';
          break;
        case 't':
          result[out_idx++] = '\t';
          break;
        default:
          result[out_idx++] = *iter;
          break;
        }
      } else {
        result[out_idx++] = *iter;
      }
    }
    result[out_idx] = '\0';
    return result;
  } else {
    // Unquoted value
    const char *cursor = current;
    while (*cursor && !isspace((unsigned char)*cursor) && *cursor != ',' &&
           *cursor != '}' && *cursor != ']') {
      cursor++;
    }
    size_t len = (size_t)(cursor - current);
    if (len == 0)
      return NULL;
    char *result = malloc(len + 1);
    if (!result)
      return NULL;
    memcpy(result, current, len);
    result[len] = '\0';
    return result;
  }
}

/**
 * @section LSP Message Handling
 *
 * Functions for reading and writing JSON-RPC 2.0 messages over stdin/stdout.
 */

/**
 * @brief Read a JSON-RPC message from stdin
 *
 * Reads the Content-Length header and then the message body.
 * The body is allocated and must be freed by the caller.
 *
 * @param out_body Output parameter for message body (caller must free)
 * @param out_length Output parameter for body length
 * @return true on success, false on error or EOF
 */
static bool read_lsp_message(char **out_body, size_t *out_length) {
  if (!out_body || !out_length)
    return false;

  char header_line[1024];
  size_t content_length = 0;
  bool have_length = false;

  while (fgets(header_line, sizeof(header_line), stdin)) {
    if (header_line[0] == '\r' || header_line[0] == '\n') {
      break;
    }

    if (strncasecmp(header_line, "Content-Length:", 15) == 0) {
      content_length = (size_t)strtoul(header_line + 15, NULL, 10);
      have_length = true;
    }
  }

  if (!have_length) {
    return false;
  }

  char *body = malloc(content_length + 1);
  if (!body) {
    fprintf(stderr, "LSP server: failed to allocate %zu-byte body buffer\n",
            content_length);
    return false;
  }

  size_t read_total = 0;
  while (read_total < content_length) {
    size_t bytes_read =
        fread(body + read_total, 1, content_length - read_total, stdin);
    if (bytes_read == 0) {
      free(body);
      return false;
    }
    read_total += bytes_read;
  }

  body[content_length] = '\0';
  *out_body = body;
  *out_length = content_length;
  return true;
}

/**
 * @brief Escape special characters in a string for JSON
 *
 * Escapes backslashes, quotes, newlines, tabs, and control characters.
 *
 * @param input String to escape
 * @param output Output buffer (must be large enough)
 * @param output_size Size of output buffer
 */
static void json_escape(const char *input, char *output, size_t output_size) {
  size_t out_pos = 0;
  for (size_t i = 0; input[i] != '\0' && out_pos < output_size - 1; i++) {
    switch (input[i]) {
    case '\\':
      if (out_pos < output_size - 2) {
        output[out_pos++] = '\\';
        output[out_pos++] = '\\';
      }
      break;
    case '"':
      if (out_pos < output_size - 2) {
        output[out_pos++] = '\\';
        output[out_pos++] = '"';
      }
      break;
    case '\n':
      if (out_pos < output_size - 2) {
        output[out_pos++] = '\\';
        output[out_pos++] = 'n';
      }
      break;
    case '\r':
      if (out_pos < output_size - 2) {
        output[out_pos++] = '\\';
        output[out_pos++] = 'r';
      }
      break;
    case '\t':
      if (out_pos < output_size - 2) {
        output[out_pos++] = '\\';
        output[out_pos++] = 't';
      }
      break;
    default:
      if ((unsigned char)input[i] < 0x20) {
        if (out_pos < output_size - 6) {
          snprintf(output + out_pos, output_size - out_pos, "\\u%04x",
                   (unsigned char)input[i]);
          out_pos += 6;
        }
      } else {
        output[out_pos++] = input[i];
      }
      break;
    }
  }
  output[out_pos] = '\0';
}

/**
 * @brief Send a JSON-RPC response
 *
 * Formats and sends a response message to stdout with Content-Length header.
 *
 * @param id Request ID (JSON value, e.g., "1" or "\"abc\"")
 * @param result Result JSON (can be "null" for void responses)
 */
static void send_response(const char *id, const char *result) {
  char buffer[8192];
  int snprintf_result =
      snprintf(buffer, sizeof(buffer),
               "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}", id, result);
  size_t body_len =
      (snprintf_result < 0 || (size_t)snprintf_result >= sizeof(buffer))
          ? sizeof(buffer) - 1
          : (size_t)snprintf_result;
  printf("Content-Length: %zu\r\n\r\n", body_len);
  fwrite(buffer, 1, body_len, stdout);
  fflush(stdout);
}

/**
 * @brief Send a JSON-RPC notification (no response expected)
 *
 * Used for sending diagnostics and other notifications to the client.
 *
 * @param method Notification method name (e.g.,
 * "textDocument/publishDiagnostics")
 * @param params Parameters JSON object
 */
static void send_notification(const char *method, const char *params) {
  int required_size =
      snprintf(NULL, 0, "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s}",
               method, params);
  if (required_size < 0)
    return;

  size_t body_len = (size_t)required_size;
  char stack_buffer[4096];
  char *buffer = stack_buffer;
  bool allocated = false;

  if (body_len >= sizeof(stack_buffer)) {
    buffer = malloc(body_len + 1);
    if (!buffer)
      return;
    allocated = true;
  }

  int snprintf_result = snprintf(
      buffer, body_len + 1,
      "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s}", method, params);

  if (snprintf_result < 0 || (size_t)snprintf_result != body_len) {
    if (allocated)
      free(buffer);
    return;
  }

  printf("Content-Length: %zu\r\n\r\n", body_len);
  fwrite(buffer, 1, body_len, stdout);
  fflush(stdout);

  if (allocated)
    free(buffer);
}

// Free symbol list
/**
 * @section Symbol Table Management
 *
 * Functions for building and managing the symbol table from the AST.
 */

/**
 * @brief Free a linked list of symbols
 *
 * Recursively frees all symbols and their associated strings.
 *
 * @param sym Head of symbol linked list
 */
static void free_symbols(Symbol *sym) {
  while (sym) {
    Symbol *next = sym->next;
    free(sym->name);
    free(sym->type_name);
    free(sym);
    sym = next;
  }
}

/**
 * @brief Estimate position from AST node
 *
 * Since AST nodes don't store exact positions, this estimates line/column
 * based on indentation level. This is approximate but sufficient for
 * symbol table purposes.
 *
 * @param node AST node
 * @param line Output parameter for line number (1-based)
 * @param col Output parameter for column number (1-based)
 */
static void get_node_position(ASTNode *node, size_t *line, size_t *col) {
  // Estimate position based on node type and structure
  // This is approximate since AST doesn't store exact positions
  *line = 1;
  *col = 1;
  if (node && node->indent >= 0) {
    // Use indent as a rough estimate
    *line = (size_t)(node->indent / 4) + 1;
    *col = (size_t)(node->indent % 4) + 1;
  }
}

// Free imported modules list
static void free_imported_modules(ImportedModule *modules) {
  while (modules) {
    ImportedModule *next = modules->next;
    free(modules->name);
    free(modules->file_path);
    free(modules);
    modules = next;
  }
}

// Helper function to check if a module is imported
static bool is_module_imported(const char *module_name) {
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

/**
 * @brief Free document state and all its resources
 *
 * Releases the URI, text, symbol table, and AST.
 *
 * @param doc Document state to free (safe to pass NULL)
 */
static void free_document_state(DocumentState *doc) {
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

// Helper function to process a block of statements and add symbols
// This is called recursively for function bodies
static void process_statements_for_symbols(ASTNode **statements, size_t count,
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
        sym->used = false;
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
      sym->used = false;
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
        param->used = false;
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
        sym->used = false;
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
          sym->used = false;
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

// Build symbol table from AST
/**
 * @brief Build the complete symbol table for a document
 *
 * Processes the AST to extract all symbols (variables, functions, parameters)
 * and stores them in the document state. Also tracks symbol usage
 * (read/written).
 *
 * @param doc Document state to populate
 * @param ast Parsed AST
 * @param text Source code text
 */
static void build_symbol_table(DocumentState *doc, AST *ast,
                               const char *source) {
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
  size_t *line_starts = NULL;
  size_t line_count = 0;
  size_t capacity = 64;
  line_starts = malloc(capacity * sizeof(size_t));
  if (line_starts) {
    line_starts[0] = 0;
    line_count = 1;
    for (size_t i = 0; source[i] != '\0'; i++) {
      if (source[i] == '\n') {
        if (line_count >= capacity) {
          capacity *= 2;
          size_t *new_starts = realloc(line_starts, capacity * sizeof(size_t));
          if (!new_starts) {
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

// Diagnostic checking - parse file and return errors with accurate positions
// Forward declarations
static Symbol *find_symbol(const char *name);

// Type inference for expressions
typedef enum {
  TYPE_UNKNOWN,
  TYPE_NUMBER,
  TYPE_STRING,
  TYPE_LIST,
  TYPE_MAP,
  TYPE_RANGE,
  TYPE_BOOL,
  TYPE_NULL
} ExprType;

// Forward declarations
static ASTNode *find_variable_assignment(AST *ast, const char *var_name);
static ExprType infer_type_with_ast(ASTNode *node, Symbol *symbols, AST *ast);

// Infer type from AST node (with optional AST for variable value lookup)
static ExprType infer_type_with_ast(ASTNode *node, Symbol *symbols, AST *ast) {
  if (!node)
    return TYPE_UNKNOWN;

  switch (node->type) {
  case AST_NUMBER:
    return TYPE_NUMBER;
  case AST_STRING:
  case AST_FSTRING:
    return TYPE_STRING;
  case AST_BOOL:
    return TYPE_BOOL;
  case AST_NULL:
    return TYPE_NULL;
  case AST_LIST:
    return TYPE_LIST;
  case AST_MAP:
    return TYPE_MAP;
  case AST_VAR: {
    Symbol *sym = find_symbol(node->as.var_name);
    if (sym && sym->type_name) {
      if (strcmp(sym->type_name, "number") == 0)
        return TYPE_NUMBER;
      if (strcmp(sym->type_name, "string") == 0)
        return TYPE_STRING;
      if (strcmp(sym->type_name, "list") == 0)
        return TYPE_LIST;
      if (strcmp(sym->type_name, "map") == 0)
        return TYPE_MAP;
      if (strcmp(sym->type_name, "bool") == 0)
        return TYPE_BOOL;
    }
    // If no explicit type annotation, try to infer from assigned value
    if (ast) {
      ASTNode *assign_node = find_variable_assignment(ast, node->as.var_name);
      if (assign_node && assign_node->as.assign.value) {
        // Recursively infer type from the assigned value
        return infer_type_with_ast(assign_node->as.assign.value, symbols, ast);
      }
    }
    return TYPE_UNKNOWN;
  }
  case AST_BINOP:
    // Plus can return number (addition) or string (concatenation)
    if (node->as.binop.op == BINOP_ADD) {
      ExprType left_type =
          infer_type_with_ast(node->as.binop.left, symbols, ast);
      ExprType right_type =
          infer_type_with_ast(node->as.binop.right, symbols, ast);
      // If either operand is a string, result is string (concatenation)
      // This handles: string + string, string + number, number + string
      if (left_type == TYPE_STRING || right_type == TYPE_STRING) {
        return TYPE_STRING;
      }
      // Otherwise, result is number (addition)
      return TYPE_NUMBER;
    }
    // Other arithmetic operations return numbers
    if (node->as.binop.op == BINOP_SUB || node->as.binop.op == BINOP_MUL ||
        node->as.binop.op == BINOP_DIV || node->as.binop.op == BINOP_MOD) {
      return TYPE_NUMBER;
    }
    // Unary operators
    if (node->as.binop.op == BINOP_NOT || node->as.binop.op == BINOP_NEG) {
      // Unary NOT returns bool, unary NEG returns number
      if (node->as.binop.op == BINOP_NOT) {
        return TYPE_BOOL;
      } else {
        return TYPE_NUMBER;
      }
    }
    // Comparisons return bool
    if (node->as.binop.op == BINOP_EQ || node->as.binop.op == BINOP_NEQ ||
        node->as.binop.op == BINOP_GT || node->as.binop.op == BINOP_LT ||
        node->as.binop.op == BINOP_GTE || node->as.binop.op == BINOP_LTE) {
      return TYPE_BOOL;
    }
    return TYPE_UNKNOWN;
  case AST_INDEX: {
    // List/string/map indexing - for maps, return TYPE_UNKNOWN (value type)
    // For lists/strings, return the element type
    ExprType container_type =
        infer_type_with_ast(node->as.index.list_expr, symbols, ast);
    if (container_type == TYPE_MAP) {
      // Map indexing returns the value type, which we can't infer statically
      return TYPE_UNKNOWN;
    }
    return container_type;
  }
  case AST_RANGE:
    return TYPE_RANGE;
  case AST_SLICE: {
    // Slicing returns the same type as the container
    ExprType container_type =
        infer_type_with_ast(node->as.slice.list_expr, symbols, ast);
    return container_type;
  }
  default:
    return TYPE_UNKNOWN;
  }
}

// Get constant numeric value if possible
static bool get_constant_number(ASTNode *node, double *value) {
  if (!node)
    return false;
  if (node->type == AST_NUMBER) {
    *value = node->as.number;
    return true;
  }
  return false;
}

// Find position of a node in source text (helper)
static void find_node_position(ASTNode *node, const char *text,
                               const char *pattern, size_t *line, size_t *col) {
  (void)node; // Unused parameter (kept for API consistency)
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

// Forward declarations
static bool find_nth_occurrence(const char *text, const char *varname, size_t n,
                                size_t *line, size_t *col);

// Find the position and length of the value in an assignment statement
// Returns true if found, false otherwise
static bool find_assignment_value_position(const char *text,
                                           const char *varname,
                                           size_t occurrence,
                                           ASTNode *value_node, size_t *line,
                                           size_t *col, size_t *length) {
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

// Get expected argument count for built-in functions
// Returns -1 if function is not a built-in, or if it has variable arguments
static int get_builtin_arg_count(const char *func_name) {
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
      strcmp(func_name, "dirname") == 0 ||
      strcmp(func_name, "basename") == 0) {
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
      strcmp(func_name, "join_path") == 0 ||
      strcmp(func_name, "match") == 0 ||
      strcmp(func_name, "search") == 0 ||
      strcmp(func_name, "findall") == 0 ||
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

// Find function call position in source text
static void find_call_position(const char *text, const char *func_name,
                               size_t *line, size_t *col) {
  *line = 1;
  *col = 0;

  // Search for "call <func_name> with" pattern
  char pattern[256];
  snprintf(pattern, sizeof(pattern), "call %s with", func_name);

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

// Find the position of the first argument in a function call
// Returns true if found, false otherwise
static bool find_call_argument_position(const char *text, const char *func_name,
                                        ASTNode *arg_node, size_t *line,
                                        size_t *col, size_t *length) {
  *line = 1;
  *col = 0;
  *length = 0;
  if (!text || !func_name || !arg_node)
    return false;

  // Find "call <func_name> with" pattern
  char pattern[256];
  snprintf(pattern, sizeof(pattern), "call %s with", func_name);
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

// Check function call argument counts in AST
static void check_function_calls(AST *ast, const char *text, Symbol *symbols,
                                 char *diagnostics, size_t *pos,
                                 size_t *remaining, bool *has_diagnostics) {
  if (!ast || !ast->statements)
    return;

  for (size_t i = 0; i < ast->count; i++) {
    ASTNode *node = ast->statements[i];
    if (!node)
      continue;

    if (node->type == AST_CALL) {
      const char *func_name = node->as.call.name;
      size_t arg_count = node->as.call.arg_count;

      // Check for module.function syntax
      const char *dot = strchr(func_name, '.');
      const char *actual_func_name = func_name;
      if (dot) {
        size_t module_len = (size_t)(dot - func_name);
        char *module_name = malloc(module_len + 1);
        if (module_name) {
          strncpy(module_name, func_name, module_len);
          module_name[module_len] = '\0';
          if (strcmp(module_name, "math") == 0) {
            actual_func_name = dot + 1;
          } else if (is_module_imported(module_name)) {
            // File-based module - skip validation for now (can't parse module
            // files) This prevents false "unknown module" errors
            free(module_name);
            continue;
          } else {
            free(module_name);
            continue; // Unknown module (skip to avoid false errors)
          }
          free(module_name);
        }
      }

      // Check built-in functions
      int expected_args = get_builtin_arg_count(actual_func_name);
      if (expected_args > 0) {
        if ((size_t)expected_args != arg_count) {
          // Find position in source text
          size_t line = 1, col = 0;
          find_call_position(text, func_name, &line, &col);

          char escaped_msg[512];
          snprintf(escaped_msg, sizeof(escaped_msg),
                   "Function '%s' expects %d argument%s, but got %zu",
                   func_name, expected_args, expected_args == 1 ? "" : "s",
                   arg_count);
          char escaped_msg_final[512];
          json_escape(escaped_msg, escaped_msg_final,
                      sizeof(escaped_msg_final));

          int written = snprintf(
              diagnostics + *pos, *remaining,
              "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
              "\"end\":{\"line\":%zu,\"character\":%zu}},"
              "\"severity\":1,"
              "\"message\":\"%s\"}",
              *has_diagnostics ? "," : "", line - 1, col, line - 1, col + 20,
              escaped_msg_final);
          if (written > 0 && (size_t)written < *remaining) {
            *pos += (size_t)written;
            *remaining -= (size_t)written;
            *has_diagnostics = true;
          }
        }
      } else if (expected_args == -2) {
        // Variable arguments - check at least 1
        if (arg_count < 1) {
          size_t line = 1, col = 0;
          find_call_position(text, func_name, &line, &col);

          char escaped_msg[512];
          snprintf(escaped_msg, sizeof(escaped_msg),
                   "Function '%s' expects at least 1 argument, but got %zu",
                   func_name, arg_count);
          char escaped_msg_final[512];
          json_escape(escaped_msg, escaped_msg_final,
                      sizeof(escaped_msg_final));

          int written = snprintf(
              diagnostics + *pos, *remaining,
              "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
              "\"end\":{\"line\":%zu,\"character\":%zu}},"
              "\"severity\":1,"
              "\"message\":\"%s\"}",
              *has_diagnostics ? "," : "", line - 1, col, line - 1, col + 20,
              escaped_msg_final);
          if (written > 0 && (size_t)written < *remaining) {
            *pos += (size_t)written;
            *remaining -= (size_t)written;
            *has_diagnostics = true;
          }
        }
      } else {
        // Check user-defined functions
        Symbol *sym = find_symbol(func_name);
        if (sym && sym->type == SYMBOL_FUNCTION) {
          if (sym->param_count != arg_count) {
            size_t line = 1, col = 0;
            find_call_position(text, func_name, &line, &col);

            char escaped_msg[512];
            snprintf(escaped_msg, sizeof(escaped_msg),
                     "Function '%s' expects %zu argument%s, but got %zu",
                     func_name, sym->param_count,
                     sym->param_count == 1 ? "" : "s", arg_count);
            char escaped_msg_final[512];
            json_escape(escaped_msg, escaped_msg_final,
                        sizeof(escaped_msg_final));

            int written = snprintf(
                diagnostics + *pos, *remaining,
                "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
                "\"end\":{\"line\":%zu,\"character\":%zu}},"
                "\"severity\":1,"
                "\"message\":\"%s\"}",
                *has_diagnostics ? "," : "", line - 1, col, line - 1, col + 20,
                escaped_msg_final);
            if (written > 0 && (size_t)written < *remaining) {
              *pos += (size_t)written;
              *remaining -= (size_t)written;
              *has_diagnostics = true;
            }
          }
        } else if (!sym) {
          // Undefined function (not built-in and not user-defined)
          size_t line = 1, col = 0;
          find_call_position(text, func_name, &line, &col);

          char escaped_msg[512];
          snprintf(escaped_msg, sizeof(escaped_msg), "Undefined function '%s'",
                   func_name);
          char escaped_msg_final[512];
          json_escape(escaped_msg, escaped_msg_final,
                      sizeof(escaped_msg_final));

          int written = snprintf(
              diagnostics + *pos, *remaining,
              "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
              "\"end\":{\"line\":%zu,\"character\":%zu}},"
              "\"severity\":1,"
              "\"message\":\"%s\"}",
              *has_diagnostics ? "," : "", line - 1, col, line - 1, col + 20,
              escaped_msg_final);
          if (written > 0 && (size_t)written < *remaining) {
            *pos += (size_t)written;
            *remaining -= (size_t)written;
            *has_diagnostics = true;
          }
        }
      }
    }

    // Recursively check nested structures
    if (node->type == AST_IF) {
      // Check if block
      if (node->as.if_stmt.block) {
        AST temp_ast = {node->as.if_stmt.block, node->as.if_stmt.block_size,
                        node->as.if_stmt.block_size};
        check_function_calls(&temp_ast, text, symbols, diagnostics, pos,
                             remaining, has_diagnostics);
      }
      // Check else-if blocks
      for (size_t j = 0; j < node->as.if_stmt.else_if_count; j++) {
        if (node->as.if_stmt.else_if_blocks[j]) {
          AST temp_ast = {node->as.if_stmt.else_if_blocks[j],
                          node->as.if_stmt.else_if_block_sizes[j],
                          node->as.if_stmt.else_if_block_sizes[j]};
          check_function_calls(&temp_ast, text, symbols, diagnostics, pos,
                               remaining, has_diagnostics);
        }
      }
      // Check else block
      if (node->as.if_stmt.else_block) {
        AST temp_ast = {node->as.if_stmt.else_block,
                        node->as.if_stmt.else_block_size,
                        node->as.if_stmt.else_block_size};
        check_function_calls(&temp_ast, text, symbols, diagnostics, pos,
                             remaining, has_diagnostics);
      }
    } else if (node->type == AST_FOR || node->type == AST_WHILE) {
      ASTNode **block = NULL;
      size_t block_size = 0;
      if (node->type == AST_FOR) {
        block = node->as.for_stmt.block;
        block_size = node->as.for_stmt.block_size;
      } else {
        block = node->as.while_stmt.block;
        block_size = node->as.while_stmt.block_size;
      }
      if (block) {
        AST temp_ast = {block, block_size, block_size};
        check_function_calls(&temp_ast, text, symbols, diagnostics, pos,
                             remaining, has_diagnostics);
      }
    } else if (node->type == AST_FUNCTION) {
      if (node->as.function.block) {
        AST temp_ast = {node->as.function.block, node->as.function.block_size,
                        node->as.function.block_size};
        check_function_calls(&temp_ast, text, symbols, diagnostics, pos,
                             remaining, has_diagnostics);
      }
    }
  }
}

// Find the Nth occurrence of "set <varname> to" in text (for finding specific
// assignments) Only matches actual statements, not comments or strings Returns
// true if found, false otherwise
static bool find_nth_occurrence(const char *text, const char *varname, size_t n,
                                size_t *line, size_t *col) {
  *line = 1;
  *col = 0;
  if (!text || !varname || n == 0)
    return false;

  // Build patterns: both "let <varname> to" and "set <varname> to"
  char pattern_let[256];
  char pattern_set[256];
  snprintf(pattern_let, sizeof(pattern_let), "let %s to", varname);
  snprintf(pattern_set, sizeof(pattern_set), "set %s to", varname);
  size_t pattern_let_len = strlen(pattern_let);
  size_t pattern_set_len = strlen(pattern_set);

  // Collect all matches with their positions
  typedef struct {
    const char *pos;
    size_t len;
  } Match;
  Match matches[256]; // Max 256 matches
  size_t match_count = 0;

  // Search for "let" pattern
  const char *pos = text;
  while ((pos = strstr(pos, pattern_let)) != NULL && match_count < 256) {
    matches[match_count].pos = pos;
    matches[match_count].len = pattern_let_len;
    match_count++;
    pos += pattern_let_len;
  }

  // Search for "set" pattern
  pos = text;
  while ((pos = strstr(pos, pattern_set)) != NULL && match_count < 256) {
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
      pos += pattern_len;
      continue;
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
      pos += pattern_len;
      continue;
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

// Check for undefined variables
// Helper function to find variable assignment in AST
static ASTNode *find_variable_assignment(AST *ast, const char *var_name) {
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

// Recursively check expressions for diagnostics
// seen_vars and seen_count are optional - used to check if variables were
// assigned earlier in scope
typedef struct {
  char *name;
  bool is_mutable;
  size_t assignment_count;
  size_t first_statement_index;
} SeenVar;

static void check_expression(ASTNode *node, const char *text, Symbol *symbols,
                             AST *ast, char *diagnostics, size_t *pos,
                             size_t *remaining, bool *has_diagnostics,
                             SeenVar *seen_vars, size_t seen_count) {
  if (!node)
    return;

  // Check index operations
  if (node->type == AST_INDEX) {
    // Mark list/string variable as read (indexing is a read operation)
    if (node->as.index.list_expr->type == AST_VAR) {
      Symbol *list_sym = find_symbol(node->as.index.list_expr->as.var_name);
      if (list_sym && list_sym->type == SYMBOL_VARIABLE) {
        list_sym->read = true;
        list_sym->used = true;
      }
    }

    ExprType list_type =
        infer_type_with_ast(node->as.index.list_expr, symbols, ast);
    ASTNode *index_node = node->as.index.index;
    ExprType index_type = infer_type_with_ast(index_node, symbols, ast);

    // Check for unsafe memory access: indexing into null/undefined
    if (list_type == TYPE_NULL) {
      size_t line = 1, col = 0;
      find_node_position(node, text, "at", &line, &col);

      char escaped_msg[512] =
          "Unsafe memory access: cannot index into null/undefined";
      char escaped_msg_final[512];
      json_escape(escaped_msg, escaped_msg_final, sizeof(escaped_msg_final));

      int written =
          snprintf(diagnostics + *pos, *remaining,
                   "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
                   "\"end\":{\"line\":%zu,\"character\":%zu}},"
                   "\"severity\":1,"
                   "\"message\":\"%s\"}",
                   *has_diagnostics ? "," : "", line - 1, col, line - 1,
                   col + 20, escaped_msg_final);
      if (written > 0 && (size_t)written < *remaining) {
        *pos += (size_t)written;
        *remaining -= (size_t)written;
        *has_diagnostics = true;
      }
    }

    // Index must be a number for lists/strings/ranges, but maps accept any key
    // type Check both inferred type and direct AST node type
    bool is_string_index =
        (index_node->type == AST_STRING || index_node->type == AST_FSTRING);
    // Only check index type for non-map containers
    if (list_type != TYPE_MAP &&
        (is_string_index ||
         (index_type != TYPE_NUMBER && index_type != TYPE_UNKNOWN &&
          index_type == TYPE_STRING))) {
      size_t line = 1, col = 0;
      find_node_position(node, text, "at", &line, &col);

      char escaped_msg[512] = "List/string/range index must be a number";
      char escaped_msg_final[512];
      json_escape(escaped_msg, escaped_msg_final, sizeof(escaped_msg_final));

      int written =
          snprintf(diagnostics + *pos, *remaining,
                   "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
                   "\"end\":{\"line\":%zu,\"character\":%zu}},"
                   "\"severity\":1,"
                   "\"message\":\"%s\"}",
                   *has_diagnostics ? "," : "", line - 1, col, line - 1,
                   col + 20, escaped_msg_final);
      if (written > 0 && (size_t)written < *remaining) {
        *pos += (size_t)written;
        *remaining -= (size_t)written;
        *has_diagnostics = true;
      }
    }

    // Check for out-of-bounds (constant list and index)
    // Maps accept any key type, so we only check lists here
    if (list_type == TYPE_LIST) {
      double index_val;
      if (get_constant_number(node->as.index.index, &index_val)) {
        size_t list_len = 0;
        bool found_list = false;

        // Check if it's a direct list literal
        if (node->as.index.list_expr->type == AST_LIST) {
          list_len = node->as.index.list_expr->as.list.element_count;
          found_list = true;
        } else if (node->as.index.list_expr->type == AST_VAR && ast) {
          // If it's a variable, try to find where it was assigned to a list
          // literal
          const char *var_name = node->as.index.list_expr->as.var_name;
          ASTNode *assign_node = find_variable_assignment(ast, var_name);
          if (assign_node && assign_node->as.assign.value &&
              assign_node->as.assign.value->type == AST_LIST) {
            list_len = assign_node->as.assign.value->as.list.element_count;
            found_list = true;
          }
        }

        if (found_list && (index_val < 0 || (size_t)index_val >= list_len)) {
          size_t line = 1, col = 0;
          find_node_position(node, text, "at", &line, &col);

          char escaped_msg[512] = "List index out of bounds";
          char escaped_msg_final[512];
          json_escape(escaped_msg, escaped_msg_final,
                      sizeof(escaped_msg_final));

          int written = snprintf(
              diagnostics + *pos, *remaining,
              "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
              "\"end\":{\"line\":%zu,\"character\":%zu}},"
              "\"severity\":1,"
              "\"message\":\"%s\"}",
              *has_diagnostics ? "," : "", line - 1, col, line - 1, col + 20,
              escaped_msg_final);
          if (written > 0 && (size_t)written < *remaining) {
            *pos += (size_t)written;
            *remaining -= (size_t)written;
            *has_diagnostics = true;
          }
        }
      }
    }

    // Recursively check nested expressions
    check_expression(node->as.index.list_expr, text, symbols, ast, diagnostics,
                     pos, remaining, has_diagnostics, NULL, 0);
    check_expression(node->as.index.index, text, symbols, ast, diagnostics, pos,
                     remaining, has_diagnostics, NULL, 0);
    return;
  }

  // Check binary operations
  if (node->type == AST_BINOP) {
    ExprType left_type = infer_type_with_ast(node->as.binop.left, symbols, ast);
    ExprType right_type =
        infer_type_with_ast(node->as.binop.right, symbols, ast);

    // Plus (addition/concatenation) - allow numbers, strings, or mixed (string
    // + number) String + number or number + string results in string
    // concatenation Number + number results in addition String + string results
    // in concatenation No type errors for plus - it's flexible
    if (node->as.binop.op == BINOP_ADD) {
      // No type checking needed - plus handles all combinations
      // (string + string, string + number, number + string, number + number)
    }
    // Other arithmetic operations require numbers
    else if (node->as.binop.op == BINOP_SUB || node->as.binop.op == BINOP_MUL ||
             node->as.binop.op == BINOP_DIV || node->as.binop.op == BINOP_MOD) {
      if (left_type != TYPE_NUMBER || right_type != TYPE_NUMBER) {
        if (left_type != TYPE_UNKNOWN && right_type != TYPE_UNKNOWN) {
          const char *op_name = node->as.binop.op == BINOP_SUB   ? "subtract"
                                : node->as.binop.op == BINOP_MUL ? "multiply"
                                : node->as.binop.op == BINOP_DIV ? "divide"
                                                                 : "modulo";
          size_t line = 1, col = 0;
          char pattern[256];
          if (node->as.binop.op == BINOP_SUB)
            snprintf(pattern, sizeof(pattern), "minus");
          else if (node->as.binop.op == BINOP_MUL)
            snprintf(pattern, sizeof(pattern), "times");
          else if (node->as.binop.op == BINOP_DIV)
            snprintf(pattern, sizeof(pattern), "divided by");
          else if (node->as.binop.op == BINOP_MOD)
            snprintf(pattern, sizeof(pattern), "mod");
          find_node_position(node, text, pattern, &line, &col);

          char escaped_msg[512];
          snprintf(escaped_msg, sizeof(escaped_msg),
                   "Cannot %s - both values must be numbers", op_name);
          char escaped_msg_final[512];
          json_escape(escaped_msg, escaped_msg_final,
                      sizeof(escaped_msg_final));

          int written = snprintf(
              diagnostics + *pos, *remaining,
              "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
              "\"end\":{\"line\":%zu,\"character\":%zu}},"
              "\"severity\":1,"
              "\"message\":\"%s\"}",
              *has_diagnostics ? "," : "", line - 1, col, line - 1, col + 20,
              escaped_msg_final);
          if (written > 0 && (size_t)written < *remaining) {
            *pos += (size_t)written;
            *remaining -= (size_t)written;
            *has_diagnostics = true;
          }
        }
      }

      // Check for division by zero (constant)
      if (node->as.binop.op == BINOP_DIV) {
        double right_val;
        if (get_constant_number(node->as.binop.right, &right_val) &&
            right_val == 0.0) {
          size_t line = 1, col = 0;
          find_node_position(node, text, "divided by", &line, &col);

          char escaped_msg[512] = "Cannot divide by zero";
          char escaped_msg_final[512];
          json_escape(escaped_msg, escaped_msg_final,
                      sizeof(escaped_msg_final));

          int written = snprintf(
              diagnostics + *pos, *remaining,
              "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
              "\"end\":{\"line\":%zu,\"character\":%zu}},"
              "\"severity\":1,"
              "\"message\":\"%s\"}",
              *has_diagnostics ? "," : "", line - 1, col, line - 1, col + 20,
              escaped_msg_final);
          if (written > 0 && (size_t)written < *remaining) {
            *pos += (size_t)written;
            *remaining -= (size_t)written;
            *has_diagnostics = true;
          }
        }
      }
    }
    // Unary operators (NOT and NEG) - right is NULL for unary ops
    else if (node->as.binop.op == BINOP_NOT || node->as.binop.op == BINOP_NEG) {
      if (node->as.binop.right == NULL) {
        // Valid unary operator - check operand type
        ExprType operand_type =
            infer_type_with_ast(node->as.binop.left, symbols, ast);
        if (operand_type != TYPE_UNKNOWN) {
          if (node->as.binop.op == BINOP_NOT) {
            // NOT requires boolean operand
            if (operand_type != TYPE_BOOL) {
              size_t line = 1, col = 0;
              find_node_position(node, text, "not", &line, &col);
              char escaped_msg[512];
              snprintf(escaped_msg, sizeof(escaped_msg),
                       "Cannot negate - operand must be boolean");
              char escaped_msg_final[512];
              json_escape(escaped_msg, escaped_msg_final,
                          sizeof(escaped_msg_final));
              int written = snprintf(
                  diagnostics + *pos, *remaining,
                  "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
                  "\"end\":{\"line\":%zu,\"character\":%zu}},"
                  "\"severity\":1,"
                  "\"message\":\"%s\"}",
                  *has_diagnostics ? "," : "", line - 1, col, line - 1, col + 3,
                  escaped_msg_final);
              if (written > 0 && (size_t)written < *remaining) {
                *pos += (size_t)written;
                *remaining -= (size_t)written;
                *has_diagnostics = true;
              }
            }
          } else if (node->as.binop.op == BINOP_NEG) {
            // NEG requires number operand
            if (operand_type != TYPE_NUMBER) {
              size_t line = 1, col = 0;
              find_node_position(node, text, "-", &line, &col);
              char escaped_msg[512];
              snprintf(escaped_msg, sizeof(escaped_msg),
                       "Cannot negate - operand must be a number");
              char escaped_msg_final[512];
              json_escape(escaped_msg, escaped_msg_final,
                          sizeof(escaped_msg_final));
              int written = snprintf(
                  diagnostics + *pos, *remaining,
                  "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
                  "\"end\":{\"line\":%zu,\"character\":%zu}},"
                  "\"severity\":1,"
                  "\"message\":\"%s\"}",
                  *has_diagnostics ? "," : "", line - 1, col, line - 1, col + 1,
                  escaped_msg_final);
              if (written > 0 && (size_t)written < *remaining) {
                *pos += (size_t)written;
                *remaining -= (size_t)written;
                *has_diagnostics = true;
              }
            }
          }
        }
      }
      // If right is not NULL, skip unary handling (invalid state for unary op)
    }

    // Comparison operations require numbers
    if (node->as.binop.op == BINOP_GT || node->as.binop.op == BINOP_LT ||
        node->as.binop.op == BINOP_GTE || node->as.binop.op == BINOP_LTE) {
      if (left_type != TYPE_NUMBER || right_type != TYPE_NUMBER) {
        if (left_type != TYPE_UNKNOWN && right_type != TYPE_UNKNOWN) {
          size_t line = 1, col = 0;
          char pattern[256] = "greater than";
          if (node->as.binop.op == BINOP_LT)
            snprintf(pattern, sizeof(pattern), "less than");
          else if (node->as.binop.op == BINOP_GTE)
            snprintf(pattern, sizeof(pattern), "greater than or equal to");
          else if (node->as.binop.op == BINOP_LTE)
            snprintf(pattern, sizeof(pattern), "less than or equal to");
          find_node_position(node, text, pattern, &line, &col);

          char escaped_msg[512] =
              "Cannot compare - both values must be numbers";
          char escaped_msg_final[512];
          json_escape(escaped_msg, escaped_msg_final,
                      sizeof(escaped_msg_final));

          int written = snprintf(
              diagnostics + *pos, *remaining,
              "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
              "\"end\":{\"line\":%zu,\"character\":%zu}},"
              "\"severity\":1,"
              "\"message\":\"%s\"}",
              *has_diagnostics ? "," : "", line - 1, col, line - 1, col + 20,
              escaped_msg_final);
          if (written > 0 && (size_t)written < *remaining) {
            *pos += (size_t)written;
            *remaining -= (size_t)written;
            *has_diagnostics = true;
          }
        }
      }
    }

    // Recursively check nested expressions
    // For unary operators (NOT, NEG), right is NULL
    if (node->as.binop.right == NULL) {
      // Unary operator - only check left operand
      check_expression(node->as.binop.left, text, symbols, ast, diagnostics,
                       pos, remaining, has_diagnostics, seen_vars, seen_count);
    } else {
      // Binary operator - check both operands
      check_expression(node->as.binop.left, text, symbols, ast, diagnostics,
                       pos, remaining, has_diagnostics, seen_vars, seen_count);
      check_expression(node->as.binop.right, text, symbols, ast, diagnostics,
                       pos, remaining, has_diagnostics, seen_vars, seen_count);
    }
    return;
  }

  // Check variables
  if (node->type == AST_VAR) {
    Symbol *sym = find_symbol(node->as.var_name);

    // Check if variable was assigned earlier in this scope
    bool assigned_in_scope = false;
    if (seen_vars) {
      for (size_t j = 0; j < seen_count; j++) {
        if (strcmp(seen_vars[j].name, node->as.var_name) == 0) {
          assigned_in_scope = true;
          break;
        }
      }
    }

    if (sym &&
        (sym->type == SYMBOL_VARIABLE || sym->type == SYMBOL_PARAMETER)) {
      // Mark variable as read (used in expression)
      sym->used = true;
      sym->read = true;
    } else if (assigned_in_scope) {
      // Variable is assigned in this scope, so it's not undefined
      // (this handles forward references within the same scope)
    } else if (!sym || (sym->type != SYMBOL_VARIABLE &&
                        sym->type != SYMBOL_PARAMETER)) {
      // Check if it's a built-in constant or keyword
      if (strcmp(node->as.var_name, "Pi") != 0 &&
          strcmp(node->as.var_name, "undefined") != 0) {
        size_t line = 1, col = 0;
        char pattern[256];
        snprintf(pattern, sizeof(pattern), "%s", node->as.var_name);
        find_node_position(node, text, pattern, &line, &col);

        char escaped_msg[512];
        snprintf(escaped_msg, sizeof(escaped_msg), "Undefined variable '%s'",
                 node->as.var_name);
        char escaped_msg_final[512];
        json_escape(escaped_msg, escaped_msg_final, sizeof(escaped_msg_final));

        int written =
            snprintf(diagnostics + *pos, *remaining,
                     "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
                     "\"end\":{\"line\":%zu,\"character\":%zu}},"
                     "\"severity\":1,"
                     "\"message\":\"%s\"}",
                     *has_diagnostics ? "," : "", line - 1, col, line - 1,
                     col + strlen(node->as.var_name), escaped_msg_final);
        if (written > 0 && (size_t)written < *remaining) {
          *pos += (size_t)written;
          *remaining -= (size_t)written;
          *has_diagnostics = true;
        }
      }
    }
    return;
  }

  // Check lists recursively
  if (node->type == AST_LIST) {
    for (size_t i = 0; i < node->as.list.element_count; i++) {
      check_expression(node->as.list.elements[i], text, symbols, ast,
                       diagnostics, pos, remaining, has_diagnostics, seen_vars,
                       seen_count);
    }
    return;
  }

  // Check function calls recursively
  if (node->type == AST_CALL) {
    for (size_t i = 0; i < node->as.call.arg_count; i++) {
      check_expression(node->as.call.args[i], text, symbols, ast, diagnostics,
                       pos, remaining, has_diagnostics, seen_vars, seen_count);
    }
    return;
  }
}

static void check_undefined_variables(AST *ast, const char *text,
                                      Symbol *symbols, char *diagnostics,
                                      size_t *pos, size_t *remaining,
                                      bool *has_diagnostics) {
  if (!ast || !ast->statements)
    return;

  // Track variables we've seen assigned (for immutable reassignment check)
  // Use the global SeenVar type defined above
  SeenVar *seen_vars = NULL;
  size_t seen_count = 0;
  size_t seen_capacity = 16;
  seen_vars = malloc(seen_capacity * sizeof(SeenVar));
  if (!seen_vars)
    return;

  for (size_t i = 0; i < ast->count; i++) {
    ASTNode *node = ast->statements[i];
    if (!node)
      continue;

    // Check variable usage
    if (node->type == AST_VAR) {
      Symbol *sym = find_symbol(node->as.var_name);

      // Check if variable was assigned earlier in this scope
      bool assigned_in_scope = false;
      for (size_t j = 0; j < seen_count; j++) {
        if (strcmp(seen_vars[j].name, node->as.var_name) == 0 &&
            seen_vars[j].first_statement_index < i) {
          assigned_in_scope = true;
          break;
        }
      }

      if (sym &&
          (sym->type == SYMBOL_VARIABLE || sym->type == SYMBOL_PARAMETER)) {
        // Mark variable as read (used in expression)
        sym->used = true;
        sym->read = true;
      } else if (assigned_in_scope) {
        // Variable is assigned later in this scope, so it's not undefined
        // (this handles forward references within the same scope)
      } else if (!sym || (sym->type != SYMBOL_VARIABLE &&
                          sym->type != SYMBOL_PARAMETER)) {
        // Check if it's a built-in constant or keyword
        if (strcmp(node->as.var_name, "Pi") != 0 &&
            strcmp(node->as.var_name, "undefined") != 0) {
          size_t line = 1, col = 0;
          char pattern[256];
          snprintf(pattern, sizeof(pattern), "%s", node->as.var_name);
          find_node_position(node, text, pattern, &line, &col);

          char escaped_msg[512];
          snprintf(escaped_msg, sizeof(escaped_msg), "Undefined variable '%s'",
                   node->as.var_name);
          char escaped_msg_final[512];
          json_escape(escaped_msg, escaped_msg_final,
                      sizeof(escaped_msg_final));

          int written = snprintf(
              diagnostics + *pos, *remaining,
              "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
              "\"end\":{\"line\":%zu,\"character\":%zu}},"
              "\"severity\":1,"
              "\"message\":\"%s\"}",
              *has_diagnostics ? "," : "", line - 1, col, line - 1,
              col + strlen(node->as.var_name), escaped_msg_final);
          if (written > 0 && (size_t)written < *remaining) {
            *pos += (size_t)written;
            *remaining -= (size_t)written;
            *has_diagnostics = true;
          }
        }
      }
    }

    // Check assignments for immutable reassignment
    if (node->type == AST_ASSIGN) {
      // Add variable to seen_vars FIRST, before checking expressions
      // This allows forward references within the same scope
      bool found = false;
      for (size_t j = 0; j < seen_count; j++) {
        if (strcmp(seen_vars[j].name, node->as.assign.name) == 0) {
          found = true;
          seen_vars[j].assignment_count++;
          break;
        }
      }
      if (!found) {
        // Add new variable to seen_vars
        if (seen_count >= seen_capacity) {
          seen_capacity *= 2;
          SeenVar *new_vars =
              realloc(seen_vars, seen_capacity * sizeof(SeenVar));
          if (!new_vars) {
            // Can't expand, skip adding this variable
          } else {
            seen_vars = new_vars;
            seen_vars[seen_count].name = strdup(node->as.assign.name);
            seen_vars[seen_count].is_mutable = node->as.assign.is_mutable;
            seen_vars[seen_count].assignment_count = 1;
            seen_vars[seen_count].first_statement_index = i;
            seen_count++;
          }
        } else {
          seen_vars[seen_count].name = strdup(node->as.assign.name);
          seen_vars[seen_count].is_mutable = node->as.assign.is_mutable;
          seen_vars[seen_count].assignment_count = 1;
          seen_vars[seen_count].first_statement_index = i;
          seen_count++;
        }
      }

      // Mark variable as written to (assignment)
      Symbol *assign_sym = find_symbol(node->as.assign.name);
      if (assign_sym && assign_sym->type == SYMBOL_VARIABLE) {
        assign_sym->used = true; // Keep for backward compatibility
        assign_sym->written = true;
      }

      // Check if this is Pi (always immutable)
      if (strcmp(node->as.assign.name, "Pi") == 0) {
        size_t line = 1, col = 0;
        char pattern[256];
        snprintf(pattern, sizeof(pattern), "set %s to", node->as.assign.name);
        find_node_position(node, text, pattern, &line, &col);

        char escaped_msg[512];
        snprintf(escaped_msg, sizeof(escaped_msg),
                 "Cannot reassign immutable variable '%s'",
                 node->as.assign.name);
        char escaped_msg_final[512];
        json_escape(escaped_msg, escaped_msg_final, sizeof(escaped_msg_final));

        int written =
            snprintf(diagnostics + *pos, *remaining,
                     "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
                     "\"end\":{\"line\":%zu,\"character\":%zu}},"
                     "\"severity\":1,"
                     "\"message\":\"%s\"}",
                     *has_diagnostics ? "," : "", line - 1, col, line - 1,
                     col + 20, escaped_msg_final);
        if (written > 0 && (size_t)written < *remaining) {
          *pos += (size_t)written;
          *remaining -= (size_t)written;
          *has_diagnostics = true;
        }
      } else {
        // Check if variable was already assigned (reassignment check)
        // Note: Variable was already added to seen_vars above, so we just need
        // to check
        bool found = false;
        bool was_immutable = false;
        size_t occurrence = 0;
        for (size_t j = 0; j < seen_count; j++) {
          if (strcmp(seen_vars[j].name, node->as.assign.name) == 0) {
            found = true;
            was_immutable = !seen_vars[j].is_mutable;
            // Get the occurrence number BEFORE incrementing
            occurrence = seen_vars[j].assignment_count;
            break;
          }
        }

        // If variable was seen before and was immutable, this is an error
        if (found && was_immutable) {
          // Find the position of this specific assignment (the Nth occurrence)
          size_t line = 1, col = 0;
          if (!find_nth_occurrence(text, node->as.assign.name, occurrence,
                                   &line, &col)) {
            // Fallback: if we can't find the exact occurrence, use a simple
            // search This shouldn't happen, but provides a fallback
            char pattern[256];
            snprintf(pattern, sizeof(pattern), "set %s to",
                     node->as.assign.name);
            find_node_position(node, text, pattern, &line, &col);
          }

          char escaped_msg[512];
          snprintf(escaped_msg, sizeof(escaped_msg),
                   "Cannot reassign immutable variable '%s'",
                   node->as.assign.name);
          char escaped_msg_final[512];
          json_escape(escaped_msg, escaped_msg_final,
                      sizeof(escaped_msg_final));

          int written = snprintf(
              diagnostics + *pos, *remaining,
              "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
              "\"end\":{\"line\":%zu,\"character\":%zu}},"
              "\"severity\":1,"
              "\"message\":\"%s\"}",
              *has_diagnostics ? "," : "", line - 1, col, line - 1, col + 20,
              escaped_msg_final);
          if (written > 0 && (size_t)written < *remaining) {
            *pos += (size_t)written;
            *remaining -= (size_t)written;
            *has_diagnostics = true;
          }
        }

        // Check for type-annotated variables initialized with null/undefined
        // Numbers must have a value (cannot be null/undefined)
        // Strings and lists can be null/undefined (can be empty string/list
        // later)
        if (!found && node->as.assign.value &&
            node->as.assign.value->type == AST_NULL) {
          Symbol *sym = find_symbol(node->as.assign.name);
          if (sym && sym->type_name && strcmp(sym->type_name, "number") == 0) {
            // Find the position of the null/undefined value
            size_t line = 1, col = 0, value_length = 0;
            const char *null_type = "null";
            if (!find_assignment_value_position(text, node->as.assign.name, 1,
                                                node->as.assign.value, &line,
                                                &col, &value_length)) {
              // Fallback: find the assignment and use a default range
              if (!find_nth_occurrence(text, node->as.assign.name, 1, &line,
                                       &col)) {
                char pattern[256];
                snprintf(pattern, sizeof(pattern), "let %s to",
                         node->as.assign.name);
                find_node_position(node, text, pattern, &line, &col);
                if (line == 1 && col == 0) {
                  snprintf(pattern, sizeof(pattern), "set %s to",
                           node->as.assign.name);
                  find_node_position(node, text, pattern, &line, &col);
                }
              }
              // Try to determine if it's "null" or "undefined" from source
              // Find the line in the text
              const char *line_start = text;
              size_t current_line = 1;
              for (const char *p = text; *p != '\0' && current_line < line;
                   p++) {
                if (*p == '\n') {
                  current_line++;
                  if (current_line == line) {
                    line_start = p + 1;
                    break;
                  }
                }
              }
              // Find "to" keyword after the variable name
              const char *to_pos = line_start + col;
              while (*to_pos != '\0' && *to_pos != '\n' &&
                     strncmp(to_pos, "to", 2) != 0) {
                to_pos++;
              }
              if (*to_pos != '\0' && *to_pos != '\n') {
                to_pos += 2;
                while (*to_pos == ' ' || *to_pos == '\t') {
                  to_pos++;
                }
                // Check if it's "undefined"
                if (strncmp(to_pos, "undefined", 9) == 0) {
                  null_type = "undefined";
                  value_length = 9;
                } else {
                  value_length = 4; // "null"
                }
              } else {
                value_length = 4; // Default to "null"
              }
            } else {
              // Check the source text to determine if it's "null" or
              // "undefined"
              const char *line_start = text;
              size_t current_line = 1;
              for (const char *p = text; *p != '\0' && current_line < line;
                   p++) {
                if (*p == '\n') {
                  current_line++;
                  if (current_line == line) {
                    line_start = p + 1;
                    break;
                  }
                }
              }
              if (line_start + col < text + strlen(text)) {
                const char *value_start = line_start + col;
                if (strncmp(value_start, "undefined", 9) == 0) {
                  null_type = "undefined";
                }
              }
            }

            char escaped_msg[512];
            snprintf(escaped_msg, sizeof(escaped_msg),
                     "Type mismatch for variable '%s': expected 'number', got "
                     "'%s'",
                     node->as.assign.name, null_type);
            char escaped_msg_final[512];
            json_escape(escaped_msg, escaped_msg_final,
                        sizeof(escaped_msg_final));

            int written = snprintf(
                diagnostics + *pos, *remaining,
                "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
                "\"end\":{\"line\":%zu,\"character\":%zu}},"
                "\"severity\":1,"
                "\"message\":\"%s\"}",
                *has_diagnostics ? "," : "", line - 1, col, line - 1,
                col + value_length, escaped_msg_final);
            if (written > 0 && (size_t)written < *remaining) {
              *pos += (size_t)written;
              *remaining -= (size_t)written;
              *has_diagnostics = true;
            }
          }
        }

        // Check for type mismatch if this is a reassignment and variable has an
        // explicit type annotation (e.g., "as number")
        // Variables initialized with null or no value can be reassigned to any
        // type
        if (found && node->as.assign.value) {
          Symbol *sym = find_symbol(node->as.assign.name);
          if (sym && sym->type_name) {
            // Variable has an explicit type annotation - check if the new value
            // matches
            const char *expected_type = sym->type_name;
            const char *actual_type = NULL;

            // Infer type from the assigned value
            switch (node->as.assign.value->type) {
            case AST_NUMBER:
              actual_type = "number";
              break;
            case AST_STRING:
            case AST_FSTRING:
              actual_type = "string";
              break;
            case AST_BOOL:
              actual_type = "bool";
              break;
            case AST_LIST:
              actual_type = "list";
              break;
            case AST_NULL:
              actual_type = "null";
              break;
            case AST_VAR: {
              // For variables, try to infer from symbol table
              Symbol *val_sym = find_symbol(node->as.assign.value->as.var_name);
              if (val_sym && val_sym->type_name) {
                actual_type = val_sym->type_name;
              }
              break;
            }
            default:
              // Unknown type - skip check
              break;
            }

            // If we have both expected and actual types, check for mismatch
            if (actual_type && strcmp(expected_type, actual_type) != 0) {
              // Find the position of the problematic value (not the entire
              // assignment) Since we're processing AST nodes in order, we can
              // find all assignments of this variable and use the current
              // statement index to identify the right one
              size_t line = 1, col = 0, value_length = 0;

              // Count how many assignments of this variable come before the
              // current one
              size_t assignment_index = 0;
              for (size_t k = 0; k < i; k++) {
                ASTNode *prev_node = ast->statements[k];
                if (prev_node && prev_node->type == AST_ASSIGN &&
                    strcmp(prev_node->as.assign.name, node->as.assign.name) ==
                        0) {
                  assignment_index++;
                }
              }
              // The current assignment is the (assignment_index + 1)th
              // occurrence
              size_t target_occurrence = assignment_index + 1;

              // Try to find the exact position of the value
              if (!find_assignment_value_position(
                      text, node->as.assign.name, target_occurrence,
                      node->as.assign.value, &line, &col, &value_length)) {
                // Fallback: find the assignment and use a default range
                if (!find_nth_occurrence(text, node->as.assign.name,
                                         target_occurrence, &line, &col)) {
                  char pattern[256];
                  snprintf(pattern, sizeof(pattern), "let %s to",
                           node->as.assign.name);
                  find_node_position(node, text, pattern, &line, &col);
                  if (line == 1 && col == 0) {
                    snprintf(pattern, sizeof(pattern), "set %s to",
                             node->as.assign.name);
                    find_node_position(node, text, pattern, &line, &col);
                  }
                }
                // Default to underlining the variable name if we can't find the
                // value
                value_length = strlen(node->as.assign.name);
              }

              char escaped_msg[512];
              snprintf(
                  escaped_msg, sizeof(escaped_msg),
                  "Type mismatch for variable '%s': expected '%s', got '%s'",
                  node->as.assign.name, expected_type, actual_type);
              char escaped_msg_final[512];
              json_escape(escaped_msg, escaped_msg_final,
                          sizeof(escaped_msg_final));

              int written = snprintf(
                  diagnostics + *pos, *remaining,
                  "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
                  "\"end\":{\"line\":%zu,\"character\":%zu}},"
                  "\"severity\":1,"
                  "\"message\":\"%s\"}",
                  *has_diagnostics ? "," : "", line - 1, col, line - 1,
                  col + value_length, escaped_msg_final);
              if (written > 0 && (size_t)written < *remaining) {
                *pos += (size_t)written;
                *remaining -= (size_t)written;
                *has_diagnostics = true;
              }
            }
          }
        }

        // Note: Variable already added to seen_vars at the start of AST_ASSIGN
        // handling (lines 2302-2333), no need to add again here
      }
      // Check expressions in assignment value
      if (node->as.assign.value) {
        check_expression(node->as.assign.value, text, symbols, ast, diagnostics,
                         pos, remaining, has_diagnostics, seen_vars,
                         seen_count);
      }
    }

    // Check expressions in print statements
    if (node->type == AST_PRINT) {
      if (node->as.print.value) {
        check_expression(node->as.print.value, text, symbols, ast, diagnostics,
                         pos, remaining, has_diagnostics, seen_vars,
                         seen_count);
      }
    }

    // Check expressions in if conditions
    if (node->type == AST_IF) {
      if (node->as.if_stmt.condition) {
        check_expression(node->as.if_stmt.condition, text, symbols, ast,
                         diagnostics, pos, remaining, has_diagnostics,
                         seen_vars, seen_count);
      }
      // Check else-if conditions
      for (size_t j = 0; j < node->as.if_stmt.else_if_count; j++) {
        if (node->as.if_stmt.else_if_conditions[j]) {
          check_expression(node->as.if_stmt.else_if_conditions[j], text,
                           symbols, ast, diagnostics, pos, remaining,
                           has_diagnostics, seen_vars, seen_count);
        }
      }
    }

    // Check expressions in for loops
    if (node->type == AST_FOR) {
      // Mark loop variable as written (assigned by the loop)
      if (node->as.for_stmt.var) {
        Symbol *loop_sym = find_symbol(node->as.for_stmt.var);
        if (loop_sym && loop_sym->type == SYMBOL_VARIABLE) {
          loop_sym->written = true;
        }
      }
      if (node->as.for_stmt.iterable) {
        check_expression(node->as.for_stmt.iterable, text, symbols, ast,
                         diagnostics, pos, remaining, has_diagnostics,
                         seen_vars, seen_count);
      }
      if (node->as.for_stmt.end) {
        check_expression(node->as.for_stmt.end, text, symbols, ast, diagnostics,
                         pos, remaining, has_diagnostics, seen_vars,
                         seen_count);
      }
      if (node->as.for_stmt.step) {
        check_expression(node->as.for_stmt.step, text, symbols, ast,
                         diagnostics, pos, remaining, has_diagnostics,
                         seen_vars, seen_count);
      }
    }

    // Check expressions in while loops
    if (node->type == AST_WHILE) {
      if (node->as.while_stmt.condition) {
        check_expression(node->as.while_stmt.condition, text, symbols, ast,
                         diagnostics, pos, remaining, has_diagnostics,
                         seen_vars, seen_count);
      }
    }

    // Check expressions in return statements
    if (node->type == AST_RETURN) {
      if (node->as.return_stmt.value) {
        check_expression(node->as.return_stmt.value, text, symbols, ast,
                         diagnostics, pos, remaining, has_diagnostics,
                         seen_vars, seen_count);
      }
    }

    // Check expressions in index assignments
    if (node->type == AST_ASSIGN_INDEX) {
      // Check target, index, and value expressions
      if (node->as.assign_index.target) {
        check_expression(node->as.assign_index.target, text, symbols, ast,
                         diagnostics, pos, remaining, has_diagnostics,
                         seen_vars, seen_count);
      }
      if (node->as.assign_index.index) {
        check_expression(node->as.assign_index.index, text, symbols, ast,
                         diagnostics, pos, remaining, has_diagnostics,
                         seen_vars, seen_count);
      }
      if (node->as.assign_index.value) {
        check_expression(node->as.assign_index.value, text, symbols, ast,
                         diagnostics, pos, remaining, has_diagnostics,
                         seen_vars, seen_count);
      }
    }

    // Check expressions in delete statements
    if (node->type == AST_DELETE) {
      // Check target and key expressions
      if (node->as.delete_stmt.target) {
        check_expression(node->as.delete_stmt.target, text, symbols, ast,
                         diagnostics, pos, remaining, has_diagnostics,
                         seen_vars, seen_count);
      }
      if (node->as.delete_stmt.key) {
        check_expression(node->as.delete_stmt.key, text, symbols, ast,
                         diagnostics, pos, remaining, has_diagnostics,
                         seen_vars, seen_count);
      }
    }

    // Check expressions in try/catch/finally blocks
    if (node->type == AST_TRY) {
      // Check expressions in try block
      if (node->as.try_stmt.try_block) {
        AST try_ast = {node->as.try_stmt.try_block,
                       node->as.try_stmt.try_block_size,
                       node->as.try_stmt.try_block_size};
        check_undefined_variables(&try_ast, text, symbols, diagnostics, pos,
                                  remaining, has_diagnostics);
      }
      // Check expressions in catch blocks
      for (size_t j = 0; j < node->as.try_stmt.catch_block_count; j++) {
        if (node->as.try_stmt.catch_blocks[j].catch_block) {
          AST catch_ast = {node->as.try_stmt.catch_blocks[j].catch_block,
                           node->as.try_stmt.catch_blocks[j].catch_block_size,
                           node->as.try_stmt.catch_blocks[j].catch_block_size};
          check_undefined_variables(&catch_ast, text, symbols, diagnostics, pos,
                                    remaining, has_diagnostics);
        }
      }
      // Check expressions in finally block
      if (node->as.try_stmt.finally_block) {
        AST finally_ast = {node->as.try_stmt.finally_block,
                           node->as.try_stmt.finally_block_size,
                           node->as.try_stmt.finally_block_size};
        check_undefined_variables(&finally_ast, text, symbols, diagnostics, pos,
                                  remaining, has_diagnostics);
      }
    }

    // Check expressions in raise statements
    if (node->type == AST_RAISE) {
      // Check error message expression
      if (node->as.raise_stmt.message) {
        check_expression(node->as.raise_stmt.message, text, symbols, ast,
                         diagnostics, pos, remaining, has_diagnostics,
                         seen_vars, seen_count);
      }
    }

    // Check function calls for type errors in arguments
    if (node->type == AST_CALL) {
      const char *func_name = node->as.call.name;
      const char *dot = strchr(func_name, '.');
      const char *actual_func_name = func_name;
      if (dot) {
        size_t module_len = (size_t)(dot - func_name);
        char *module_name = malloc(module_len + 1);
        if (module_name) {
          strncpy(module_name, func_name, module_len);
          module_name[module_len] = '\0';
          if (strcmp(module_name, "math") == 0) {
            actual_func_name = dot + 1;
          } else if (is_module_imported(module_name)) {
            // File-based module - skip type checking for now
            free(module_name);
            continue;
          }
          free(module_name);
        }
      } else {
        // Mark function as used if it's a user-defined function
        Symbol *func_sym = find_symbol(func_name);
        if (func_sym && func_sym->type == SYMBOL_FUNCTION) {
          func_sym->used = true;
        }
      }

      // Check built-in function argument types
      if (strcmp(actual_func_name, "add") == 0 ||
          strcmp(actual_func_name, "subtract") == 0 ||
          strcmp(actual_func_name, "multiply") == 0 ||
          strcmp(actual_func_name, "divide") == 0 ||
          strcmp(actual_func_name, "power") == 0) {
        // These require number arguments
        for (size_t j = 0; j < node->as.call.arg_count && j < 2; j++) {
          ExprType arg_type =
              infer_type_with_ast(node->as.call.args[j], symbols, ast);
          if (arg_type != TYPE_NUMBER && arg_type != TYPE_UNKNOWN) {
            size_t line = 1, col = 0;
            find_call_position(text, func_name, &line, &col);

            char escaped_msg[512];
            snprintf(escaped_msg, sizeof(escaped_msg),
                     "Function '%s' requires both arguments to be numbers",
                     func_name);
            char escaped_msg_final[512];
            json_escape(escaped_msg, escaped_msg_final,
                        sizeof(escaped_msg_final));

            int written = snprintf(
                diagnostics + *pos, *remaining,
                "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
                "\"end\":{\"line\":%zu,\"character\":%zu}},"
                "\"severity\":1,"
                "\"message\":\"%s\"}",
                *has_diagnostics ? "," : "", line - 1, col, line - 1, col + 20,
                escaped_msg_final);
            if (written > 0 && (size_t)written < *remaining) {
              *pos += (size_t)written;
              *remaining -= (size_t)written;
              *has_diagnostics = true;
            }
            break;
          }
        }
      } else if (strcmp(actual_func_name, "sqrt") == 0 ||
                 strcmp(actual_func_name, "abs") == 0 ||
                 strcmp(actual_func_name, "round") == 0 ||
                 strcmp(actual_func_name, "floor") == 0 ||
                 strcmp(actual_func_name, "ceil") == 0) {
        // These require number argument
        if (node->as.call.arg_count > 0) {
          ExprType arg_type =
              infer_type_with_ast(node->as.call.args[0], symbols, ast);
          if (arg_type != TYPE_NUMBER && arg_type != TYPE_UNKNOWN) {
            size_t line = 1, col = 0;
            find_call_position(text, func_name, &line, &col);

            char escaped_msg[512];
            snprintf(escaped_msg, sizeof(escaped_msg),
                     "Function '%s' requires a number argument", func_name);
            char escaped_msg_final[512];
            json_escape(escaped_msg, escaped_msg_final,
                        sizeof(escaped_msg_final));

            int written = snprintf(
                diagnostics + *pos, *remaining,
                "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
                "\"end\":{\"line\":%zu,\"character\":%zu}},"
                "\"severity\":1,"
                "\"message\":\"%s\"}",
                *has_diagnostics ? "," : "", line - 1, col, line - 1, col + 20,
                escaped_msg_final);
            if (written > 0 && (size_t)written < *remaining) {
              *pos += (size_t)written;
              *remaining -= (size_t)written;
              *has_diagnostics = true;
            }
          }
        }
      } else if (strcmp(actual_func_name, "len") == 0) {
        // len accepts list, string, or range - no type error for any of these
        // No special validation needed here
        (void)node; // Suppress unused warning
      } else if (strcmp(actual_func_name, "reverse") == 0 ||
                 strcmp(actual_func_name, "sort") == 0) {
        // These require list argument (not string)
        if (node->as.call.arg_count > 0) {
          ASTNode *arg_node = node->as.call.args[0];
          (void)infer_type_with_ast(arg_node, symbols,
                                    ast); // Type inference for future use

          // Check for string literals directly
          if (arg_node->type == AST_STRING || arg_node->type == AST_FSTRING) {
            // Explicitly check for string - reverse and sort don't accept
            // strings
            size_t line = 1, col = 0;
            find_call_position(text, func_name, &line, &col);

            char escaped_msg[512];
            snprintf(escaped_msg, sizeof(escaped_msg),
                     "Function '%s' requires a list argument", func_name);
            char escaped_msg_final[512];
            json_escape(escaped_msg, escaped_msg_final,
                        sizeof(escaped_msg_final));

            int written = snprintf(
                diagnostics + *pos, *remaining,
                "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
                "\"end\":{\"line\":%zu,\"character\":%zu}},"
                "\"severity\":1,"
                "\"message\":\"%s\"}",
                *has_diagnostics ? "," : "", line - 1, col, line - 1, col + 20,
                escaped_msg_final);
            if (written > 0 && (size_t)written < *remaining) {
              *pos += (size_t)written;
              *remaining -= (size_t)written;
              *has_diagnostics = true;
            }
          }
          if (strcmp(actual_func_name, "sort") == 0) {
            // Check if sort list has mixed types (all must be numbers or all
            // strings)
            ASTNode *list_node = NULL;

            // Check if argument is a list literal
            if (arg_node->type == AST_LIST) {
              list_node = arg_node;
            } else if (arg_node->type == AST_VAR) {
              // Find where this variable was assigned
              // Look backwards in AST to find the most recent assignment before
              // this call
              if (ast && ast->statements) {
                // Find the index of the current node
                size_t current_idx = 0;
                for (size_t k = 0; k < ast->count; k++) {
                  if (ast->statements[k] == node) {
                    current_idx = k;
                    break;
                  }
                }
                // Look backwards from current node to find the assignment
                for (size_t k = 0; k < current_idx; k++) {
                  ASTNode *stmt = ast->statements[k];
                  if (stmt && stmt->type == AST_ASSIGN &&
                      strcmp(stmt->as.assign.name, arg_node->as.var_name) ==
                          0) {
                    // Found the assignment - check if it's a list literal
                    if (stmt->as.assign.value &&
                        stmt->as.assign.value->type == AST_LIST) {
                      list_node = stmt->as.assign.value;
                      // Don't break - we want the most recent assignment, so
                      // keep going
                    }
                  }
                }
              }
            }

            if (list_node && list_node->as.list.element_count > 0) {
              ExprType first_type = TYPE_UNKNOWN;
              bool has_mixed_types = false;

              for (size_t j = 0; j < list_node->as.list.element_count; j++) {
                if (list_node->as.list.elements[j]) {
                  ExprType elem_type = infer_type_with_ast(
                      list_node->as.list.elements[j], symbols, ast);
                  if (elem_type != TYPE_UNKNOWN) {
                    if (first_type == TYPE_UNKNOWN) {
                      first_type = elem_type;
                    } else if (elem_type != first_type) {
                      has_mixed_types = true;
                      break;
                    }
                  }
                }
              }

              // Sort requires all numbers or all strings, not mixed
              if (has_mixed_types ||
                  (first_type != TYPE_UNKNOWN && first_type != TYPE_NUMBER &&
                   first_type != TYPE_STRING)) {
                size_t line = 1, col = 0;
                find_call_position(text, func_name, &line, &col);

                char escaped_msg[512];
                snprintf(escaped_msg, sizeof(escaped_msg),
                         "Function 'sort' requires list items to be all "
                         "numbers or all strings");
                char escaped_msg_final[512];
                json_escape(escaped_msg, escaped_msg_final,
                            sizeof(escaped_msg_final));

                int written = snprintf(
                    diagnostics + *pos, *remaining,
                    "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
                    "\"end\":{\"line\":%zu,\"character\":%zu}},"
                    "\"severity\":1,"
                    "\"message\":\"%s\"}",
                    *has_diagnostics ? "," : "", line - 1, col, line - 1,
                    col + 20, escaped_msg_final);
                if (written > 0 && (size_t)written < *remaining) {
                  *pos += (size_t)written;
                  *remaining -= (size_t)written;
                  *has_diagnostics = true;
                }
              }
            }
          }
        }
      } else if (strcmp(actual_func_name, "uppercase") == 0 ||
                 strcmp(actual_func_name, "lowercase") == 0 ||
                 strcmp(actual_func_name, "trim") == 0) {
        // These require string argument
        if (node->as.call.arg_count > 0) {
          ExprType arg_type =
              infer_type_with_ast(node->as.call.args[0], symbols, ast);
          if (arg_type != TYPE_STRING && arg_type != TYPE_UNKNOWN) {
            size_t line = 1, col = 0;
            find_call_position(text, func_name, &line, &col);

            char escaped_msg[512];
            snprintf(escaped_msg, sizeof(escaped_msg),
                     "Function '%s' requires a string argument", func_name);
            char escaped_msg_final[512];
            json_escape(escaped_msg, escaped_msg_final,
                        sizeof(escaped_msg_final));

            int written = snprintf(
                diagnostics + *pos, *remaining,
                "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
                "\"end\":{\"line\":%zu,\"character\":%zu}},"
                "\"severity\":1,"
                "\"message\":\"%s\"}",
                *has_diagnostics ? "," : "", line - 1, col, line - 1, col + 20,
                escaped_msg_final);
            if (written > 0 && (size_t)written < *remaining) {
              *pos += (size_t)written;
              *remaining -= (size_t)written;
              *has_diagnostics = true;
            }
          }
        }
      } else if (strcmp(actual_func_name, "to_number") == 0) {
        // to_number requires string or number argument, not list
        if (node->as.call.arg_count > 0) {
          ASTNode *arg_node = node->as.call.args[0];
          ExprType arg_type = infer_type_with_ast(arg_node, symbols, ast);

          // Check for list type or list literal
          if (arg_type == TYPE_LIST || arg_node->type == AST_LIST) {
            size_t line = 1, col = 0, arg_length = 0;
            if (!find_call_argument_position(text, func_name, arg_node, &line,
                                             &col, &arg_length)) {
              // Fallback to function call position
              find_call_position(text, func_name, &line, &col);
              arg_length = 20;
            }

            char escaped_msg[512];
            snprintf(escaped_msg, sizeof(escaped_msg),
                     "to_number requires string or number");
            char escaped_msg_final[512];
            json_escape(escaped_msg, escaped_msg_final,
                        sizeof(escaped_msg_final));

            int written = snprintf(
                diagnostics + *pos, *remaining,
                "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
                "\"end\":{\"line\":%zu,\"character\":%zu}},"
                "\"severity\":1,"
                "\"message\":\"%s\"}",
                *has_diagnostics ? "," : "", line - 1, col, line - 1,
                col + arg_length, escaped_msg_final);
            if (written > 0 && (size_t)written < *remaining) {
              *pos += (size_t)written;
              *remaining -= (size_t)written;
              *has_diagnostics = true;
            }
          } else if (arg_node->type == AST_STRING) {
            // Check if string literal can be converted to number
            // For AST_STRING, check if the string value is a valid number
            if (arg_node->as.string.value) {
              const char *str_val = arg_node->as.string.value;
              // Skip leading whitespace
              while (*str_val == ' ' || *str_val == '\t') {
                str_val++;
              }
              // If empty after whitespace, it's not a valid number
              if (*str_val == '\0') {
                size_t line = 1, col = 0, arg_length = 0;
                if (!find_call_argument_position(text, func_name, arg_node,
                                                 &line, &col, &arg_length)) {
                  find_call_position(text, func_name, &line, &col);
                  arg_length = 20;
                }

                char escaped_msg[512];
                snprintf(escaped_msg, sizeof(escaped_msg),
                         "cannot convert string to number");
                char escaped_msg_final[512];
                json_escape(escaped_msg, escaped_msg_final,
                            sizeof(escaped_msg_final));

                int written = snprintf(
                    diagnostics + *pos, *remaining,
                    "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
                    "\"end\":{\"line\":%zu,\"character\":%zu}},"
                    "\"severity\":1,"
                    "\"message\":\"%s\"}",
                    *has_diagnostics ? "," : "", line - 1, col, line - 1,
                    col + arg_length, escaped_msg_final);
                if (written > 0 && (size_t)written < *remaining) {
                  *pos += (size_t)written;
                  *remaining -= (size_t)written;
                  *has_diagnostics = true;
                }
              } else {
                // Try to parse as number - if it fails, it's not a valid
                // numeric string
                char *endptr;
                strtod(str_val, &endptr);
                // Skip trailing whitespace
                while (*endptr == ' ' || *endptr == '\t') {
                  endptr++;
                }
                // If endptr doesn't point to the end, it's not a valid number
                if (*endptr != '\0' && *endptr != '\n' && *endptr != '\r') {
                  // Not a valid numeric string
                  size_t line = 1, col = 0, arg_length = 0;
                  if (!find_call_argument_position(text, func_name, arg_node,
                                                   &line, &col, &arg_length)) {
                    find_call_position(text, func_name, &line, &col);
                    arg_length = 20;
                  }

                  char escaped_msg[512];
                  snprintf(escaped_msg, sizeof(escaped_msg),
                           "cannot convert string to number");
                  char escaped_msg_final[512];
                  json_escape(escaped_msg, escaped_msg_final,
                              sizeof(escaped_msg_final));

                  int written =
                      snprintf(diagnostics + *pos, *remaining,
                               "%s{\"range\":{\"start\":{\"line\":%zu,"
                               "\"character\":%zu},"
                               "\"end\":{\"line\":%zu,\"character\":%zu}},"
                               "\"severity\":1,"
                               "\"message\":\"%s\"}",
                               *has_diagnostics ? "," : "", line - 1, col,
                               line - 1, col + arg_length, escaped_msg_final);
                  if (written > 0 && (size_t)written < *remaining) {
                    *pos += (size_t)written;
                    *remaining -= (size_t)written;
                    *has_diagnostics = true;
                  }
                }
              }
            }
          }
        }
      } else if (strcmp(actual_func_name, "to_bool") == 0) {
        // to_bool requires number, string, or bool argument, not list
        if (node->as.call.arg_count > 0) {
          ASTNode *arg_node = node->as.call.args[0];
          ExprType arg_type = infer_type_with_ast(arg_node, symbols, ast);

          // Check for list type or list literal
          if (arg_type == TYPE_LIST || arg_node->type == AST_LIST) {
            size_t line = 1, col = 0, arg_length = 0;
            if (!find_call_argument_position(text, func_name, arg_node, &line,
                                             &col, &arg_length)) {
              // Fallback to function call position
              find_call_position(text, func_name, &line, &col);
              arg_length = 20;
            }

            char escaped_msg[512];
            snprintf(escaped_msg, sizeof(escaped_msg),
                     "cannot convert type to boolean");
            char escaped_msg_final[512];
            json_escape(escaped_msg, escaped_msg_final,
                        sizeof(escaped_msg_final));

            int written = snprintf(
                diagnostics + *pos, *remaining,
                "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
                "\"end\":{\"line\":%zu,\"character\":%zu}},"
                "\"severity\":1,"
                "\"message\":\"%s\"}",
                *has_diagnostics ? "," : "", line - 1, col, line - 1,
                col + arg_length, escaped_msg_final);
            if (written > 0 && (size_t)written < *remaining) {
              *pos += (size_t)written;
              *remaining -= (size_t)written;
              *has_diagnostics = true;
            }
          }
        }
      }

      // Check for negative sqrt
      if (strcmp(actual_func_name, "sqrt") == 0 &&
          node->as.call.arg_count > 0) {
        double arg_val;
        if (get_constant_number(node->as.call.args[0], &arg_val) &&
            arg_val < 0) {
          size_t line = 1, col = 0;
          find_call_position(text, func_name, &line, &col);

          char escaped_msg[512] =
              "Function 'sqrt' cannot take negative argument";
          char escaped_msg_final[512];
          json_escape(escaped_msg, escaped_msg_final,
                      sizeof(escaped_msg_final));

          int written = snprintf(
              diagnostics + *pos, *remaining,
              "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
              "\"end\":{\"line\":%zu,\"character\":%zu}},"
              "\"severity\":1,"
              "\"message\":\"%s\"}",
              *has_diagnostics ? "," : "", line - 1, col, line - 1, col + 20,
              escaped_msg_final);
          if (written > 0 && (size_t)written < *remaining) {
            *pos += (size_t)written;
            *remaining -= (size_t)written;
            *has_diagnostics = true;
          }
        }
      }
    }

    // Recursively check nested structures
    if (node->type == AST_IF) {
      if (node->as.if_stmt.block) {
        AST temp_ast = {node->as.if_stmt.block, node->as.if_stmt.block_size,
                        node->as.if_stmt.block_size};
        check_undefined_variables(&temp_ast, text, symbols, diagnostics, pos,
                                  remaining, has_diagnostics);
      }
      for (size_t j = 0; j < node->as.if_stmt.else_if_count; j++) {
        if (node->as.if_stmt.else_if_blocks[j]) {
          AST temp_ast = {node->as.if_stmt.else_if_blocks[j],
                          node->as.if_stmt.else_if_block_sizes[j],
                          node->as.if_stmt.else_if_block_sizes[j]};
          check_undefined_variables(&temp_ast, text, symbols, diagnostics, pos,
                                    remaining, has_diagnostics);
        }
      }
      if (node->as.if_stmt.else_block) {
        AST temp_ast = {node->as.if_stmt.else_block,
                        node->as.if_stmt.else_block_size,
                        node->as.if_stmt.else_block_size};
        check_undefined_variables(&temp_ast, text, symbols, diagnostics, pos,
                                  remaining, has_diagnostics);
      }
    } else if (node->type == AST_FOR || node->type == AST_WHILE) {
      ASTNode **block = NULL;
      size_t block_size = 0;
      if (node->type == AST_FOR) {
        block = node->as.for_stmt.block;
        block_size = node->as.for_stmt.block_size;
      } else {
        block = node->as.while_stmt.block;
        block_size = node->as.while_stmt.block_size;
      }
      if (block) {
        AST temp_ast = {block, block_size, block_size};
        check_undefined_variables(&temp_ast, text, symbols, diagnostics, pos,
                                  remaining, has_diagnostics);
      }
    } else if (node->type == AST_FUNCTION) {
      if (node->as.function.block) {
        AST temp_ast = {node->as.function.block, node->as.function.block_size,
                        node->as.function.block_size};
        check_undefined_variables(&temp_ast, text, symbols, diagnostics, pos,
                                  remaining, has_diagnostics);
      }
    }
  }

  // Free seen_vars
  for (size_t i = 0; i < seen_count; i++) {
    free(seen_vars[i].name);
  }
  free(seen_vars);
}

// Helper function to check if a variable is a loop variable
static bool is_loop_variable(Symbol *sym, AST *ast) {
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

// Check for unused variables and functions
static void check_unused_symbols(Symbol *symbols, const char *text, AST *ast,
                                 char *diagnostics, size_t *pos,
                                 size_t *remaining, bool *has_diagnostics) {
  if (!symbols || !text)
    return;

  for (Symbol *sym = symbols; sym != NULL; sym = sym->next) {
    // Skip parameters (they're used by function calls)
    if (sym->type == SYMBOL_PARAMETER)
      continue;

    // Skip loop variables - they're always "used" by the loop itself
    if (ast && is_loop_variable(sym, ast))
      continue;

    // Check for variables defined but never read (memory waste - TypeScript
    // pattern)
    if (sym->type == SYMBOL_VARIABLE && sym->written && !sym->read) {
      // Find the actual position of the variable declaration in source text
      size_t line = 1, col = 0;
      char pattern[256];
      snprintf(pattern, sizeof(pattern), "let %s to", sym->name);
      find_node_position(NULL, text, pattern, &line, &col);

      // If "let" pattern not found, try "set"
      if (line == 1 && col == 0) {
        snprintf(pattern, sizeof(pattern), "set %s to", sym->name);
        find_node_position(NULL, text, pattern, &line, &col);
      }

      // If still not found, try "for X in" pattern (for loop variables)
      if (line == 1 && col == 0) {
        snprintf(pattern, sizeof(pattern), "for %s in", sym->name);
        find_node_position(NULL, text, pattern, &line, &col);
        if (line > 1 || col > 0) {
          // Found "for X in" - skip this variable (it's a loop variable)
          continue;
        }
      }

      // If still not found, try to find just the variable name (fallback)
      if (line == 1 && col == 0) {
        find_nth_occurrence(text, sym->name, 1, &line, &col);
      }

      char escaped_msg[512];
      snprintf(escaped_msg, sizeof(escaped_msg),
               "Variable '%s' is defined but never read (memory allocation not "
               "utilized)",
               sym->name);
      char escaped_msg_final[512];
      json_escape(escaped_msg, escaped_msg_final, sizeof(escaped_msg_final));

      // Calculate the column position of the variable name itself
      // Pattern could be "let <name> to", "set <name> to", or "for <name> in"
      size_t var_col = col;
      if (line > 1 || col > 0) {
        // Find the actual position of the variable name in the line
        const char *line_start = text;
        size_t current_line = 1;
        for (const char *p = text; *p != '\0' && current_line < line; p++) {
          if (*p == '\n') {
            current_line++;
            if (current_line == line) {
              line_start = p + 1;
              break;
            }
          }
        }
        const char *pattern_start = line_start + col;
        const char *var_start = pattern_start;

        // Check if this is a "for X in" pattern
        if (strncmp(pattern_start, "for ", 4) == 0) {
          // Skip "for " to get to variable name
          var_start = pattern_start + 4;
          // Skip whitespace after "for"
          while (*var_start == ' ' || *var_start == '\t') {
            var_start++;
          }
        } else {
          // Pattern is "let <name> to" or "set <name> to"
          // Skip "let " or "set "
          while (*var_start != '\0' && *var_start != '\n' &&
                 (*var_start == ' ' || *var_start == '\t' ||
                  (*var_start >= 'a' && *var_start <= 'z') ||
                  (*var_start >= 'A' && *var_start <= 'Z'))) {
            if (*var_start == ' ') {
              var_start++;
              break;
            }
            var_start++;
          }
          // Skip whitespace after "let"/"set"
          while (*var_start == ' ' || *var_start == '\t') {
            var_start++;
          }
        }
        var_col = (size_t)(var_start - line_start);
      }

      int written =
          snprintf(diagnostics + *pos, *remaining,
                   "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
                   "\"end\":{\"line\":%zu,\"character\":%zu}},"
                   "\"severity\":1,"
                   "\"message\":\"%s\"}",
                   *has_diagnostics ? "," : "", line - 1, var_col, line - 1,
                   var_col + strlen(sym->name), escaped_msg_final);
      if (written > 0 && (size_t)written < *remaining) {
        *pos += (size_t)written;
        *remaining -= (size_t)written;
        *has_diagnostics = true;
      }
    }

    // Check for completely unused variables and functions (not written or read)
    // Skip variables that are written (they're used, just not read - already
    // reported above)
    if (!sym->used &&
        (sym->type == SYMBOL_VARIABLE || sym->type == SYMBOL_FUNCTION) &&
        !(sym->type == SYMBOL_VARIABLE && sym->written)) {
      // Find the actual position of the symbol in source text
      size_t line = 1, col = 0;

      if (sym->type == SYMBOL_FUNCTION) {
        // Find function declaration: "function <name> with"
        char pattern[256];
        snprintf(pattern, sizeof(pattern), "function %s with", sym->name);
        find_node_position(NULL, text, pattern, &line, &col);
        // If not found, try without "with" (function might not have parameters)
        if (line == 1 && col == 0) {
          snprintf(pattern, sizeof(pattern), "function %s", sym->name);
          find_node_position(NULL, text, pattern, &line, &col);
        }
      } else {
        // Find variable declaration: "let <name> to" or "set <name> to"
        char pattern[256];
        snprintf(pattern, sizeof(pattern), "let %s to", sym->name);
        find_node_position(NULL, text, pattern, &line, &col);
        if (line == 1 && col == 0) {
          snprintf(pattern, sizeof(pattern), "set %s to", sym->name);
          find_node_position(NULL, text, pattern, &line, &col);
        }
      }

      // If still not found, try to find just the name (fallback)
      if (line == 1 && col == 0) {
        find_nth_occurrence(text, sym->name, 1, &line, &col);
      }

      // Calculate the column position of the symbol name itself
      size_t name_col = col;
      if (line > 1 || col > 0) {
        const char *line_start = text;
        size_t current_line = 1;
        for (const char *p = text; *p != '\0' && current_line < line; p++) {
          if (*p == '\n') {
            current_line++;
            if (current_line == line) {
              line_start = p + 1;
              break;
            }
          }
        }

        if (sym->type == SYMBOL_FUNCTION) {
          // Skip "function " to get to function name
          const char *pattern_start = line_start + col;
          const char *name_start = pattern_start;
          // Skip "function "
          while (*name_start != '\0' && *name_start != '\n' &&
                 (*name_start == ' ' || *name_start == '\t' ||
                  (*name_start >= 'a' && *name_start <= 'z') ||
                  (*name_start >= 'A' && *name_start <= 'Z'))) {
            if (*name_start == ' ') {
              name_start++;
              break;
            }
            name_start++;
          }
          while (*name_start == ' ' || *name_start == '\t') {
            name_start++;
          }
          name_col = (size_t)(name_start - line_start);
        } else {
          // Skip "let " or "set " to get to variable name
          const char *pattern_start = line_start + col;
          const char *name_start = pattern_start;
          while (*name_start != '\0' && *name_start != '\n' &&
                 (*name_start == ' ' || *name_start == '\t' ||
                  (*name_start >= 'a' && *name_start <= 'z') ||
                  (*name_start >= 'A' && *name_start <= 'Z'))) {
            if (*name_start == ' ') {
              name_start++;
              break;
            }
            name_start++;
          }
          while (*name_start == ' ' || *name_start == '\t') {
            name_start++;
          }
          name_col = (size_t)(name_start - line_start);
        }
      }

      const char *symbol_type =
          sym->type == SYMBOL_FUNCTION ? "function" : "variable";
      char escaped_msg[512];
      snprintf(escaped_msg, sizeof(escaped_msg), "Unused %s '%s'", symbol_type,
               sym->name);
      char escaped_msg_final[512];
      json_escape(escaped_msg, escaped_msg_final, sizeof(escaped_msg_final));

      int written =
          snprintf(diagnostics + *pos, *remaining,
                   "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
                   "\"end\":{\"line\":%zu,\"character\":%zu}},"
                   "\"severity\":2,"
                   "\"message\":\"%s\"}",
                   *has_diagnostics ? "," : "", line - 1, name_col, line - 1,
                   name_col + strlen(sym->name), escaped_msg_final);
      if (written > 0 && (size_t)written < *remaining) {
        *pos += (size_t)written;
        *remaining -= (size_t)written;
        *has_diagnostics = true;
      }
    }
  }
}

static void check_diagnostics(const char *uri, const char *text) {
  TokenizeError *tokenize_err = NULL;
  TokenArray *tokens = tokenize(text, &tokenize_err);

  char diagnostics[8192];
  size_t pos = 0;
  size_t remaining = sizeof(diagnostics);

  // Start building JSON
  int written = snprintf(diagnostics + pos, remaining,
                         "{\"uri\":\"%s\",\"diagnostics\":[", uri);
  if (written < 0 || (size_t)written >= remaining) {
    if (tokenize_err)
      tokenize_error_free(tokenize_err);
    return;
  }
  pos += (size_t)written;
  remaining -= (size_t)written;

  bool has_diagnostics = false;

  // Check tokenization errors
  if (tokenize_err) {
    size_t line = tokenize_err->line > 0 ? tokenize_err->line - 1 : 0;
    size_t col = tokenize_err->column > 0 ? tokenize_err->column - 1 : 0;

    char escaped_msg[512];
    json_escape(tokenize_err->message, escaped_msg, sizeof(escaped_msg));

    written = snprintf(
        diagnostics + pos, remaining,
        "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
        "\"end\":{\"line\":%zu,\"character\":%zu}},"
        "\"severity\":1,"
        "\"message\":\"%s\"}",
        has_diagnostics ? "," : "", line, col, line, col + 1, escaped_msg);
    if (written > 0 && (size_t)written < remaining) {
      pos += (size_t)written;
      remaining -= (size_t)written;
      has_diagnostics = true;
    }
    tokenize_error_free(tokenize_err);
  }

  // Check parsing errors and analyze AST
  AST *ast = NULL;
  if (tokens) {
    ast = parse(tokens);
    if (!ast || ast->count == 0) {
      // Parse error - use first token position as estimate
      if (tokens->count > 0) {
        size_t line = 1, col = 1;
        // Estimate position (tokens don't store line/column, so we estimate)
        written = snprintf(
            diagnostics + pos, remaining,
            "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
            "\"end\":{\"line\":%zu,\"character\":%zu}},"
            "\"severity\":1,"
            "\"message\":\"Syntax error: failed to parse\"}",
            has_diagnostics ? "," : "", line - 1, col - 1, line - 1, col);
        if (written > 0 && (size_t)written < remaining) {
          pos += (size_t)written;
          remaining -= (size_t)written;
          has_diagnostics = true;
        }
      }
    } else {
      // Update document state first (build symbol table)
      if (g_doc) {
        if (g_doc->ast)
          ast_free(g_doc->ast);
        g_doc->ast = ast;
        build_symbol_table(g_doc, ast, text);
      }

      // Check function call argument counts
      Symbol *symbols = g_doc ? g_doc->symbols : NULL;
      check_function_calls(ast, text, symbols, diagnostics, &pos, &remaining,
                           &has_diagnostics);

      // Check for undefined variables, type errors, and other diagnostics
      check_undefined_variables(ast, text, symbols, diagnostics, &pos,
                                &remaining, &has_diagnostics);

      // Check for unused variables and functions
      check_unused_symbols(symbols, text, ast, diagnostics, &pos, &remaining,
                           &has_diagnostics);

      // Free AST if not stored in document
      if (!g_doc) {
        ast_free(ast);
      }
    }
    token_array_free(tokens);
  }

  // Close the JSON array and object
  written = snprintf(diagnostics + pos, remaining, "]}");
  if (written > 0 && (size_t)written < remaining) {
    pos += (size_t)written;
  }

  send_notification("textDocument/publishDiagnostics", diagnostics);
}

/**
 * @section LSP Request Handlers
 *
 * Functions that handle specific LSP requests from the client.
 */

/**
 * @brief Handle initialize request
 *
 * Responds with server capabilities including:
 * - Text document synchronization
 * - Completion provider
 * - Definition provider
 * - Hover provider
 * - Document symbols
 * - Semantic tokens
 *
 * @param id Request ID
 */
static void handle_initialize(const char *id) {
  const char *capabilities =
      "{"
      "\"capabilities\":{"
      "\"textDocumentSync\":1,"
      "\"completionProvider\":{\"triggerCharacters\":[\".\",\":\"]},"
      "\"definitionProvider\":true,"
      "\"hoverProvider\":true,"
      "\"documentSymbolProvider\":true,"
      "\"semanticTokensProvider\":{"
      "\"legend\":{"
      "\"tokenTypes\":[\"variable\",\"function\",\"parameter\"],"
      "\"tokenModifiers\":[\"unused\",\"readonly\"]"
      "},"
      "\"range\":false,"
      "\"full\":{\"delta\":false}"
      "}"
      "}"
      "}";
  send_response(id, capabilities);
}

/**
 * @brief Handle shutdown request
 *
 * Acknowledges shutdown. The main loop will exit after this.
 *
 * @param id Request ID
 */
static void handle_shutdown(const char *id) { send_response(id, "null"); }

/**
 * @brief Handle textDocument/didOpen notification
 *
 * Called when a document is opened. Parses the document, builds the symbol
 * table, and sends initial diagnostics.
 *
 * @param uri Document URI
 * @param text Document text
 */
static void handle_did_open(const char *uri, const char *text) {
  // Update or create document state
  if (g_doc) {
    free_document_state(g_doc);
  }
  g_doc = malloc(sizeof(DocumentState));
  if (g_doc) {
    g_doc->uri = strdup(uri);
    g_doc->text = strdup(text);
    g_doc->symbols = NULL;
    g_doc->ast = NULL;
    g_doc->imported_modules = NULL;
  }
  check_diagnostics(uri, text);
}

/**
 * @brief Handle textDocument/didChange notification
 *
 * Called when a document is modified. Updates the document text and
 * re-runs diagnostics.
 *
 * @param uri Document URI
 * @param text Updated document text
 */
static void handle_did_change(const char *uri, const char *text) {
  // Update document text
  if (g_doc && g_doc->uri && strcmp(g_doc->uri, uri) == 0) {
    free(g_doc->text);
    g_doc->text = strdup(text);
  }
  check_diagnostics(uri, text);
}

/**
 * @brief Find a symbol by name in the current document
 *
 * Searches the symbol table for a symbol with the given name.
 *
 * @param name Symbol name to find
 * @return Symbol pointer, or NULL if not found
 */
static Symbol *find_symbol(const char *name) {
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

/**
 * @brief Extract the word at a given position in source code
 *
 * Finds the identifier (variable/function name) at the specified line and
 * character position. Handles dots for module.function syntax.
 *
 * @param source Source code text
 * @param line Line number (0-based)
 * @param character Character position (0-based)
 * @return Extracted word (caller must free), or NULL if not found
 */
static char *get_word_at_position(const char *source, size_t line,
                                  size_t character) {
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

/**
 * @brief Handle textDocument/definition request (go-to-definition)
 *
 * Finds the definition location of a symbol at the requested position.
 * Returns the file URI and position of the definition.
 *
 * @param id Request ID
 * @param body Request body JSON
 */
static void handle_definition(const char *id, const char *body) {
  if (!g_doc || !g_doc->text) {
    send_response(id, "null");
    return;
  }

  char *line_str = json_get_nested_value(body, "params.position.line");
  char *character_str =
      json_get_nested_value(body, "params.position.character");

  if (!line_str || !character_str) {
    send_response(id, "null");
    free(line_str);
    free(character_str);
    return;
  }

  size_t line = (size_t)strtoul(line_str, NULL, 10);
  size_t character = (size_t)strtoul(character_str, NULL, 10);
  free(line_str);
  free(character_str);

  // Find word at position
  char *word = get_word_at_position(g_doc->text, line, character);
  if (!word) {
    send_response(id, "null");
    return;
  }

  // Handle module.function syntax
  char *dot = strchr(word, '.');
  if (dot) {
    // Module function - for now return null (could implement module lookup)
    free(word);
    send_response(id, "null");
    return;
  }

  // Find symbol
  Symbol *sym = find_symbol(word);
  free(word);

  if (!sym) {
    send_response(id, "null");
    return;
  }

  // Return definition location
  char result[512];
  char escaped_uri[256];
  json_escape(g_doc->uri, escaped_uri, sizeof(escaped_uri));
  snprintf(
      result, sizeof(result),
      "{\"uri\":\"%s\",\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
      "\"end\":{\"line\":%zu,\"character\":%zu}}}",
      escaped_uri, sym->line - 1, sym->column - 1, sym->line - 1,
      sym->column - 1 + strlen(sym->name));
  send_response(id, result);
}

// Get module description for built-in modules
static const char *get_module_description(const char *module_name) {
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
           "Provides pattern matching using POSIX extended regular expressions:\n\n"
           "• `match(string, pattern)` - Returns true if pattern matches entire string  \n"
           "• `search(string, pattern)` - Returns first matched substring or null  \n"
           "• `findall(string, pattern)` - Returns list of all matched substrings  \n\n"
           "**Usage:** `import regex` then `call regex.match with \"hello\", \"h.*o\"`";
  }
  return NULL;
}

// Handle hover
/**
 * @brief Handle textDocument/hover request
 *
 * Provides hover information for a symbol at the requested position.
 * Returns type information, documentation, and whether the symbol is mutable.
 *
 * @param id Request ID
 * @param body Request body JSON
 */
static void handle_hover(const char *id, const char *body) {
  if (!g_doc || !g_doc->text) {
    send_response(id, "null");
    return;
  }

  char *line_str = json_get_nested_value(body, "params.position.line");
  char *character_str =
      json_get_nested_value(body, "params.position.character");

  if (!line_str || !character_str) {
    send_response(id, "null");
    free(line_str);
    free(character_str);
    return;
  }

  size_t line = (size_t)strtoul(line_str, NULL, 10);
  size_t character = (size_t)strtoul(character_str, NULL, 10);
  free(line_str);
  free(character_str);

  // Find word at position
  char *word = get_word_at_position(g_doc->text, line, character);
  if (!word) {
    send_response(id, "null");
    return;
  }

  // Handle module.function syntax
  char *dot = strchr(word, '.');
  if (dot) {
    // Module function - could provide hover info for built-ins
    free(word);
    send_response(id, "null");
    return;
  }

  // Check if it's a built-in module
  const char *module_desc = get_module_description(word);
  if (module_desc) {
    char *escaped_name = malloc(strlen(word) * 2 + 1);
    if (!escaped_name) {
      free(word);
      send_response(id, "null");
      return;
    }
    json_escape(word, escaped_name, strlen(word) * 2 + 1);

    // For markdown, we need to escape special characters but preserve newlines
    // Build the hover text with proper escaping
    char hover_text[2048];
    snprintf(hover_text, sizeof(hover_text), "**module** `%s`\n\n%s",
             escaped_name, module_desc);

    // Escape for JSON but preserve newlines as \n (not \\n)
    char escaped_hover[4096];
    size_t out_pos = 0;
    for (size_t i = 0;
         hover_text[i] != '\0' && out_pos < sizeof(escaped_hover) - 1; i++) {
      switch (hover_text[i]) {
      case '\\':
        if (out_pos < sizeof(escaped_hover) - 2) {
          escaped_hover[out_pos++] = '\\';
          escaped_hover[out_pos++] = '\\';
        }
        break;
      case '"':
        if (out_pos < sizeof(escaped_hover) - 2) {
          escaped_hover[out_pos++] = '\\';
          escaped_hover[out_pos++] = '"';
        }
        break;
      case '\n':
        // Preserve newlines as \n (not \\n) for markdown rendering
        if (out_pos < sizeof(escaped_hover) - 2) {
          escaped_hover[out_pos++] = '\\';
          escaped_hover[out_pos++] = 'n';
        }
        break;
      case '\r':
        if (out_pos < sizeof(escaped_hover) - 2) {
          escaped_hover[out_pos++] = '\\';
          escaped_hover[out_pos++] = 'r';
        }
        break;
      case '\t':
        if (out_pos < sizeof(escaped_hover) - 2) {
          escaped_hover[out_pos++] = '\\';
          escaped_hover[out_pos++] = 't';
        }
        break;
      default:
        if (out_pos < sizeof(escaped_hover) - 1) {
          escaped_hover[out_pos++] = hover_text[i];
        }
        break;
      }
    }
    escaped_hover[out_pos] = '\0';

    char result[4096];
    snprintf(result, sizeof(result),
             "{\"contents\":{\"kind\":\"markdown\",\"value\":\"%s\"}}",
             escaped_hover);
    free(escaped_name);
    free(word);
    send_response(id, result);
    return;
  }

  // Find symbol
  Symbol *sym = find_symbol(word);
  free(word);

  if (!sym) {
    send_response(id, "null");
    return;
  }

  // Build hover info
  char hover_text[512];
  const char *type_str = "variable";
  if (sym->type == SYMBOL_FUNCTION)
    type_str = "function";
  else if (sym->type == SYMBOL_PARAMETER)
    type_str = "parameter";

  char *escaped_name = malloc(strlen(sym->name) * 2 + 1);
  if (!escaped_name) {
    send_response(id, "null");
    return;
  }
  json_escape(sym->name, escaped_name, strlen(sym->name) * 2 + 1);

  if (sym->type_name) {
    char *escaped_type = malloc(strlen(sym->type_name) * 2 + 1);
    if (escaped_type) {
      json_escape(sym->type_name, escaped_type, strlen(sym->type_name) * 2 + 1);
      snprintf(hover_text, sizeof(hover_text), "**%s** `%s`\n\nType: `%s`\n%s",
               type_str, escaped_name, escaped_type,
               sym->is_mutable ? "Mutable" : "Immutable");
      free(escaped_type);
    } else {
      snprintf(hover_text, sizeof(hover_text), "**%s** `%s`", type_str,
               escaped_name);
    }
  } else {
    snprintf(hover_text, sizeof(hover_text), "**%s** `%s`\n%s", type_str,
             escaped_name, sym->is_mutable ? "Mutable" : "Immutable");
  }
  free(escaped_name);

  char escaped_hover[1024];
  json_escape(hover_text, escaped_hover, sizeof(escaped_hover));

  char result[1024];
  snprintf(result, sizeof(result),
           "{\"contents\":{\"kind\":\"markdown\",\"value\":\"%s\"}}",
           escaped_hover);
  send_response(id, result);
}

// Handle document symbols
/**
 * @brief Handle textDocument/documentSymbol request
 *
 * Returns all symbols in the document for the outline view.
 * Includes variables, functions, and their hierarchical structure.
 *
 * @param id Request ID
 */
static void handle_document_symbols(const char *id) {
  if (!g_doc || !g_doc->symbols) {
    send_response(id, "[]");
    return;
  }

  char symbols[8192];
  size_t pos = 0;
  size_t remaining = sizeof(symbols);
  bool first = true;

  pos += snprintf(symbols + pos, remaining - pos, "[");

  Symbol *sym = g_doc->symbols;
  while (sym && pos < remaining - 200) {
    if (!first)
      pos += snprintf(symbols + pos, remaining - pos, ",");
    first = false;

    const char *kind_str = "6"; // Variable
    if (sym->type == SYMBOL_FUNCTION)
      kind_str = "12"; // Function
    else if (sym->type == SYMBOL_PARAMETER)
      kind_str = "5"; // Property

    char *escaped_name = malloc(strlen(sym->name) * 2 + 1);
    if (escaped_name) {
      json_escape(sym->name, escaped_name, strlen(sym->name) * 2 + 1);
      pos += snprintf(symbols + pos, remaining - pos,
                      "{\"name\":\"%s\",\"kind\":%s,"
                      "\"location\":{\"uri\":\"%s\","
                      "\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
                      "\"end\":{\"line\":%zu,\"character\":%zu}}}}",
                      escaped_name, kind_str, g_doc->uri, sym->line - 1,
                      sym->column - 1, sym->line - 1,
                      sym->column - 1 + strlen(sym->name));
      free(escaped_name);
    }
    sym = sym->next;
  }

  pos += snprintf(symbols + pos, remaining - pos, "]");
  send_response(id, symbols);
}

// Handle semantic tokens request
/**
 * @brief Handle textDocument/semanticTokens/full request
 *
 * Returns semantic token information for syntax highlighting.
 * Identifies variables, functions, and parameters with their modifiers
 * (unused, readonly).
 *
 * @param id Request ID
 */
static void handle_semantic_tokens(const char *id) {
  if (!g_doc || !g_doc->text || !g_doc->symbols)
    return send_response(id, "{\"data\":[]}");

  // Build semantic tokens array
  // Format: [deltaLine, deltaStartChar, length, tokenType, tokenModifiers, ...]
  // Token types: 0=variable, 1=function, 2=parameter
  // Token modifiers: 0=unused (bit 0), 1=readonly (bit 1)
  char tokens[16384];
  size_t pos = 0;
  size_t remaining = sizeof(tokens);

  pos += snprintf(tokens + pos, remaining - pos, "{\"data\":[");

  bool first = true;
  int prev_line = 0;
  int prev_col = 0;

  for (Symbol *sym = g_doc->symbols; sym != NULL; sym = sym->next) {
    if (sym->type == SYMBOL_PARAMETER)
      continue;

    // Find the actual position of the symbol in source text
    size_t line = 1, col = 0;

    if (sym->type == SYMBOL_FUNCTION) {
      char pattern[256];
      snprintf(pattern, sizeof(pattern), "function %s with", sym->name);
      find_node_position(NULL, g_doc->text, pattern, &line, &col);
      if (line == 1 && col == 0) {
        snprintf(pattern, sizeof(pattern), "function %s", sym->name);
        find_node_position(NULL, g_doc->text, pattern, &line, &col);
      }
    } else {
      char pattern[256];
      snprintf(pattern, sizeof(pattern), "let %s to", sym->name);
      find_node_position(NULL, g_doc->text, pattern, &line, &col);
      if (line == 1 && col == 0) {
        snprintf(pattern, sizeof(pattern), "set %s to", sym->name);
        find_node_position(NULL, g_doc->text, pattern, &line, &col);
      }
    }

    if (line == 1 && col == 0)
      continue; // Skip if not found

    // Calculate the column position of the symbol name itself
    size_t name_col = col;
    const char *line_start = g_doc->text;
    size_t current_line = 1;
    for (const char *p = g_doc->text; *p != '\0' && current_line < line; p++) {
      if (*p == '\n') {
        current_line++;
        if (current_line == line) {
          line_start = p + 1;
          break;
        }
      }
    }

    if (sym->type == SYMBOL_FUNCTION) {
      const char *pattern_start = line_start + col;
      const char *name_start = pattern_start;
      while (*name_start != '\0' && *name_start != '\n' &&
             (*name_start == ' ' || *name_start == '\t' ||
              (*name_start >= 'a' && *name_start <= 'z') ||
              (*name_start >= 'A' && *name_start <= 'Z'))) {
        if (*name_start == ' ') {
          name_start++;
          break;
        }
        name_start++;
      }
      while (*name_start == ' ' || *name_start == '\t') {
        name_start++;
      }
      name_col = (size_t)(name_start - line_start);
    } else {
      const char *pattern_start = line_start + col;
      const char *name_start = pattern_start;
      while (*name_start != '\0' && *name_start != '\n' &&
             (*name_start == ' ' || *name_start == '\t' ||
              (*name_start >= 'a' && *name_start <= 'z') ||
              (*name_start >= 'A' && *name_start <= 'Z'))) {
        if (*name_start == ' ') {
          name_start++;
          break;
        }
        name_start++;
      }
      while (*name_start == ' ' || *name_start == '\t') {
        name_start++;
      }
      name_col = (size_t)(name_start - line_start);
    }

    // Determine token type and modifiers
    int token_type =
        sym->type == SYMBOL_FUNCTION ? 1 : 0; // 0=variable, 1=function
    int token_modifiers = 0;

    // Mark as unused if not used (or defined but never read for variables)
    if (sym->type == SYMBOL_VARIABLE) {
      if ((sym->written && !sym->read) || (!sym->used && !sym->written)) {
        token_modifiers |= 1; // unused modifier (bit 0)
      }
      if (!sym->is_mutable) {
        token_modifiers |= 2; // readonly modifier (bit 1)
      }
    } else if (sym->type == SYMBOL_FUNCTION) {
      if (!sym->used) {
        token_modifiers |= 1; // unused modifier (bit 0)
      }
    }

    // Calculate relative positions (delta encoding)
    int delta_line = (int)line - 1 - prev_line;
    int delta_col = delta_line == 0 ? (int)name_col - prev_col : (int)name_col;

    if (!first) {
      int written = snprintf(tokens + pos, remaining - pos, ",");
      if (written > 0 && (size_t)written < remaining - pos) {
        pos += (size_t)written;
        remaining -= (size_t)written;
      } else {
        break; // Buffer full
      }
    }
    first = false;

    int written =
        snprintf(tokens + pos, remaining - pos, "%d,%d,%zu,%d,%d", delta_line,
                 delta_col, strlen(sym->name), token_type, token_modifiers);
    if (written > 0 && (size_t)written < remaining - pos) {
      pos += (size_t)written;
      remaining -= (size_t)written;
    } else {
      break; // Buffer full
    }

    prev_line = (int)line - 1;
    prev_col = (int)name_col;
  }

  int written = snprintf(tokens + pos, remaining - pos, "]}");
  if (written > 0 && (size_t)written < remaining - pos) {
    pos += (size_t)written;
  }
  send_response(id, tokens);
}

// Handle completion with context awareness
/**
 * @brief Handle textDocument/completion request
 *
 * Provides code completion suggestions at the requested position.
 * Returns keywords, built-in functions, variables, and functions from
 * the current scope.
 *
 * @param id Request ID
 * @param body Request body JSON
 */
static void handle_completion(const char *id, const char *body) {
  (void)body; // Unused parameter
  // Build completion list with keywords, built-ins, and symbols
  char completions[16384];
  size_t pos = 0;
  size_t remaining = sizeof(completions);

  pos += snprintf(completions + pos, remaining - pos,
                  "{\"isIncomplete\":false,\"items\":[");

  bool first = true;

  // Add keywords
  const char *keywords[][2] = {
      // Variable declarations
      {"set", "Immutable variable"},
      {"let", "Mutable variable"},
      {"to", "Assignment operator (set x to 5)"},
      {"as", "Type annotation (as number)"},
      // Control flow
      {"if", "Conditional statement"},
      {"else", "Else clause"},
      {"else if", "Else-if clause"},
      {"for", "For loop"},
      {"in", "Loop iterator (for x in list)"},
      {"while", "While loop"},
      {"break", "Break out of loop"},
      {"continue", "Continue to next iteration"},
      {"delete", "Delete map key (delete var at key)"},
      // Exception handling
      {"try", "Try block (exception handling)"},
      {"catch", "Catch exception (catch ErrorType as var)"},
      {"finally", "Finally block (always executes)"},
      {"raise", "Raise exception (raise ErrorType \"message\")"},
      // Logical operators
      {"and", "Logical AND operator"},
      {"or", "Logical OR operator"},
      {"not", "Logical NOT operator"},
      // Arithmetic operators
      {"plus", "Addition operator"},
      {"minus", "Subtraction operator"},
      {"times", "Multiplication operator"},
      {"divided", "Division operator"},
      {"by", "Step value or division (divided by)"},
      {"mod", "Modulo operator"},
      // Comparison operators
      {"is", "Comparison prefix (is equal to)"},
      {"equal", "Equality comparison"},
      {"greater", "Greater than comparison"},
      {"less", "Less than comparison"},
      {"than", "Comparison suffix (greater than)"},
      // Data structures
      {"list", "Create list literal"},
      {"map", "Create map literal"},
      {"range", "Create range literal (range 1 to 10)"},
      {"at", "List/map indexing operator"},
      {"from", "List slicing operator"},
      {"end", "End of list (for slicing)"},
      // Functions
      {"function", "Define function"},
      {"call", "Call function"},
      {"with", "Function arguments (call fn with args)"},
      {"return", "Return value"},
      // Modules
      {"import", "Import module"},
      // I/O
      {"print", "Print value"},
      // Literals
      {"true", "Boolean true"},
      {"false", "Boolean false"},
      {"null", "Null value"},
  };

  for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
    if (!first) {
      int written = snprintf(completions + pos, remaining - pos, ",");
      if (written > 0 && (size_t)written < remaining - pos) {
        pos += (size_t)written;
        remaining -= (size_t)written;
      } else {
        break; // Buffer full
      }
    }
    first = false;
    char escaped[256];
    json_escape(keywords[i][0], escaped, sizeof(escaped));
    char escaped_detail[256];
    json_escape(keywords[i][1], escaped_detail, sizeof(escaped_detail));
    int written = snprintf(completions + pos, remaining - pos,
                           "{\"label\":\"%s\",\"kind\":14,\"detail\":\"%s\"}",
                           escaped, escaped_detail);
    if (written > 0 && (size_t)written < remaining - pos) {
      pos += (size_t)written;
      remaining -= (size_t)written;
    } else {
      break; // Buffer full
    }
  }

  // Add built-in functions
  const char *builtins[][2] = {
      {"len", "Get length of list, string, or range"},
      {"uppercase", "Convert string to uppercase"},
      {"lowercase", "Convert string to lowercase"},
      {"trim", "Remove leading and trailing whitespace"},
      {"split", "Split string by delimiter into list"},
      {"join", "Join list of strings with delimiter"},
      {"to_string", "Convert value to string"},
      {"to_number", "Convert string to number"},
      {"to_bool", "Convert value to boolean"},
      {"contains", "Check if string contains substring"},
      {"starts_with", "Check if string starts with prefix"},
      {"ends_with", "Check if string ends with suffix"},
      {"replace", "Replace all occurrences (string, old, new)"},
      {"sqrt", "Square root of a number"},
      {"power", "Raise base to exponent"},
      {"abs", "Absolute value of a number"},
      {"round", "Round number to nearest integer"},
      {"floor", "Floor of a number"},
      {"ceil", "Ceiling of a number"},
      {"rand", "Random number between 0 and 1 (no args)"},
      {"min", "Minimum of numbers"},
      {"max", "Maximum of numbers"},
      {"reverse", "Reverse a list"},
      {"sort", "Sort a list"},
      {"read_file", "Read entire file content as string"},
      {"write_file", "Write string content to file (path, content)"},
      {"read_lines", "Read file and return list of lines"},
      {"file_exists", "Check if file or directory exists"},
      {"list_files", "List files in directory"},
      {"join_path", "Join two path components (path1, path2)"},
      {"dirname", "Get directory name from path"},
      {"basename", "Get file name from path"},
      {"regex.match", "Check if pattern matches entire string (string, pattern)"},
      {"regex.search", "Find first match in string (string, pattern)"},
      {"regex.findall", "Find all matches in string (string, pattern)"},
  };

  for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
    if (!first) {
      int written = snprintf(completions + pos, remaining - pos, ",");
      if (written > 0 && (size_t)written < remaining - pos) {
        pos += (size_t)written;
        remaining -= (size_t)written;
      } else {
        break; // Buffer full
      }
    }
    first = false;
    char escaped[256];
    json_escape(builtins[i][0], escaped, sizeof(escaped));
    char escaped_detail[256];
    json_escape(builtins[i][1], escaped_detail, sizeof(escaped_detail));
    int written = snprintf(completions + pos, remaining - pos,
                           "{\"label\":\"%s\",\"kind\":3,\"detail\":\"%s\"}",
                           escaped, escaped_detail);
    if (written > 0 && (size_t)written < remaining - pos) {
      pos += (size_t)written;
      remaining -= (size_t)written;
    } else {
      break; // Buffer full
    }
  }

  // Add document symbols (variables and functions)
  if (g_doc && g_doc->symbols) {
    Symbol *sym = g_doc->symbols;
    while (sym && pos < remaining - 200) {
      if (!first)
        pos += snprintf(completions + pos, remaining - pos, ",");
      first = false;

      const char *kind_str = "6"; // Variable
      if (sym->type == SYMBOL_FUNCTION)
        kind_str = "12"; // Function

      char escaped[256];
      json_escape(sym->name, escaped, sizeof(escaped));
      const char *detail =
          sym->type == SYMBOL_FUNCTION ? "User-defined function" : "Variable";
      char escaped_detail[256];
      json_escape(detail, escaped_detail, sizeof(escaped_detail));

      pos += snprintf(completions + pos, remaining - pos,
                      "{\"label\":\"%s\",\"kind\":%s,\"detail\":\"%s\"}",
                      escaped, kind_str, escaped_detail);
      sym = sym->next;
    }
  }

  // Add constants
  if (!first)
    pos += snprintf(completions + pos, remaining - pos, ",");
  pos += snprintf(completions + pos, remaining - pos,
                  "{\"label\":\"Pi\",\"kind\":21,\"detail\":\"Mathematical "
                  "constant\"}");

  pos += snprintf(completions + pos, remaining - pos, "]}");
  send_response(id, completions);
}

/**
 * @brief Main LSP server loop
 *
 * Reads JSON-RPC messages from stdin and dispatches them to appropriate
 * handlers. Continues until shutdown request is received.
 *
 * Supported methods:
 * - initialize: Server initialization
 * - shutdown: Server shutdown
 * - textDocument/didOpen: Document opened
 * - textDocument/didChange: Document changed
 * - textDocument/completion: Code completion
 * - textDocument/definition: Go to definition
 * - textDocument/hover: Hover information
 * - textDocument/documentSymbol: Document outline
 * - textDocument/semanticTokens/full: Semantic highlighting
 *
 * @return 0 on normal exit
 */
int main(void) {
  fprintf(stderr, "Kronos LSP Server starting...\n");

  char *body = NULL;
  size_t body_len = 0;

  while (read_lsp_message(&body, &body_len)) {
    if (!body)
      continue;

    char *method = json_get_string_value(body, "method");
    if (!method) {
      free(body);
      body = NULL;
      continue;
    }

    if (strcmp(method, "initialize") == 0) {
      char *id = json_get_id_value(body);
      handle_initialize(id ? id : "null");
      free(id);
    } else if (strcmp(method, "shutdown") == 0) {
      char *id = json_get_id_value(body);
      handle_shutdown(id ? id : "null");
      free(id);
      free(method);
      free(body);
      break;
    } else if (strcmp(method, "textDocument/didOpen") == 0) {
      // LSP spec: params.textDocument.uri and params.textDocument.text
      char *uri = json_get_nested_value(body, "params.textDocument.uri");
      if (!uri) {
        uri = json_get_string_value(body, "uri");
      }
      char *text = json_get_nested_value(body, "params.textDocument.text");
      if (!text) {
        text = json_get_string_value(body, "text");
      }
      if (uri && text) {
        handle_did_open(uri, text);
      }
      free(uri);
      free(text);
    } else if (strcmp(method, "textDocument/didChange") == 0) {
      // LSP spec: params.textDocument.uri and params.contentChanges[0].text
      char *uri = json_get_nested_value(body, "params.textDocument.uri");
      if (!uri) {
        uri = json_get_string_value(body, "uri");
      }
      // Get text from contentChanges[0].text
      char *text = json_get_nested_value(body, "params.contentChanges.0.text");
      if (!text) {
        // Fallback to params.text
        text = json_get_string_value(body, "text");
      }
      if (uri && text) {
        handle_did_change(uri, text);
      }
      free(uri);
      free(text);
    } else if (strcmp(method, "textDocument/completion") == 0) {
      char *id = json_get_id_value(body);
      handle_completion(id ? id : "null", body);
      free(id);
    } else if (strcmp(method, "textDocument/definition") == 0) {
      char *id = json_get_id_value(body);
      handle_definition(id ? id : "null", body);
      free(id);
    } else if (strcmp(method, "textDocument/hover") == 0) {
      char *id = json_get_id_value(body);
      handle_hover(id ? id : "null", body);
      free(id);
    } else if (strcmp(method, "textDocument/documentSymbol") == 0) {
      char *id = json_get_id_value(body);
      handle_document_symbols(id ? id : "null");
      free(id);
    } else if (strcmp(method, "textDocument/semanticTokens/full") == 0) {
      char *id = json_get_id_value(body);
      handle_semantic_tokens(id ? id : "null");
      free(id);
    } else {
      fprintf(stderr, "Unsupported LSP method: %s\n", method);
    }

    free(method);
    free(body);
    body = NULL;
  }

  free_document_state(g_doc);
  free(body);
  return 0;
}
