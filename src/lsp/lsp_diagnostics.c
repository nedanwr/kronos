/**
 * @file lsp_diagnostics.c
 * @brief Error checking and diagnostics for LSP server
 */

#include "../frontend/tokenizer.h"
#include "lsp.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern DocumentState *g_doc;

// Macro to safely write to diagnostics buffer with automatic growth
#define SAFE_DIAGNOSTICS_WRITE(diag, cap, pos, rem, needed, ...)               \
  do {                                                                         \
    if (grow_diagnostics_buffer((diag), (cap), *(pos), (needed))) {            \
      *(rem) = *(cap) - *(pos);                                                \
      int _written = snprintf(*(diag) + *(pos), *(rem), __VA_ARGS__);          \
      if (_written > 0 && (size_t)_written < *(rem)) {                         \
        *(pos) += (size_t)_written;                                            \
        *(rem) = *(cap) - *(pos);                                              \
      }                                                                        \
    }                                                                          \
  } while (0)

// SeenVar structure for tracking variables in scope
typedef struct {
  char *name;
  bool is_mutable;
  size_t assignment_count;
  size_t first_statement_index;
} SeenVar;

// Maximum recursion depth for type inference to prevent stack overflow
#define MAX_TYPE_INFER_DEPTH 32

#define LSP_IMPORT_STACK_MAX 256

typedef struct {
  char *paths[LSP_IMPORT_STACK_MAX];
  size_t count;
} LSPImportStack;

typedef enum {
  LSP_MODULE_CACHE_STATE_UNKNOWN = 0,
  LSP_MODULE_CACHE_STATE_READY,
  LSP_MODULE_CACHE_STATE_MISSING,
  LSP_MODULE_CACHE_STATE_PARSE_ERROR,
} LSPModuleCacheState;

typedef enum {
  LSP_MODULE_LOAD_OK = 0,
  LSP_MODULE_LOAD_PENDING,
  LSP_MODULE_LOAD_MISSING,
  LSP_MODULE_LOAD_PARSE_ERROR,
} LSPModuleLoadResult;

typedef struct LSPModuleCacheEntry {
  char *path;
  char *source;
  AST *ast;
  LSPModuleCacheState state;
  struct LSPModuleCacheEntry *next;
} LSPModuleCacheEntry;

static LSPModuleCacheEntry *g_module_cache = NULL;

static int lsp_hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return 10 + (c - 'a');
  }
  if (c >= 'A' && c <= 'F') {
    return 10 + (c - 'A');
  }
  return -1;
}

static char *lsp_uri_to_path(const char *uri) {
  if (!uri) {
    return NULL;
  }

  const char *path = uri;
  if (strncmp(uri, "file://", 7) == 0) {
    path = uri + 7;
  }

  size_t path_len = strlen(path);
  char *decoded = malloc(path_len + 1);
  if (!decoded) {
    return NULL;
  }

  size_t out = 0;
  for (size_t i = 0; i < path_len; i++) {
    if (path[i] == '%' && i + 2 < path_len) {
      int hi = lsp_hex_value(path[i + 1]);
      int lo = lsp_hex_value(path[i + 2]);
      if (hi >= 0 && lo >= 0) {
        decoded[out++] = (char)((hi << 4) | lo);
        i += 2;
        continue;
      }
    }
    decoded[out++] = path[i];
  }
  decoded[out] = '\0';
  return decoded;
}

static void lsp_module_cache_entry_clear_ast(LSPModuleCacheEntry *entry) {
  if (!entry || !entry->ast) {
    return;
  }
  ast_free(entry->ast);
  entry->ast = NULL;
}

static LSPModuleCacheEntry *lsp_module_cache_find(const char *file_path) {
  if (!file_path) {
    return NULL;
  }
  for (LSPModuleCacheEntry *entry = g_module_cache; entry;
       entry = entry->next) {
    if (entry->path && strcmp(entry->path, file_path) == 0) {
      return entry;
    }
  }
  return NULL;
}

static LSPModuleCacheEntry *lsp_module_cache_get_or_create(
    const char *file_path) {
  if (!file_path) {
    return NULL;
  }

  LSPModuleCacheEntry *entry = lsp_module_cache_find(file_path);
  if (entry) {
    return entry;
  }

  entry = calloc(1, sizeof(*entry));
  if (!entry) {
    return NULL;
  }
  entry->path = strdup(file_path);
  if (!entry->path) {
    free(entry);
    return NULL;
  }
  entry->state = LSP_MODULE_CACHE_STATE_UNKNOWN;
  entry->next = g_module_cache;
  g_module_cache = entry;
  return entry;
}

static bool lsp_module_cache_set_source_text(LSPModuleCacheEntry *entry,
                                             const char *source_text) {
  if (!entry || !source_text) {
    return false;
  }

  char *source_copy = strdup(source_text);
  if (!source_copy) {
    return false;
  }

  free(entry->source);
  entry->source = source_copy;
  lsp_module_cache_entry_clear_ast(entry);
  entry->state = LSP_MODULE_CACHE_STATE_UNKNOWN;
  return true;
}

static void lsp_module_cache_store_ast(LSPModuleCacheEntry *entry, AST *ast) {
  if (!entry) {
    if (ast) {
      ast_free(ast);
    }
    return;
  }

  lsp_module_cache_entry_clear_ast(entry);
  entry->ast = ast;
  entry->state =
      ast ? LSP_MODULE_CACHE_STATE_READY : LSP_MODULE_CACHE_STATE_PARSE_ERROR;
}

static void lsp_module_cache_mark_missing(LSPModuleCacheEntry *entry) {
  if (!entry) {
    return;
  }

  free(entry->source);
  entry->source = NULL;
  lsp_module_cache_entry_clear_ast(entry);
  entry->state = LSP_MODULE_CACHE_STATE_MISSING;
}

static void lsp_module_cache_mark_parse_error(LSPModuleCacheEntry *entry) {
  if (!entry) {
    return;
  }

  lsp_module_cache_entry_clear_ast(entry);
  entry->state = LSP_MODULE_CACHE_STATE_PARSE_ERROR;
}

static void lsp_module_cache_clear_all(void) {
  LSPModuleCacheEntry *entry = g_module_cache;
  while (entry) {
    LSPModuleCacheEntry *next = entry->next;
    free(entry->path);
    free(entry->source);
    if (entry->ast) {
      ast_free(entry->ast);
    }
    free(entry);
    entry = next;
  }
  g_module_cache = NULL;
}

void lsp_clear_diagnostics_cache(void) { lsp_module_cache_clear_all(); }

static void lsp_module_cache_update_document_source(const char *uri,
                                                    const char *text) {
  if (!uri || !text) {
    return;
  }

  char *path = lsp_uri_to_path(uri);
  if (!path) {
    return;
  }

  LSPModuleCacheEntry *entry = lsp_module_cache_get_or_create(path);
  if (entry) {
    (void)lsp_module_cache_set_source_text(entry, text);
  }
  free(path);
}

static AST *lsp_parse_ast_from_source(const char *source) {
  if (!source) {
    return NULL;
  }

  TokenArray *tokens = tokenize(source, NULL);
  if (!tokens) {
    return NULL;
  }

  AST *ast = parse(tokens, NULL);
  token_array_free(tokens);
  return ast;
}

static char *lsp_resolve_module_path(const char *base_path,
                                     const char *module_path) {
  if (!module_path) {
    return NULL;
  }

  if (module_path[0] == '/') {
    return strdup(module_path);
  }

  if ((module_path[0] == '.' && module_path[1] == '/') ||
      (module_path[0] == '.' && module_path[1] == '.' &&
       module_path[2] == '/')) {
    if (base_path && base_path[0] != '\0') {
      char *last_slash = strrchr(base_path, '/');
      if (last_slash) {
        size_t dir_len = (size_t)(last_slash - base_path) + 1;
        size_t module_len = strlen(module_path);
        char *resolved = malloc(dir_len + module_len + 1);
        if (!resolved) {
          return NULL;
        }
        strncpy(resolved, base_path, dir_len);
        strcpy(resolved + dir_len, module_path);
        return resolved;
      }
    }
    return strdup(module_path);
  }

  if (strchr(module_path, '/')) {
    return strdup(module_path);
  }

  if (base_path && base_path[0] != '\0') {
    char *last_slash = strrchr(base_path, '/');
    if (last_slash) {
      size_t dir_len = (size_t)(last_slash - base_path) + 1;
      size_t module_len = strlen(module_path);
      char *resolved = malloc(dir_len + module_len + 1);
      if (!resolved) {
        return NULL;
      }
      strncpy(resolved, base_path, dir_len);
      strcpy(resolved + dir_len, module_path);
      return resolved;
    }
  }

  return strdup(module_path);
}

static bool lsp_import_stack_contains(const LSPImportStack *stack,
                                      const char *path_key) {
  if (!stack || !path_key) {
    return false;
  }

  for (size_t i = 0; i < stack->count; i++) {
    if (stack->paths[i] && strcmp(stack->paths[i], path_key) == 0) {
      return true;
    }
  }
  return false;
}

static bool lsp_import_stack_push(LSPImportStack *stack, const char *path_key) {
  if (!stack || !path_key) {
    return false;
  }
  if (stack->count >= LSP_IMPORT_STACK_MAX) {
    return false;
  }

  char *copy = strdup(path_key);
  if (!copy) {
    return false;
  }
  stack->paths[stack->count++] = copy;
  return true;
}

static void lsp_import_stack_pop(LSPImportStack *stack) {
  if (!stack || stack->count == 0) {
    return;
  }
  stack->count--;
  free(stack->paths[stack->count]);
  stack->paths[stack->count] = NULL;
}

static void lsp_import_stack_clear(LSPImportStack *stack) {
  if (!stack) {
    return;
  }
  while (stack->count > 0) {
    lsp_import_stack_pop(stack);
  }
}

static char *lsp_read_source_from_file(const char *file_path) {
  if (!file_path) {
    return NULL;
  }

  FILE *file = fopen(file_path, "r");
  if (!file) {
    return NULL;
  }

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

  return source;
}

static LSPModuleLoadResult lsp_parse_ast_from_file(const char *file_path,
                                                   bool allow_disk_io,
                                                   AST **out_ast) {
  if (!out_ast) {
    return LSP_MODULE_LOAD_PARSE_ERROR;
  }
  *out_ast = NULL;

  if (!file_path) {
    return LSP_MODULE_LOAD_PARSE_ERROR;
  }

  LSPModuleCacheEntry *entry = lsp_module_cache_find(file_path);
  if (entry && entry->state == LSP_MODULE_CACHE_STATE_READY && entry->ast) {
    *out_ast = entry->ast;
    return LSP_MODULE_LOAD_OK;
  }

  if (entry && entry->source) {
    AST *parsed = lsp_parse_ast_from_source(entry->source);
    if (!parsed) {
      lsp_module_cache_mark_parse_error(entry);
      return LSP_MODULE_LOAD_PARSE_ERROR;
    }
    lsp_module_cache_store_ast(entry, parsed);
    *out_ast = entry->ast;
    return LSP_MODULE_LOAD_OK;
  }

  if (entry && entry->state == LSP_MODULE_CACHE_STATE_MISSING) {
    return LSP_MODULE_LOAD_MISSING;
  }

  if (!allow_disk_io) {
    return LSP_MODULE_LOAD_PENDING;
  }

  if (!entry) {
    entry = lsp_module_cache_get_or_create(file_path);
    if (!entry) {
      return LSP_MODULE_LOAD_PARSE_ERROR;
    }
  }

  char *source = lsp_read_source_from_file(file_path);
  if (!source) {
    lsp_module_cache_mark_missing(entry);
    return LSP_MODULE_LOAD_MISSING;
  }

  if (!lsp_module_cache_set_source_text(entry, source)) {
    free(source);
    return LSP_MODULE_LOAD_PARSE_ERROR;
  }

  AST *parsed = lsp_parse_ast_from_source(source);
  free(source);
  if (!parsed) {
    lsp_module_cache_mark_parse_error(entry);
    return LSP_MODULE_LOAD_PARSE_ERROR;
  }

  lsp_module_cache_store_ast(entry, parsed);
  *out_ast = entry->ast;
  return LSP_MODULE_LOAD_OK;
}

static bool lsp_check_import_chain_recursive(const char *module_name,
                                             const char *import_path,
                                             const char *resolved_path,
                                             LSPImportStack *stack,
                                             bool allow_disk_io,
                                             char *error_msg,
                                             size_t error_msg_size) {
  if (!module_name || !resolved_path || !stack || !error_msg ||
      error_msg_size == 0) {
    return false;
  }

  if (lsp_import_stack_contains(stack, resolved_path)) {
    snprintf(error_msg, error_msg_size,
             "Circular import detected: module '%s' is already being loaded",
             module_name);
    return true;
  }

  if (!lsp_import_stack_push(stack, resolved_path)) {
    return false;
  }

  AST *module_ast = NULL;
  LSPModuleLoadResult load_result =
      lsp_parse_ast_from_file(resolved_path, allow_disk_io, &module_ast);
  if (load_result == LSP_MODULE_LOAD_MISSING) {
    snprintf(error_msg, error_msg_size, "Failed to open module file: %s",
             import_path ? import_path : resolved_path);
    lsp_import_stack_pop(stack);
    return true;
  }
  if (load_result == LSP_MODULE_LOAD_PENDING) {
    lsp_import_stack_pop(stack);
    return false;
  }
  if (load_result != LSP_MODULE_LOAD_OK || !module_ast) {
    lsp_import_stack_pop(stack);
    return false;
  }

  for (size_t i = 0; i < module_ast->count; i++) {
    ASTNode *node = module_ast->statements[i];
    if (!node || node->type != AST_IMPORT || !node->as.import.module_name ||
        !node->as.import.file_path) {
      continue;
    }

    char *child_resolved =
        lsp_resolve_module_path(resolved_path, node->as.import.file_path);
    if (!child_resolved) {
      continue;
    }

    bool has_issue = lsp_check_import_chain_recursive(
        node->as.import.module_name, node->as.import.file_path, child_resolved,
        stack, allow_disk_io, error_msg, error_msg_size);
    free(child_resolved);

    if (has_issue) {
      lsp_import_stack_pop(stack);
      return true;
    }
  }

  lsp_import_stack_pop(stack);
  return false;
}

static ExprType expr_type_from_annotation(const char *type_name) {
  if (!type_name) {
    return TYPE_UNKNOWN;
  }
  if (strcmp(type_name, "number") == 0) {
    return TYPE_NUMBER;
  }
  if (strcmp(type_name, "string") == 0) {
    return TYPE_STRING;
  }
  if (strcmp(type_name, "boolean") == 0 || strcmp(type_name, "bool") == 0) {
    return TYPE_BOOL;
  }
  if (strcmp(type_name, "null") == 0) {
    return TYPE_NULL;
  }
  if (strcmp(type_name, "range") == 0) {
    return TYPE_RANGE;
  }
  if (strncmp(type_name, "list", 4) == 0) {
    return TYPE_LIST;
  }
  if (strncmp(type_name, "map", 3) == 0) {
    return TYPE_MAP;
  }
  return TYPE_UNKNOWN;
}

static Symbol *lsp_find_symbol_for_var_node(const ASTNode *node) {
  if (!node || node->type != AST_VAR || !node->as.var_name) {
    return NULL;
  }

  if (node->line > 0 && node->column > 0) {
    Symbol *scoped = find_symbol_at_position(node->as.var_name, node->line - 1,
                                             node->column - 1);
    if (scoped) {
      return scoped;
    }
  }

  return find_symbol(node->as.var_name);
}

// Internal recursive version with depth tracking
static ExprType infer_type_internal(ASTNode *node, Symbol *symbols, AST *ast,
                                    int depth) {
  // Prevent infinite recursion (e.g., x = x + 1 where x references itself)
  if (depth > MAX_TYPE_INFER_DEPTH)
    return TYPE_UNKNOWN;

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
  case AST_LIST_COMPREHENSION:
    return TYPE_LIST;
  case AST_MAP:
    return TYPE_MAP;
  case AST_VAR: {
    Symbol *sym = lsp_find_symbol_for_var_node(node);
    if (sym && sym->type_name) {
      ExprType annotated = expr_type_from_annotation(sym->type_name);
      if (annotated != TYPE_UNKNOWN) {
        return annotated;
      }
    }
    // If no explicit type annotation, try to infer from assigned value
    if (ast) {
      ASTNode *assign_node = find_variable_assignment(ast, node->as.var_name);
      if (assign_node && assign_node->as.assign.value) {
        // Recursively infer type from the assigned value
        return infer_type_internal(assign_node->as.assign.value, symbols, ast,
                                   depth + 1);
      }
    }
    return TYPE_UNKNOWN;
  }
  case AST_BINOP:
    // Plus can return number (addition) or string (concatenation)
    if (node->as.binop.op == BINOP_ADD) {
      ExprType left_type =
          infer_type_internal(node->as.binop.left, symbols, ast, depth + 1);
      ExprType right_type =
          infer_type_internal(node->as.binop.right, symbols, ast, depth + 1);
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
        infer_type_internal(node->as.index.list_expr, symbols, ast, depth + 1);
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
        infer_type_internal(node->as.slice.list_expr, symbols, ast, depth + 1);
    return container_type;
  }
  default:
    return TYPE_UNKNOWN;
  }
}

// Public API wrapper - starts recursion with depth 0
ExprType infer_type_with_ast(ASTNode *node, Symbol *symbols, AST *ast) {
  return infer_type_internal(node, symbols, ast, 0);
}

static bool lsp_is_static_map_key_literal(ASTNode *node) {
  if (!node) {
    return false;
  }
  return node->type == AST_STRING || node->type == AST_NUMBER ||
         node->type == AST_BOOL || node->type == AST_NULL;
}

static bool lsp_map_key_literals_equal(ASTNode *left, ASTNode *right) {
  if (!left || !right || left->type != right->type) {
    return false;
  }

  switch (left->type) {
  case AST_STRING:
    return left->as.string.value && right->as.string.value &&
           strcmp(left->as.string.value, right->as.string.value) == 0;
  case AST_NUMBER:
    return left->as.number == right->as.number;
  case AST_BOOL:
    return left->as.boolean == right->as.boolean;
  case AST_NULL:
    return true;
  default:
    return false;
  }
}

static bool lsp_map_has_static_literal_key(ASTNode *map_node, ASTNode *key,
                                           bool *known, bool *present) {
  if (!known || !present || !map_node || map_node->type != AST_MAP ||
      !lsp_is_static_map_key_literal(key)) {
    return false;
  }

  *known = false;
  *present = false;

  for (size_t i = 0; i < map_node->as.map.entry_count; i++) {
    ASTNode *map_key = map_node->as.map.keys[i];
    if (!lsp_is_static_map_key_literal(map_key)) {
      return true;
    }
    if (lsp_map_key_literals_equal(map_key, key)) {
      *present = true;
    }
  }

  *known = true;
  return true;
}

static bool lsp_target_is_variable(ASTNode *target, const char *name) {
  return target && target->type == AST_VAR && target->as.var_name &&
         strcmp(target->as.var_name, name) == 0;
}

static bool lsp_stmt_has_non_linear_control_flow(const ASTNode *stmt) {
  if (!stmt) {
    return false;
  }

  switch (stmt->type) {
  case AST_IF:
  case AST_MATCH:
  case AST_WHILE:
  case AST_FOR:
  case AST_TRY:
    return true;
  default:
    return false;
  }
}

static bool lsp_delete_key_missing_in_static_map(ASTNode *target, ASTNode *key,
                                                 AST *ast,
                                                 size_t delete_stmt_index) {
  if (!target || !key || !ast || !lsp_is_static_map_key_literal(key)) {
    return false;
  }

  bool known = false;
  bool present = false;

  if (target->type == AST_MAP) {
    if (!lsp_map_has_static_literal_key(target, key, &known, &present)) {
      return false;
    }
    return known && !present;
  }

  if (target->type != AST_VAR || !target->as.var_name) {
    return false;
  }

  const char *target_name = target->as.var_name;
  bool state_known = false;
  bool key_present = false;

  // Track map key state from top-level statements before this delete.
  for (size_t i = 0; i < ast->count && i < delete_stmt_index; i++) {
    ASTNode *stmt = ast->statements[i];
    if (!stmt) {
      continue;
    }

    if (lsp_stmt_has_non_linear_control_flow(stmt)) {
      state_known = false;
      continue;
    }

    if (stmt->type == AST_ASSIGN && stmt->as.assign.name &&
        strcmp(stmt->as.assign.name, target_name) == 0) {
      if (stmt->as.assign.value &&
          lsp_map_has_static_literal_key(stmt->as.assign.value, key, &known,
                                         &present) &&
          known) {
        state_known = true;
        key_present = present;
      } else {
        state_known = false;
      }
      continue;
    }

    if (!state_known) {
      continue;
    }

    if (stmt->type == AST_ASSIGN_INDEX &&
        lsp_target_is_variable(stmt->as.assign_index.target, target_name)) {
      ASTNode *index = stmt->as.assign_index.index;
      if (!lsp_is_static_map_key_literal(index)) {
        state_known = false;
      } else if (lsp_map_key_literals_equal(index, key)) {
        key_present = true;
      }
      continue;
    }

    if (stmt->type == AST_DELETE &&
        lsp_target_is_variable(stmt->as.delete_stmt.target, target_name)) {
      ASTNode *deleted_key = stmt->as.delete_stmt.key;
      if (!lsp_is_static_map_key_literal(deleted_key)) {
        state_known = false;
      } else if (lsp_map_key_literals_equal(deleted_key, key)) {
        key_present = false;
      }
    }
  }

  return state_known && !key_present;
}

#define LSP_TYPE_MATCH_MAX_DEPTH 32

static char *lsp_trim_dup(const char *s, size_t len) {
  if (!s) {
    return NULL;
  }
  size_t start = 0;
  while (start < len && isspace((unsigned char)s[start])) {
    start++;
  }
  size_t end = len;
  while (end > start && isspace((unsigned char)s[end - 1])) {
    end--;
  }
  size_t out_len = end - start;
  char *out = malloc(out_len + 1);
  if (!out) {
    return NULL;
  }
  memcpy(out, s + start, out_len);
  out[out_len] = '\0';
  return out;
}

static bool lsp_find_top_level_char(const char *s, char target,
                                    size_t *idx_out) {
  if (!s) {
    return false;
  }

  int angle_depth = 0;
  int brace_depth = 0;
  size_t len = strlen(s);
  for (size_t i = 0; i < len; i++) {
    char c = s[i];
    if (c == '<') {
      angle_depth++;
      continue;
    }
    if (c == '>') {
      angle_depth--;
      continue;
    }
    if (c == '{') {
      brace_depth++;
      continue;
    }
    if (c == '}') {
      brace_depth--;
      continue;
    }
    if (c == target && angle_depth == 0 && brace_depth == 0) {
      if (idx_out) {
        *idx_out = i;
      }
      return true;
    }
  }
  return false;
}

static bool lsp_parse_generic_inner(const char *type_name, const char *base,
                                    char **inner_out) {
  if (!type_name || !base || !inner_out) {
    return false;
  }
  *inner_out = NULL;

  size_t base_len = strlen(base);
  size_t type_len = strlen(type_name);
  if (type_len <= base_len + 2) {
    return false;
  }
  if (strncmp(type_name, base, base_len) != 0 || type_name[base_len] != '<') {
    return false;
  }

  int depth = 1;
  size_t close_idx = SIZE_MAX;
  for (size_t i = base_len + 1; i < type_len; i++) {
    char c = type_name[i];
    if (c == '<') {
      depth++;
    } else if (c == '>') {
      depth--;
      if (depth == 0) {
        close_idx = i;
        break;
      }
      if (depth < 0) {
        return false;
      }
    }
  }

  if (close_idx == SIZE_MAX || close_idx != type_len - 1) {
    return false;
  }

  *inner_out =
      lsp_trim_dup(type_name + base_len + 1, close_idx - (base_len + 1));
  return *inner_out != NULL;
}

static bool lsp_type_annotations_compatible(const char *expected,
                                            const char *actual, int depth);

static bool lsp_try_union_compat(const char *container, const char *other,
                                 bool container_is_expected, int depth) {
  int angle_depth = 0;
  int brace_depth = 0;
  size_t start = 0;
  size_t len = strlen(container);
  bool saw_union = false;

  for (size_t i = 0; i < len; i++) {
    char c = container[i];
    if (c == '<') {
      angle_depth++;
      continue;
    }
    if (c == '>') {
      angle_depth--;
      continue;
    }
    if (c == '{') {
      brace_depth++;
      continue;
    }
    if (c == '}') {
      brace_depth--;
      continue;
    }
    if (angle_depth == 0 && brace_depth == 0 && c == 'o' && i + 1 < len &&
        container[i + 1] == 'r' &&
        (i == 0 || isspace((unsigned char)container[i - 1])) &&
        (i + 2 >= len || isspace((unsigned char)container[i + 2]))) {
      saw_union = true;
      char *segment = lsp_trim_dup(container + start, i - start);
      if (!segment) {
        return false;
      }
      bool ok = container_is_expected
                    ? lsp_type_annotations_compatible(segment, other, depth + 1)
                    : lsp_type_annotations_compatible(other, segment, depth + 1);
      free(segment);

      if (container_is_expected) {
        if (ok) {
          return true;
        }
      } else if (!ok) {
        return false;
      }

      i += 2;
      while (i < len && isspace((unsigned char)container[i])) {
        i++;
      }
      if (i > 0) {
        start = i;
        i--;
      } else {
        start = i;
      }
    }
  }

  if (!saw_union) {
    return false;
  }

  char *segment = lsp_trim_dup(container + start, len - start);
  if (!segment) {
    return false;
  }
  bool final_ok = container_is_expected
                      ? lsp_type_annotations_compatible(segment, other, depth + 1)
                      : lsp_type_annotations_compatible(other, segment, depth + 1);
  free(segment);
  return container_is_expected ? final_ok : final_ok;
}

static bool lsp_type_annotations_compatible(const char *expected,
                                            const char *actual, int depth) {
  if (!expected || !actual || depth > LSP_TYPE_MATCH_MAX_DEPTH) {
    return false;
  }

  char *exp = lsp_trim_dup(expected, strlen(expected));
  char *act = lsp_trim_dup(actual, strlen(actual));
  if (!exp || !act) {
    free(exp);
    free(act);
    return false;
  }

  if (strcmp(exp, act) == 0) {
    free(exp);
    free(act);
    return true;
  }

  if ((strcmp(exp, "bool") == 0 && strcmp(act, "boolean") == 0) ||
      (strcmp(exp, "boolean") == 0 && strcmp(act, "bool") == 0)) {
    free(exp);
    free(act);
    return true;
  }

  if (lsp_try_union_compat(exp, act, true, depth)) {
    free(exp);
    free(act);
    return true;
  }

  bool actual_union = false;
  int angle_depth = 0;
  int brace_depth = 0;
  size_t start = 0;
  size_t len = strlen(act);
  for (size_t i = 0; i < len; i++) {
    char c = act[i];
    if (c == '<') {
      angle_depth++;
      continue;
    }
    if (c == '>') {
      angle_depth--;
      continue;
    }
    if (c == '{') {
      brace_depth++;
      continue;
    }
    if (c == '}') {
      brace_depth--;
      continue;
    }
    if (angle_depth == 0 && brace_depth == 0 && c == 'o' && i + 1 < len &&
        act[i + 1] == 'r' &&
        (i == 0 || isspace((unsigned char)act[i - 1])) &&
        (i + 2 >= len || isspace((unsigned char)act[i + 2]))) {
      actual_union = true;
      char *segment = lsp_trim_dup(act + start, i - start);
      if (!segment) {
        free(exp);
        free(act);
        return false;
      }
      bool ok = lsp_type_annotations_compatible(exp, segment, depth + 1);
      free(segment);
      if (!ok) {
        free(exp);
        free(act);
        return false;
      }
      i += 2;
      while (i < len && isspace((unsigned char)act[i])) {
        i++;
      }
      if (i > 0) {
        start = i;
        i--;
      } else {
        start = i;
      }
    }
  }
  if (actual_union) {
    char *segment = lsp_trim_dup(act + start, len - start);
    if (!segment) {
      free(exp);
      free(act);
      return false;
    }
    bool ok = lsp_type_annotations_compatible(exp, segment, depth + 1);
    free(segment);
    free(exp);
    free(act);
    return ok;
  }

  // Generic list compatibility.
  char *exp_inner = NULL;
  char *act_inner = NULL;
  if (lsp_parse_generic_inner(exp, "list", &exp_inner) &&
      lsp_parse_generic_inner(act, "list", &act_inner)) {
    bool ok = lsp_type_annotations_compatible(exp_inner, act_inner, depth + 1);
    free(exp_inner);
    free(act_inner);
    free(exp);
    free(act);
    return ok;
  }
  free(exp_inner);
  free(act_inner);

  if (strcmp(exp, "list") == 0 && strncmp(act, "list<", 5) == 0) {
    free(exp);
    free(act);
    return true;
  }
  if (strcmp(exp, "map") == 0 &&
      (strncmp(act, "map<", 4) == 0 || strncmp(act, "map{", 4) == 0)) {
    free(exp);
    free(act);
    return true;
  }

  // Generic map compatibility.
  exp_inner = NULL;
  act_inner = NULL;
  if (lsp_parse_generic_inner(exp, "map", &exp_inner) &&
      lsp_parse_generic_inner(act, "map", &act_inner)) {
    size_t exp_comma = 0;
    size_t act_comma = 0;
    bool ok = false;
    if (lsp_find_top_level_char(exp_inner, ',', &exp_comma) &&
        lsp_find_top_level_char(act_inner, ',', &act_comma)) {
      char *exp_key = lsp_trim_dup(exp_inner, exp_comma);
      char *exp_val = lsp_trim_dup(exp_inner + exp_comma + 1,
                                   strlen(exp_inner) - exp_comma - 1);
      char *act_key = lsp_trim_dup(act_inner, act_comma);
      char *act_val = lsp_trim_dup(act_inner + act_comma + 1,
                                   strlen(act_inner) - act_comma - 1);
      if (exp_key && exp_val && act_key && act_val) {
        ok = lsp_type_annotations_compatible(exp_key, act_key, depth + 1) &&
             lsp_type_annotations_compatible(exp_val, act_val, depth + 1);
      }
      free(exp_key);
      free(exp_val);
      free(act_key);
      free(act_val);
    }
    free(exp_inner);
    free(act_inner);
    free(exp);
    free(act);
    return ok;
  }
  free(exp_inner);
  free(act_inner);

  free(exp);
  free(act);
  return false;
}

static bool lsp_expr_matches_type(ASTNode *node, const char *expected_type,
                                  Symbol *symbols, AST *ast, int depth) {
  (void)symbols;
  if (!node || !expected_type || depth > LSP_TYPE_MATCH_MAX_DEPTH) {
    return false;
  }

  char *expected = lsp_trim_dup(expected_type, strlen(expected_type));
  if (!expected) {
    return false;
  }

  // Union type support.
  int angle_depth = 0;
  int brace_depth = 0;
  size_t start = 0;
  size_t len = strlen(expected);
  bool saw_union = false;
  for (size_t i = 0; i < len; i++) {
    char c = expected[i];
    if (c == '<') {
      angle_depth++;
      continue;
    }
    if (c == '>') {
      angle_depth--;
      continue;
    }
    if (c == '{') {
      brace_depth++;
      continue;
    }
    if (c == '}') {
      brace_depth--;
      continue;
    }
    if (angle_depth == 0 && brace_depth == 0 && c == 'o' && i + 1 < len &&
        expected[i + 1] == 'r' &&
        (i == 0 || isspace((unsigned char)expected[i - 1])) &&
        (i + 2 >= len || isspace((unsigned char)expected[i + 2]))) {
      saw_union = true;
      char *segment = lsp_trim_dup(expected + start, i - start);
      if (!segment) {
        free(expected);
        return false;
      }
      bool ok = lsp_expr_matches_type(node, segment, symbols, ast, depth + 1);
      free(segment);
      if (ok) {
        free(expected);
        return true;
      }
      i += 2;
      while (i < len && isspace((unsigned char)expected[i])) {
        i++;
      }
      if (i > 0) {
        start = i;
        i--;
      } else {
        start = i;
      }
    }
  }
  if (saw_union) {
    char *segment = lsp_trim_dup(expected + start, len - start);
    bool ok =
        segment && lsp_expr_matches_type(node, segment, symbols, ast, depth + 1);
    free(segment);
    free(expected);
    return ok;
  }

  if (node->type == AST_VAR) {
    Symbol *sym = lsp_find_symbol_for_var_node(node);
    if (!sym || !sym->type_name) {
      free(expected);
      return true; // Unknown variable type; avoid false-positive diagnostics.
    }
    bool ok = lsp_type_annotations_compatible(expected, sym->type_name, depth + 1);
    free(expected);
    return ok;
  }

  char *inner = NULL;
  if (lsp_parse_generic_inner(expected, "list", &inner)) {
    bool ok = false;
    if (node->type == AST_LIST) {
      ok = true;
      for (size_t i = 0; i < node->as.list.element_count; i++) {
        if (!lsp_expr_matches_type(node->as.list.elements[i], inner, symbols,
                                   ast, depth + 1)) {
          ok = false;
          break;
        }
      }
    } else if (node->type == AST_LIST_COMPREHENSION) {
      ok = lsp_expr_matches_type(node->as.list_comprehension.element_expr, inner,
                                 symbols, ast, depth + 1);
    }
    free(inner);
    free(expected);
    return ok;
  }

  if (lsp_parse_generic_inner(expected, "map", &inner)) {
    bool ok = false;
    if (node->type == AST_MAP) {
      size_t comma_idx = 0;
      if (lsp_find_top_level_char(inner, ',', &comma_idx)) {
        char *key_type = lsp_trim_dup(inner, comma_idx);
        char *value_type =
            lsp_trim_dup(inner + comma_idx + 1, strlen(inner) - comma_idx - 1);
        if (key_type && value_type) {
          ok = true;
          for (size_t i = 0; i < node->as.map.entry_count; i++) {
            if (!lsp_expr_matches_type(node->as.map.keys[i], key_type, symbols,
                                       ast, depth + 1) ||
                !lsp_expr_matches_type(node->as.map.values[i], value_type,
                                       symbols, ast, depth + 1)) {
              ok = false;
              break;
            }
          }
        }
        free(key_type);
        free(value_type);
      }
    }
    free(inner);
    free(expected);
    return ok;
  }

  size_t expected_len = strlen(expected);
  if (expected_len >= 5 && strncmp(expected, "map{", 4) == 0 &&
      expected[expected_len - 1] == '}') {
    if (node->type != AST_MAP) {
      free(expected);
      return false;
    }
    char *fields = lsp_trim_dup(expected + 4, expected_len - 5);
    if (!fields) {
      free(expected);
      return false;
    }
    if (fields[0] == '\0') {
      free(fields);
      free(expected);
      return true;
    }

    bool ok = true;
    size_t fields_len = strlen(fields);
    size_t field_start = 0;
    int inner_angle = 0;
    int inner_brace = 0;
    for (size_t i = 0; i <= fields_len; i++) {
      char c = (i < fields_len) ? fields[i] : ',';
      if (i < fields_len) {
        if (c == '<') {
          inner_angle++;
          continue;
        }
        if (c == '>') {
          inner_angle--;
          continue;
        }
        if (c == '{') {
          inner_brace++;
          continue;
        }
        if (c == '}') {
          inner_brace--;
          continue;
        }
      }
      if (c == ',' && inner_angle == 0 && inner_brace == 0) {
        char *field = lsp_trim_dup(fields + field_start, i - field_start);
        size_t colon_idx = 0;
        if (!field || !lsp_find_top_level_char(field, ':', &colon_idx)) {
          free(field);
          ok = false;
          break;
        }
        char *field_name = lsp_trim_dup(field, colon_idx);
        char *field_type =
            lsp_trim_dup(field + colon_idx + 1, strlen(field) - colon_idx - 1);
        free(field);
        if (!field_name || !field_type || field_name[0] == '\0' ||
            field_type[0] == '\0') {
          free(field_name);
          free(field_type);
          ok = false;
          break;
        }

        bool found = false;
        for (size_t m = 0; m < node->as.map.entry_count; m++) {
          ASTNode *key = node->as.map.keys[m];
          if (key && key->type == AST_STRING && key->as.string.value &&
              strcmp(key->as.string.value, field_name) == 0) {
            found = true;
            if (!lsp_expr_matches_type(node->as.map.values[m], field_type,
                                       symbols, ast, depth + 1)) {
              ok = false;
            }
            break;
          }
        }
        free(field_name);
        free(field_type);
        if (!found || !ok) {
          ok = false;
          break;
        }
        field_start = i + 1;
      }
    }
    free(fields);
    free(expected);
    return ok;
  }

  ExprType inferred = infer_type_with_ast(node, symbols, ast);
  ExprType expected_expr = expr_type_from_annotation(expected);
  bool ok = false;
  if (expected_expr != TYPE_UNKNOWN) {
    ok = inferred == expected_expr;
  }
  free(expected);
  return ok;
}

static char *lsp_describe_expr_type(ASTNode *node, Symbol *symbols, AST *ast) {
  if (!node) {
    return strdup("unknown");
  }
  if (node->type == AST_VAR) {
    Symbol *sym = lsp_find_symbol_for_var_node(node);
    if (sym && sym->type_name) {
      return strdup(sym->type_name);
    }
  }

  switch (node->type) {
  case AST_NUMBER:
    return strdup("number");
  case AST_STRING:
  case AST_FSTRING:
    return strdup("string");
  case AST_BOOL:
    return strdup("boolean");
  case AST_NULL:
    return strdup("null");
  case AST_LIST:
  case AST_LIST_COMPREHENSION:
    return strdup("list");
  case AST_MAP:
    return strdup("map");
  case AST_RANGE:
    return strdup("range");
  default:
    break;
  }

  ExprType inferred = infer_type_with_ast(node, symbols, ast);
  switch (inferred) {
  case TYPE_NUMBER:
    return strdup("number");
  case TYPE_STRING:
    return strdup("string");
  case TYPE_LIST:
    return strdup("list");
  case TYPE_MAP:
    return strdup("map");
  case TYPE_RANGE:
    return strdup("range");
  case TYPE_BOOL:
    return strdup("boolean");
  case TYPE_NULL:
    return strdup("null");
  default:
    return strdup("unknown");
  }
}

void check_function_calls(AST *ast, const char *text, Symbol *symbols,
                          char **diagnostics, size_t *pos, size_t *remaining,
                          bool *has_diagnostics, size_t *capacity) {
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
          if (strcmp(module_name, "math") == 0 ||
              strcmp(module_name, "regex") == 0) {
            actual_func_name = dot + 1;
          } else if (is_module_imported(module_name)) {
            // File-based module - validate function exists
            ImportedModule *mod = g_doc ? g_doc->imported_modules : NULL;
            while (mod) {
              if (mod->name && strcmp(mod->name, module_name) == 0) {
                // Load exports if needed
                if (!mod->exports && mod->file_path) {
                  mod->exports = load_module_exports(mod->file_path);
                }

                // Find the function in exports
                Symbol *func_sym = mod->exports;
                bool found = false;
                while (func_sym) {
                  if (func_sym->type == SYMBOL_FUNCTION &&
                      strcmp(func_sym->name, actual_func_name) == 0) {
                    found = true;
                    // Validate argument count with default/variadic support
                    size_t required = func_sym->required_param_count;
                    size_t max_args = func_sym->has_variadic ? SIZE_MAX : func_sym->param_count;
                    bool arg_error = false;
                    char escaped_msg[LSP_ERROR_MSG_SIZE];

                    if (arg_count < required) {
                      arg_error = true;
                      snprintf(escaped_msg, sizeof(escaped_msg),
                          "Function '%s.%s' requires at least %zu argument%s, "
                          "but got %zu",
                          module_name, actual_func_name, required,
                          required == 1 ? "" : "s", arg_count);
                    } else if (arg_count > max_args) {
                      arg_error = true;
                      snprintf(escaped_msg, sizeof(escaped_msg),
                          "Function '%s.%s' accepts at most %zu argument%s, "
                          "but got %zu",
                          module_name, actual_func_name, func_sym->param_count,
                          func_sym->param_count == 1 ? "" : "s", arg_count);
                    }

                    if (arg_error) {
                      size_t line = 1, col = 0;
                      find_call_position(text, func_name, &line, &col);

                      char escaped_msg_final[LSP_ERROR_MSG_SIZE];
                      json_escape(escaped_msg, escaped_msg_final,
                                  sizeof(escaped_msg_final));

                      size_t needed = strlen(escaped_msg_final) + 200;
                      SAFE_DIAGNOSTICS_WRITE(
                          diagnostics, capacity, pos, remaining, needed,
                          "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":"
                          "%zu},"
                          "\"end\":{\"line\":%zu,\"character\":%zu}},"
                          "\"severity\":2,"
                          "\"message\":\"%s\"}",
                          *has_diagnostics ? "," : "", line - 1, col - 1,
                          line - 1, col + strlen(func_name), escaped_msg_final);
                      *has_diagnostics = true;
                    }
                    break;
                  }
                  func_sym = func_sym->next;
                }

                if (!found) {
                  // Function not found in module
                  size_t line = 1, col = 0;
                  find_call_position(text, func_name, &line, &col);

                  char escaped_msg[LSP_ERROR_MSG_SIZE];
                  snprintf(escaped_msg, sizeof(escaped_msg),
                           "Function '%s' not found in module '%s'",
                           actual_func_name, module_name);
                  char escaped_msg_final[LSP_ERROR_MSG_SIZE];
                  json_escape(escaped_msg, escaped_msg_final,
                              sizeof(escaped_msg_final));

                  size_t needed = strlen(escaped_msg_final) + 200;
                  SAFE_DIAGNOSTICS_WRITE(
                      diagnostics, capacity, pos, remaining, needed,
                      "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu}"
                      ","
                      "\"end\":{\"line\":%zu,\"character\":%zu}},"
                      "\"severity\":2,"
                      "\"message\":\"%s\"}",
                      *has_diagnostics ? "," : "", line - 1, col - 1, line - 1,
                      col + strlen(func_name), escaped_msg_final);
                  *has_diagnostics = true;
                }
                break;
              }
              mod = mod->next;
            }
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

          char escaped_msg[LSP_ERROR_MSG_SIZE];
          snprintf(escaped_msg, sizeof(escaped_msg),
                   "Function '%s' expects %d argument%s, but got %zu",
                   func_name, expected_args, expected_args == 1 ? "" : "s",
                   arg_count);
          char escaped_msg_final[LSP_ERROR_MSG_SIZE];
          json_escape(escaped_msg, escaped_msg_final,
                      sizeof(escaped_msg_final));

          size_t needed = strlen(escaped_msg_final) + 200;
          SAFE_DIAGNOSTICS_WRITE(
              diagnostics, capacity, pos, remaining, needed,
              "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
              "\"end\":{\"line\":%zu,\"character\":%zu}},"
              "\"severity\":1,"
              "\"message\":\"%s\"}",
              *has_diagnostics ? "," : "", line - 1, col, line - 1, col + 20,
              escaped_msg_final);
          *has_diagnostics = true;
        }
      } else if (expected_args == -2) {
        // Variable arguments - check at least 1
        if (arg_count < 1) {
          size_t line = 1, col = 0;
          find_call_position(text, func_name, &line, &col);

          char escaped_msg[LSP_ERROR_MSG_SIZE];
          snprintf(escaped_msg, sizeof(escaped_msg),
                   "Function '%s' expects at least 1 argument, but got %zu",
                   func_name, arg_count);
          char escaped_msg_final[LSP_ERROR_MSG_SIZE];
          json_escape(escaped_msg, escaped_msg_final,
                      sizeof(escaped_msg_final));

          size_t needed = strlen(escaped_msg_final) + 200;
          SAFE_DIAGNOSTICS_WRITE(
              diagnostics, capacity, pos, remaining, needed,
              "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
              "\"end\":{\"line\":%zu,\"character\":%zu}},"
              "\"severity\":1,"
              "\"message\":\"%s\"}",
              *has_diagnostics ? "," : "", line - 1, col, line - 1, col + 20,
              escaped_msg_final);
          *has_diagnostics = true;
        }
      } else {
        // Check user-defined functions
        Symbol *sym = find_symbol(func_name);
        if (sym && sym->type == SYMBOL_FUNCTION) {
          // Validate argument count with default/variadic support
          size_t required = sym->required_param_count;
          size_t max_args = sym->has_variadic ? SIZE_MAX : sym->param_count;
          bool arg_error = false;
          char escaped_msg[LSP_ERROR_MSG_SIZE];

          if (arg_count < required) {
            arg_error = true;
            snprintf(escaped_msg, sizeof(escaped_msg),
                     "Function '%s' requires at least %zu argument%s, but got %zu",
                     func_name, required,
                     required == 1 ? "" : "s", arg_count);
          } else if (arg_count > max_args) {
            arg_error = true;
            snprintf(escaped_msg, sizeof(escaped_msg),
                     "Function '%s' accepts at most %zu argument%s, but got %zu",
                     func_name, sym->param_count,
                     sym->param_count == 1 ? "" : "s", arg_count);
          }

          if (arg_error) {
            size_t line = 1, col = 0;
            find_call_position(text, func_name, &line, &col);

            char escaped_msg_final[LSP_ERROR_MSG_SIZE];
            json_escape(escaped_msg, escaped_msg_final,
                        sizeof(escaped_msg_final));

            size_t needed = strlen(escaped_msg_final) + 200;
            SAFE_DIAGNOSTICS_WRITE(
                diagnostics, capacity, pos, remaining, needed,
                "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
                "\"end\":{\"line\":%zu,\"character\":%zu}},"
                "\"severity\":1,"
                "\"message\":\"%s\"}",
                *has_diagnostics ? "," : "", line - 1, col, line - 1, col + 20,
                escaped_msg_final);
            *has_diagnostics = true;
          }
        } else if (!sym) {
          // Undefined function (not built-in and not user-defined)
          size_t line = 1, col = 0;
          find_call_position(text, func_name, &line, &col);

          char escaped_msg[LSP_ERROR_MSG_SIZE];
          snprintf(escaped_msg, sizeof(escaped_msg), "Undefined function '%s'",
                   func_name);
          char escaped_msg_final[LSP_ERROR_MSG_SIZE];
          json_escape(escaped_msg, escaped_msg_final,
                      sizeof(escaped_msg_final));

          size_t needed = strlen(escaped_msg_final) + 200;
          SAFE_DIAGNOSTICS_WRITE(
              diagnostics, capacity, pos, remaining, needed,
              "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
              "\"end\":{\"line\":%zu,\"character\":%zu}},"
              "\"severity\":1,"
              "\"message\":\"%s\"}",
              *has_diagnostics ? "," : "", line - 1, col, line - 1, col + 20,
              escaped_msg_final);
          *has_diagnostics = true;
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
                             remaining, has_diagnostics, capacity);
      }
      // Check else-if blocks
      for (size_t j = 0; j < node->as.if_stmt.else_if_count; j++) {
        if (node->as.if_stmt.else_if_blocks[j]) {
          AST temp_ast = {node->as.if_stmt.else_if_blocks[j],
                          node->as.if_stmt.else_if_block_sizes[j],
                          node->as.if_stmt.else_if_block_sizes[j]};
          check_function_calls(&temp_ast, text, symbols, diagnostics, pos,
                               remaining, has_diagnostics, capacity);
        }
      }
      // Check else block
      if (node->as.if_stmt.else_block) {
        AST temp_ast = {node->as.if_stmt.else_block,
                        node->as.if_stmt.else_block_size,
                        node->as.if_stmt.else_block_size};
        check_function_calls(&temp_ast, text, symbols, diagnostics, pos,
                             remaining, has_diagnostics, capacity);
      }
    } else if (node->type == AST_MATCH) {
      for (size_t j = 0; j < node->as.match_stmt.case_count; j++) {
        if (node->as.match_stmt.case_patterns[j]) {
          check_expression(node->as.match_stmt.case_patterns[j], text, symbols,
                           ast, diagnostics, pos, remaining, has_diagnostics,
                           NULL, 0, capacity);
        }
        if (node->as.match_stmt.case_blocks[j]) {
          AST temp_ast = {node->as.match_stmt.case_blocks[j],
                          node->as.match_stmt.case_block_sizes[j],
                          node->as.match_stmt.case_block_sizes[j]};
          check_function_calls(&temp_ast, text, symbols, diagnostics, pos,
                               remaining, has_diagnostics, capacity);
        }
      }
      if (node->as.match_stmt.default_block) {
        AST temp_ast = {node->as.match_stmt.default_block,
                        node->as.match_stmt.default_block_size,
                        node->as.match_stmt.default_block_size};
        check_function_calls(&temp_ast, text, symbols, diagnostics, pos,
                             remaining, has_diagnostics, capacity);
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
                             remaining, has_diagnostics, capacity);
      }
    } else if (node->type == AST_FUNCTION) {
      if (node->as.function.block) {
        AST temp_ast = {node->as.function.block, node->as.function.block_size,
                        node->as.function.block_size};
        check_function_calls(&temp_ast, text, symbols, diagnostics, pos,
                             remaining, has_diagnostics, capacity);
      }
    } else if (node->type == AST_LAMBDA) {
      // Check function calls in lambda body (multi-line lambdas)
      if (node->as.lambda.block) {
        AST temp_ast = {node->as.lambda.block, node->as.lambda.block_size,
                        node->as.lambda.block_size};
        check_function_calls(&temp_ast, text, symbols, diagnostics, pos,
                             remaining, has_diagnostics, capacity);
      }
    }
  }
}

static const SeenVar *find_seen_var(const SeenVar *seen_vars, size_t seen_count,
                                    const char *name) {
  if (!seen_vars || !name) {
    return NULL;
  }

  for (size_t i = 0; i < seen_count; i++) {
    if (seen_vars[i].name && strcmp(seen_vars[i].name, name) == 0) {
      return &seen_vars[i];
    }
  }
  return NULL;
}

static bool resolve_constant_number(ASTNode *node, AST *ast,
                                    const SeenVar *seen_vars,
                                    size_t seen_count, double *value,
                                    int depth) {
  if (!node || !value || depth > MAX_TYPE_INFER_DEPTH) {
    return false;
  }

  if (node->type == AST_NUMBER) {
    *value = node->as.number;
    return true;
  }

  if (node->type == AST_BINOP && node->as.binop.op == BINOP_NEG &&
      node->as.binop.right == NULL && node->as.binop.left) {
    double operand_value = 0.0;
    if (resolve_constant_number(node->as.binop.left, ast, seen_vars, seen_count,
                                &operand_value, depth + 1)) {
      *value = -operand_value;
      return true;
    }
    return false;
  }

  if (node->type == AST_VAR && ast && node->as.var_name) {
    const SeenVar *seen = find_seen_var(seen_vars, seen_count, node->as.var_name);
    if (!seen || seen->assignment_count != 1) {
      return false;
    }

    ASTNode *assignment = find_variable_assignment(ast, node->as.var_name);
    if (!assignment || !assignment->as.assign.value ||
        assignment->as.assign.value == node) {
      return false;
    }

    return resolve_constant_number(assignment->as.assign.value, ast, seen_vars,
                                   seen_count, value, depth + 1);
  }

  return false;
}

// Internal recursive version with depth tracking
static void check_expression_recursive(ASTNode *node, const char *text,
                                       Symbol *symbols, AST *ast,
                                       char **diagnostics, size_t *pos,
                                       size_t *remaining, bool *has_diagnostics,
                                       void *seen_vars_ptr, size_t seen_count,
                                       size_t *capacity, int depth) {
  // Prevent stack overflow from deeply nested AST structures
  if (depth > MAX_AST_DEPTH) {
    return;
  }

  SeenVar *seen_vars = (SeenVar *)seen_vars_ptr;
  if (!node)
    return;

  // Check index operations
  if (node->type == AST_INDEX) {
    // Mark list/string variable as read (indexing is a read operation)
    if (node->as.index.list_expr->type == AST_VAR) {
      Symbol *list_sym = lsp_find_symbol_for_var_node(node->as.index.list_expr);
      if (list_sym && list_sym->type == SYMBOL_VARIABLE) {
        list_sym->read = true;
        list_sym->read = true;
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

      char escaped_msg[LSP_ERROR_MSG_SIZE] =
          "Unsafe memory access: cannot index into null/undefined";
      char escaped_msg_final[LSP_ERROR_MSG_SIZE];
      json_escape(escaped_msg, escaped_msg_final, sizeof(escaped_msg_final));

      size_t needed = strlen(escaped_msg_final) + 200;
      SAFE_DIAGNOSTICS_WRITE(
          diagnostics, capacity, pos, remaining, needed,
          "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
          "\"end\":{\"line\":%zu,\"character\":%zu}},"
          "\"severity\":1,"
          "\"message\":\"%s\"}",
          *has_diagnostics ? "," : "", line - 1, col, line - 1, col + 20,
          escaped_msg_final);
      *has_diagnostics = true;
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

      char escaped_msg[LSP_ERROR_MSG_SIZE] =
          "List/string/range index must be a number";
      char escaped_msg_final[LSP_ERROR_MSG_SIZE];
      json_escape(escaped_msg, escaped_msg_final, sizeof(escaped_msg_final));

      size_t needed = strlen(escaped_msg_final) + 200;
      SAFE_DIAGNOSTICS_WRITE(
          diagnostics, capacity, pos, remaining, needed,
          "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
          "\"end\":{\"line\":%zu,\"character\":%zu}},"
          "\"severity\":1,"
          "\"message\":\"%s\"}",
          *has_diagnostics ? "," : "", line - 1, col, line - 1, col + 20,
          escaped_msg_final);
      *has_diagnostics = true;
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

          char escaped_msg[LSP_ERROR_MSG_SIZE] = "List index out of bounds";
          char escaped_msg_final[LSP_ERROR_MSG_SIZE];
          json_escape(escaped_msg, escaped_msg_final,
                      sizeof(escaped_msg_final));

          size_t needed = strlen(escaped_msg_final) + 200;
          SAFE_DIAGNOSTICS_WRITE(
              diagnostics, capacity, pos, remaining, needed,
              "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
              "\"end\":{\"line\":%zu,\"character\":%zu}},"
              "\"severity\":1,"
              "\"message\":\"%s\"}",
              *has_diagnostics ? "," : "", line - 1, col, line - 1, col + 20,
              escaped_msg_final);
          *has_diagnostics = true;
        }
      }
    }

    // Recursively check nested expressions
    check_expression_recursive(node->as.index.list_expr, text, symbols, ast,
                               diagnostics, pos, remaining, has_diagnostics,
                               NULL, 0, capacity, depth + 1);
    check_expression_recursive(node->as.index.index, text, symbols, ast,
                               diagnostics, pos, remaining, has_diagnostics,
                               NULL, 0, capacity, depth + 1);
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
          char pattern[LSP_PATTERN_BUFFER_SIZE];
          if (node->as.binop.op == BINOP_SUB)
            snprintf(pattern, sizeof(pattern), "minus");
          else if (node->as.binop.op == BINOP_MUL)
            snprintf(pattern, sizeof(pattern), "times");
          else if (node->as.binop.op == BINOP_DIV)
            snprintf(pattern, sizeof(pattern), "divided by");
          else if (node->as.binop.op == BINOP_MOD)
            snprintf(pattern, sizeof(pattern), "mod");
          find_node_position(node, text, pattern, &line, &col);

          char escaped_msg[LSP_ERROR_MSG_SIZE];
          snprintf(escaped_msg, sizeof(escaped_msg),
                   "Cannot %s - both values must be numbers", op_name);
          char escaped_msg_final[LSP_ERROR_MSG_SIZE];
          json_escape(escaped_msg, escaped_msg_final,
                      sizeof(escaped_msg_final));

          size_t needed = strlen(escaped_msg_final) + 200;
          SAFE_DIAGNOSTICS_WRITE(
              diagnostics, capacity, pos, remaining, needed,
              "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
              "\"end\":{\"line\":%zu,\"character\":%zu}},"
              "\"severity\":1,"
              "\"message\":\"%s\"}",
              *has_diagnostics ? "," : "", line - 1, col, line - 1, col + 20,
              escaped_msg_final);
          *has_diagnostics = true;
        }
      }

      // Check for division/modulo by zero when the right side resolves to a
      // compile-time constant (literal, unary-negated literal, or single-
      // assignment variable in scope).
      if (node->as.binop.op == BINOP_DIV || node->as.binop.op == BINOP_MOD) {
        double right_val = 0.0;
        if (resolve_constant_number(node->as.binop.right, ast, seen_vars,
                                    seen_count, &right_val, 0) &&
            right_val == 0.0) {
          size_t line = 1, col = 0;
          const bool is_division = node->as.binop.op == BINOP_DIV;
          find_node_position(node, text, is_division ? "divided by" : "mod",
                             &line, &col);

          char escaped_msg[LSP_ERROR_MSG_SIZE];
          snprintf(escaped_msg, sizeof(escaped_msg), "Cannot %s by zero",
                   is_division ? "divide" : "modulo");
          char escaped_msg_final[LSP_ERROR_MSG_SIZE];
          json_escape(escaped_msg, escaped_msg_final,
                      sizeof(escaped_msg_final));

          size_t needed = strlen(escaped_msg_final) + 200;
          SAFE_DIAGNOSTICS_WRITE(
              diagnostics, capacity, pos, remaining, needed,
              "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
              "\"end\":{\"line\":%zu,\"character\":%zu}},"
              "\"severity\":1,"
              "\"message\":\"%s\"}",
              *has_diagnostics ? "," : "", line - 1, col, line - 1, col + 20,
              escaped_msg_final);
          *has_diagnostics = true;
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
              char escaped_msg[LSP_ERROR_MSG_SIZE];
              snprintf(escaped_msg, sizeof(escaped_msg),
                       "Cannot negate - operand must be boolean");
              char escaped_msg_final[LSP_ERROR_MSG_SIZE];
              json_escape(escaped_msg, escaped_msg_final,
                          sizeof(escaped_msg_final));
              size_t needed = strlen(escaped_msg_final) + 200;
              SAFE_DIAGNOSTICS_WRITE(
                  diagnostics, capacity, pos, remaining, needed,
                  "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
                  "\"end\":{\"line\":%zu,\"character\":%zu}},"
                  "\"severity\":1,"
                  "\"message\":\"%s\"}",
                  *has_diagnostics ? "," : "", line - 1, col, line - 1, col + 3,
                  escaped_msg_final);
              *has_diagnostics = true;
            }
          } else if (node->as.binop.op == BINOP_NEG) {
            // NEG requires number operand
            if (operand_type != TYPE_NUMBER) {
              size_t line = 1, col = 0;
              find_node_position(node, text, "-", &line, &col);
              char escaped_msg[LSP_ERROR_MSG_SIZE];
              snprintf(escaped_msg, sizeof(escaped_msg),
                       "Cannot negate - operand must be a number");
              char escaped_msg_final[LSP_ERROR_MSG_SIZE];
              json_escape(escaped_msg, escaped_msg_final,
                          sizeof(escaped_msg_final));
              size_t needed = strlen(escaped_msg_final) + 200;
              SAFE_DIAGNOSTICS_WRITE(
                  diagnostics, capacity, pos, remaining, needed,
                  "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
                  "\"end\":{\"line\":%zu,\"character\":%zu}},"
                  "\"severity\":1,"
                  "\"message\":\"%s\"}",
                  *has_diagnostics ? "," : "", line - 1, col, line - 1, col + 1,
                  escaped_msg_final);
              *has_diagnostics = true;
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
          char pattern[LSP_PATTERN_BUFFER_SIZE] = "greater than";
          if (node->as.binop.op == BINOP_LT)
            snprintf(pattern, sizeof(pattern), "less than");
          else if (node->as.binop.op == BINOP_GTE)
            snprintf(pattern, sizeof(pattern), "greater than or equal to");
          else if (node->as.binop.op == BINOP_LTE)
            snprintf(pattern, sizeof(pattern), "less than or equal to");
          find_node_position(node, text, pattern, &line, &col);

          char escaped_msg[LSP_ERROR_MSG_SIZE] =
              "Cannot compare - both values must be numbers";
          char escaped_msg_final[LSP_ERROR_MSG_SIZE];
          json_escape(escaped_msg, escaped_msg_final,
                      sizeof(escaped_msg_final));

          size_t needed = strlen(escaped_msg_final) + 200;
          SAFE_DIAGNOSTICS_WRITE(
              diagnostics, capacity, pos, remaining, needed,
              "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
              "\"end\":{\"line\":%zu,\"character\":%zu}},"
              "\"severity\":1,"
              "\"message\":\"%s\"}",
              *has_diagnostics ? "," : "", line - 1, col, line - 1, col + 20,
              escaped_msg_final);
          *has_diagnostics = true;
        }
      }
    }

    // Recursively check nested expressions
    // For unary operators (NOT, NEG), right is NULL
    if (node->as.binop.right == NULL) {
      // Unary operator - only check left operand
      check_expression_recursive(node->as.binop.left, text, symbols, ast,
                                 diagnostics, pos, remaining, has_diagnostics,
                                 seen_vars, seen_count, capacity, depth + 1);
    } else {
      // Binary operator - check both operands
      check_expression_recursive(node->as.binop.left, text, symbols, ast,
                                 diagnostics, pos, remaining, has_diagnostics,
                                 seen_vars, seen_count, capacity, depth + 1);
      check_expression_recursive(node->as.binop.right, text, symbols, ast,
                                 diagnostics, pos, remaining, has_diagnostics,
                                 seen_vars, seen_count, capacity, depth + 1);
    }
    return;
  }

  // Check variables
  if (node->type == AST_VAR) {
    Symbol *sym = lsp_find_symbol_for_var_node(node);

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
      sym->read = true;
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
        char pattern[LSP_PATTERN_BUFFER_SIZE];
        snprintf(pattern, sizeof(pattern), "%s", node->as.var_name);
        find_node_position(node, text, pattern, &line, &col);

        char escaped_msg[LSP_ERROR_MSG_SIZE];
        snprintf(escaped_msg, sizeof(escaped_msg), "Undefined variable '%s'",
                 node->as.var_name);
        char escaped_msg_final[LSP_ERROR_MSG_SIZE];
        json_escape(escaped_msg, escaped_msg_final, sizeof(escaped_msg_final));

        size_t needed =
            strlen(escaped_msg_final) + strlen(node->as.var_name) + 200;
        SAFE_DIAGNOSTICS_WRITE(
            diagnostics, capacity, pos, remaining, needed,
            "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
            "\"end\":{\"line\":%zu,\"character\":%zu}},"
            "\"severity\":1,"
            "\"message\":\"%s\"}",
            *has_diagnostics ? "," : "", line - 1, col, line - 1,
            col + strlen(node->as.var_name), escaped_msg_final);
        *has_diagnostics = true;
      }
    }
    return;
  }

  // Check lists recursively
  if (node->type == AST_LIST) {
    for (size_t i = 0; i < node->as.list.element_count; i++) {
      check_expression_recursive(node->as.list.elements[i], text, symbols, ast,
                                 diagnostics, pos, remaining, has_diagnostics,
                                 seen_vars, seen_count, capacity, depth + 1);
    }
    return;
  }

  if (node->type == AST_LIST_COMPREHENSION) {
    if (node->as.list_comprehension.var) {
      Symbol *loop_sym = find_symbol(node->as.list_comprehension.var);
      if (loop_sym && loop_sym->type == SYMBOL_VARIABLE) {
        loop_sym->written = true;
      }
    }

    check_expression_recursive(node->as.list_comprehension.iterable, text,
                               symbols, ast, diagnostics, pos, remaining,
                               has_diagnostics, seen_vars, seen_count, capacity,
                               depth + 1);
    check_expression_recursive(node->as.list_comprehension.element_expr, text,
                               symbols, ast, diagnostics, pos, remaining,
                               has_diagnostics, seen_vars, seen_count, capacity,
                               depth + 1);
    check_expression_recursive(node->as.list_comprehension.condition, text,
                               symbols, ast, diagnostics, pos, remaining,
                               has_diagnostics, seen_vars, seen_count, capacity,
                               depth + 1);
    return;
  }

  // Check tuples recursively
  if (node->type == AST_TUPLE) {
    for (size_t i = 0; i < node->as.tuple.element_count; i++) {
      check_expression_recursive(node->as.tuple.elements[i], text, symbols, ast,
                                 diagnostics, pos, remaining, has_diagnostics,
                                 seen_vars, seen_count, capacity, depth + 1);
    }
    return;
  }

  // Check function calls recursively
  if (node->type == AST_CALL) {
    for (size_t i = 0; i < node->as.call.arg_count; i++) {
      check_expression_recursive(node->as.call.args[i], text, symbols, ast,
                                 diagnostics, pos, remaining, has_diagnostics,
                                 seen_vars, seen_count, capacity, depth + 1);
    }
    return;
  }
}

// Public wrapper that starts with depth 0
void check_expression(ASTNode *node, const char *text, Symbol *symbols,
                      AST *ast, char **diagnostics, size_t *pos,
                      size_t *remaining, bool *has_diagnostics,
                      void *seen_vars_ptr, size_t seen_count,
                      size_t *capacity) {
  check_expression_recursive(node, text, symbols, ast, diagnostics, pos,
                             remaining, has_diagnostics, seen_vars_ptr,
                             seen_count, capacity, 0);
}

static void check_import_diagnostics(AST *ast, const char *text,
                                     const char *uri, bool allow_disk_io,
                                     char **diagnostics, size_t *pos,
                                     size_t *remaining, bool *has_diagnostics,
                                     size_t *capacity) {
  if (!ast || !ast->statements || !text) {
    return;
  }

  char *current_file_path = lsp_uri_to_path(uri);

  for (size_t i = 0; i < ast->count; i++) {
    ASTNode *node = ast->statements[i];
    if (!node || node->type != AST_IMPORT || !node->as.import.module_name ||
        !node->as.import.file_path) {
      continue;
    }

    char *resolved_path =
        lsp_resolve_module_path(current_file_path, node->as.import.file_path);
    if (!resolved_path) {
      continue;
    }

    char error_msg[LSP_ERROR_MSG_SIZE] = {0};
    LSPImportStack stack = {0};
    bool has_import_error = lsp_check_import_chain_recursive(
        node->as.import.module_name, node->as.import.file_path, resolved_path,
        &stack, allow_disk_io, error_msg, sizeof(error_msg));

    lsp_import_stack_clear(&stack);
    free(resolved_path);

    if (!has_import_error || error_msg[0] == '\0') {
      continue;
    }

    size_t line = 1, col = 0;
    char pattern[LSP_PATTERN_BUFFER_SIZE];
    int n = snprintf(pattern, sizeof(pattern), "import %s",
                     node->as.import.module_name);
    if (n >= 0 && (size_t)n < sizeof(pattern)) {
      find_node_position(node, text, pattern, &line, &col);
    }

    if (line == 1 && col == 0) {
      get_node_position(node, &line, &col);
      if (col > 0) {
        col--;
      }
    }

    char escaped_msg[LSP_ERROR_MSG_SIZE];
    json_escape(error_msg, escaped_msg, sizeof(escaped_msg));

    size_t marker_len =
        (n > 0 && (size_t)n < sizeof(pattern)) ? (size_t)n : 20;
    size_t needed = strlen(escaped_msg) + marker_len + 200;
    SAFE_DIAGNOSTICS_WRITE(
        diagnostics, capacity, pos, remaining, needed,
        "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
        "\"end\":{\"line\":%zu,\"character\":%zu}},"
        "\"severity\":1,"
        "\"message\":\"%s\"}",
        *has_diagnostics ? "," : "", line - 1, col, line - 1, col + marker_len,
        escaped_msg);
    *has_diagnostics = true;
  }

  free(current_file_path);
}

void check_undefined_variables(AST *ast, const char *text, Symbol *symbols,
                               char **diagnostics, size_t *pos,
                               size_t *remaining, bool *has_diagnostics,
                               size_t *capacity) {
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
      Symbol *sym = lsp_find_symbol_for_var_node(node);

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
        sym->read = true;
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
          char pattern[LSP_PATTERN_BUFFER_SIZE];
          snprintf(pattern, sizeof(pattern), "%s", node->as.var_name);
          find_node_position(node, text, pattern, &line, &col);

          char escaped_msg[LSP_ERROR_MSG_SIZE];
          snprintf(escaped_msg, sizeof(escaped_msg), "Undefined variable '%s'",
                   node->as.var_name);
          char escaped_msg_final[LSP_ERROR_MSG_SIZE];
          json_escape(escaped_msg, escaped_msg_final,
                      sizeof(escaped_msg_final));

          size_t needed =
              strlen(escaped_msg_final) + strlen(node->as.var_name) + 200;
          SAFE_DIAGNOSTICS_WRITE(
              diagnostics, capacity, pos, remaining, needed,
              "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
              "\"end\":{\"line\":%zu,\"character\":%zu}},"
              "\"severity\":1,"
              "\"message\":\"%s\"}",
              *has_diagnostics ? "," : "", line - 1, col, line - 1,
              col + strlen(node->as.var_name), escaped_msg_final);
          *has_diagnostics = true;
        }
      }
    }

    // Check assignments for immutable reassignment
    if (node->type == AST_ASSIGN) {
      // Track whether this assignment targets an existing variable before we
      // mutate seen_vars, so reassignment checks don't treat first declarations
      // as reassignments.
      bool found_before = false;
      bool was_immutable_before = false;
      size_t occurrence = 0;
      for (size_t j = 0; j < seen_count; j++) {
        if (strcmp(seen_vars[j].name, node->as.assign.name) == 0) {
          found_before = true;
          was_immutable_before = !seen_vars[j].is_mutable;
          seen_vars[j].assignment_count++;
          occurrence = seen_vars[j].assignment_count;
          break;
        }
      }
      if (!found_before) {
        // Add new variable to seen_vars
        if (seen_count >= seen_capacity) {
          seen_capacity *= 2;
          SeenVar *new_vars =
              realloc(seen_vars, seen_capacity * sizeof(SeenVar));
          if (!new_vars) {
            // Can't expand - free existing array and return early
            // to prevent partial state and potential memory issues
            for (size_t k = 0; k < seen_count; k++) {
              free(seen_vars[k].name);
            }
            free(seen_vars);
            return;
          }
          seen_vars = new_vars;
        }
        seen_vars[seen_count].name = strdup(node->as.assign.name);
        if (!seen_vars[seen_count].name) {
          // strdup failed - free and return
          for (size_t k = 0; k < seen_count; k++) {
            free(seen_vars[k].name);
          }
          free(seen_vars);
          return;
        }
        seen_vars[seen_count].is_mutable = node->as.assign.is_mutable;
        seen_vars[seen_count].assignment_count = 1;
        seen_vars[seen_count].first_statement_index = i;
        occurrence = 1;
        seen_count++;
      }

      // Mark variable as written to (assignment)
      Symbol *assign_sym = find_symbol(node->as.assign.name);
      if (assign_sym && assign_sym->type == SYMBOL_VARIABLE) {
        assign_sym->read = true;
        assign_sym->written = true;
      }

      // Check if this is Pi (always immutable)
      if (strcmp(node->as.assign.name, "Pi") == 0) {
        size_t line = 1, col = 0;
        char pattern[LSP_PATTERN_BUFFER_SIZE];
        snprintf(pattern, sizeof(pattern), "set %s to", node->as.assign.name);
        find_node_position(node, text, pattern, &line, &col);

        char escaped_msg[LSP_ERROR_MSG_SIZE];
        snprintf(escaped_msg, sizeof(escaped_msg),
                 "Cannot reassign immutable variable '%s'",
                 node->as.assign.name);
        char escaped_msg_final[LSP_ERROR_MSG_SIZE];
        json_escape(escaped_msg, escaped_msg_final, sizeof(escaped_msg_final));

        size_t needed = strlen(escaped_msg_final) + 200;
        SAFE_DIAGNOSTICS_WRITE(
            diagnostics, capacity, pos, remaining, needed,
            "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
            "\"end\":{\"line\":%zu,\"character\":%zu}},"
            "\"severity\":1,"
            "\"message\":\"%s\"}",
            *has_diagnostics ? "," : "", line - 1, col, line - 1, col + 20,
            escaped_msg_final);
        *has_diagnostics = true;
      } else {
        // If variable was seen before and was immutable, this is an error
        if (found_before && was_immutable_before) {
          // Find the position of this specific assignment (the Nth occurrence)
          size_t line = 1, col = 0;
          if (!find_nth_occurrence(text, node->as.assign.name, occurrence,
                                   &line, &col)) {
            // Fallback: if we can't find the exact occurrence, use a simple
            // search This shouldn't happen, but provides a fallback
            char pattern[LSP_PATTERN_BUFFER_SIZE];
            snprintf(pattern, sizeof(pattern), "set %s to",
                     node->as.assign.name);
            find_node_position(node, text, pattern, &line, &col);
          }

          char escaped_msg[LSP_ERROR_MSG_SIZE];
          snprintf(escaped_msg, sizeof(escaped_msg),
                   "Cannot reassign immutable variable '%s'",
                   node->as.assign.name);
          char escaped_msg_final[LSP_ERROR_MSG_SIZE];
          json_escape(escaped_msg, escaped_msg_final,
                      sizeof(escaped_msg_final));

          size_t needed = strlen(escaped_msg_final) + 200;
          SAFE_DIAGNOSTICS_WRITE(
              diagnostics, capacity, pos, remaining, needed,
              "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
              "\"end\":{\"line\":%zu,\"character\":%zu}},"
              "\"severity\":1,"
              "\"message\":\"%s\"}",
              *has_diagnostics ? "," : "", line - 1, col, line - 1, col + 20,
              escaped_msg_final);
          *has_diagnostics = true;
        }

        // Check for type-annotated variables initialized with null/undefined
        // Numbers must have a value (cannot be null/undefined)
        // Strings and lists can be null/undefined (can be empty string/list
        // later)
        if (!found_before && node->as.assign.value &&
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
                char pattern[LSP_PATTERN_BUFFER_SIZE];
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

            char escaped_msg[LSP_ERROR_MSG_SIZE];
            snprintf(escaped_msg, sizeof(escaped_msg),
                     "Type mismatch for variable '%s': expected 'number', got "
                     "'%s'",
                     node->as.assign.name, null_type);
            char escaped_msg_final[LSP_ERROR_MSG_SIZE];
            json_escape(escaped_msg, escaped_msg_final,
                        sizeof(escaped_msg_final));

            size_t needed = strlen(escaped_msg_final) + value_length + 200;
            SAFE_DIAGNOSTICS_WRITE(
                diagnostics, capacity, pos, remaining, needed,
                "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
                "\"end\":{\"line\":%zu,\"character\":%zu}},"
                "\"severity\":1,"
                "\"message\":\"%s\"}",
                *has_diagnostics ? "," : "", line - 1, col, line - 1,
                col + value_length, escaped_msg_final);
            *has_diagnostics = true;
          }
        }

        // Check for type mismatch if this is a reassignment and variable has an
        // explicit type annotation (e.g., "as number")
        // Variables initialized with null or no value can be reassigned to any
        // type
        if (found_before && node->as.assign.value) {
          Symbol *sym = find_symbol(node->as.assign.name);
          if (sym && sym->type_name) {
            const char *expected_type = sym->type_name;
            bool matches = lsp_expr_matches_type(node->as.assign.value,
                                                 expected_type, symbols, ast, 0);

            if (!matches) {
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
                  char pattern[LSP_PATTERN_BUFFER_SIZE];
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

              char *actual_type_name =
                  lsp_describe_expr_type(node->as.assign.value, symbols, ast);
              if (!actual_type_name) {
                actual_type_name = strdup("unknown");
              }

              char escaped_msg[LSP_ERROR_MSG_SIZE];
              snprintf(escaped_msg, sizeof(escaped_msg),
                       "Type mismatch for variable '%s': expected '%s', got '%s'",
                       node->as.assign.name, expected_type, actual_type_name);
              char escaped_msg_final[LSP_ERROR_MSG_SIZE];
              json_escape(escaped_msg, escaped_msg_final,
                          sizeof(escaped_msg_final));

              size_t needed = strlen(escaped_msg_final) + value_length + 200;
              SAFE_DIAGNOSTICS_WRITE(
                  diagnostics, capacity, pos, remaining, needed,
                  "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
                  "\"end\":{\"line\":%zu,\"character\":%zu}},"
                  "\"severity\":1,"
                  "\"message\":\"%s\"}",
                  *has_diagnostics ? "," : "", line - 1, col, line - 1,
                  col + value_length, escaped_msg_final);
              *has_diagnostics = true;
              free(actual_type_name);
            }
          }
        }

        // Note: Variable already added to seen_vars at the start of AST_ASSIGN
        // handling (lines 2302-2333), no need to add again here
      }
      // Check expressions in assignment value
      if (node->as.assign.value) {
        check_expression(node->as.assign.value, text, symbols, ast, diagnostics,
                         pos, remaining, has_diagnostics, seen_vars, seen_count,
                         capacity);
      }
    }

    // Check unpacking assignments for immutable reassignment
    if (node->type == AST_UNPACK_ASSIGN) {
      // Add each unpacked variable to seen_vars
      for (size_t j = 0; j < node->as.unpack_assign.name_count; j++) {
        const char *var_name = node->as.unpack_assign.names[j];
        bool found = false;
        bool was_immutable = false;
        size_t occurrence = 0;

        for (size_t k = 0; k < seen_count; k++) {
          if (strcmp(seen_vars[k].name, var_name) == 0) {
            found = true;
            was_immutable = !seen_vars[k].is_mutable;
            occurrence = seen_vars[k].assignment_count;
            seen_vars[k].assignment_count++;
            break;
          }
        }

        // Add new variable to seen_vars if not found
        if (!found) {
          if (seen_count >= seen_capacity) {
            seen_capacity *= 2;
            SeenVar *new_vars =
                realloc(seen_vars, seen_capacity * sizeof(SeenVar));
            if (!new_vars) {
              for (size_t k = 0; k < seen_count; k++) {
                free(seen_vars[k].name);
              }
              free(seen_vars);
              return;
            }
            seen_vars = new_vars;
          }
          seen_vars[seen_count].name = strdup(var_name);
          if (!seen_vars[seen_count].name) {
            for (size_t k = 0; k < seen_count; k++) {
              free(seen_vars[k].name);
            }
            free(seen_vars);
            return;
          }
          seen_vars[seen_count].is_mutable = node->as.unpack_assign.is_mutable;
          seen_vars[seen_count].assignment_count = 1;
          seen_vars[seen_count].first_statement_index = i;
          seen_count++;
        }

        // Check for immutable variable reassignment
        if (found && was_immutable) {
          size_t line = 1, col = 0;
          if (!find_nth_occurrence(text, var_name, occurrence, &line, &col)) {
            char pattern[LSP_PATTERN_BUFFER_SIZE];
            snprintf(pattern, sizeof(pattern), "set %s", var_name);
            find_node_position(node, text, pattern, &line, &col);
          }

          char escaped_msg[LSP_ERROR_MSG_SIZE];
          snprintf(escaped_msg, sizeof(escaped_msg),
                   "Cannot reassign immutable variable '%s'", var_name);
          char escaped_msg_final[LSP_ERROR_MSG_SIZE];
          json_escape(escaped_msg, escaped_msg_final, sizeof(escaped_msg_final));

          size_t needed = strlen(escaped_msg_final) + 200;
          SAFE_DIAGNOSTICS_WRITE(
              diagnostics, capacity, pos, remaining, needed,
              "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
              "\"end\":{\"line\":%zu,\"character\":%zu}},"
              "\"severity\":1,"
              "\"message\":\"%s\"}",
              *has_diagnostics ? "," : "", line - 1, col, line - 1, col + 20,
              escaped_msg_final);
          *has_diagnostics = true;
        }

        // Mark variable as written
        Symbol *sym = find_symbol(var_name);
        if (sym && sym->type == SYMBOL_VARIABLE) {
          sym->written = true;
        }
      }
    }

    // Check expressions in print statements
    if (node->type == AST_PRINT) {
      if (node->as.print.value) {
        check_expression(node->as.print.value, text, symbols, ast, diagnostics,
                         pos, remaining, has_diagnostics, seen_vars, seen_count,
                         capacity);
      }
    }

    // Check expressions in debug statements
    if (node->type == AST_DEBUG) {
      for (size_t j = 0; j < node->as.debug_stmt.value_count; j++) {
        if (node->as.debug_stmt.values[j]) {
          check_expression(node->as.debug_stmt.values[j], text, symbols, ast,
                           diagnostics, pos, remaining, has_diagnostics,
                           seen_vars, seen_count, capacity);
        }
      }
    }

    // Check expressions in if conditions
    if (node->type == AST_IF) {
      if (node->as.if_stmt.condition) {
        check_expression(node->as.if_stmt.condition, text, symbols, ast,
                         diagnostics, pos, remaining, has_diagnostics,
                         seen_vars, seen_count, capacity);
      }
      // Check else-if conditions
      for (size_t j = 0; j < node->as.if_stmt.else_if_count; j++) {
        if (node->as.if_stmt.else_if_conditions[j]) {
          check_expression(node->as.if_stmt.else_if_conditions[j], text,
                           symbols, ast, diagnostics, pos, remaining,
                           has_diagnostics, seen_vars, seen_count, capacity);
        }
      }
    }

    if (node->type == AST_MATCH) {
      if (node->as.match_stmt.value) {
        check_expression(node->as.match_stmt.value, text, symbols, ast,
                         diagnostics, pos, remaining, has_diagnostics,
                         seen_vars, seen_count, capacity);
      }
      for (size_t j = 0; j < node->as.match_stmt.case_count; j++) {
        if (node->as.match_stmt.case_patterns[j]) {
          check_expression(node->as.match_stmt.case_patterns[j], text, symbols,
                           ast, diagnostics, pos, remaining, has_diagnostics,
                           seen_vars, seen_count, capacity);
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
                         seen_vars, seen_count, capacity);
      }
      if (node->as.for_stmt.end) {
        check_expression(node->as.for_stmt.end, text, symbols, ast, diagnostics,
                         pos, remaining, has_diagnostics, seen_vars, seen_count,
                         capacity);
      }
      if (node->as.for_stmt.step) {
        check_expression(node->as.for_stmt.step, text, symbols, ast,
                         diagnostics, pos, remaining, has_diagnostics,
                         seen_vars, seen_count, capacity);
      }
    }

    // Check expressions in while loops
    if (node->type == AST_WHILE) {
      if (node->as.while_stmt.condition) {
        check_expression(node->as.while_stmt.condition, text, symbols, ast,
                         diagnostics, pos, remaining, has_diagnostics,
                         seen_vars, seen_count, capacity);
      }
    }

    // Check expressions in return statements
    if (node->type == AST_RETURN) {
      for (size_t i = 0; i < node->as.return_stmt.value_count; i++) {
        if (node->as.return_stmt.values[i]) {
          check_expression(node->as.return_stmt.values[i], text, symbols, ast,
                           diagnostics, pos, remaining, has_diagnostics,
                           seen_vars, seen_count, capacity);
        }
      }
    }

    // Check expressions in unpacking assignments
    if (node->type == AST_UNPACK_ASSIGN) {
      if (node->as.unpack_assign.value) {
        check_expression(node->as.unpack_assign.value, text, symbols, ast,
                         diagnostics, pos, remaining, has_diagnostics,
                         seen_vars, seen_count, capacity);
      }
    }

    // Check expressions in index assignments
    if (node->type == AST_ASSIGN_INDEX) {
      // Check target, index, and value expressions
      if (node->as.assign_index.target) {
        check_expression(node->as.assign_index.target, text, symbols, ast,
                         diagnostics, pos, remaining, has_diagnostics,
                         seen_vars, seen_count, capacity);
      }
      if (node->as.assign_index.index) {
        check_expression(node->as.assign_index.index, text, symbols, ast,
                         diagnostics, pos, remaining, has_diagnostics,
                         seen_vars, seen_count, capacity);
      }
      if (node->as.assign_index.value) {
        check_expression(node->as.assign_index.value, text, symbols, ast,
                         diagnostics, pos, remaining, has_diagnostics,
                         seen_vars, seen_count, capacity);
      }
    }

    // Check expressions in delete statements
    if (node->type == AST_DELETE) {
      // Check target and key expressions
      if (node->as.delete_stmt.target) {
        check_expression(node->as.delete_stmt.target, text, symbols, ast,
                         diagnostics, pos, remaining, has_diagnostics,
                         seen_vars, seen_count, capacity);
      }
      if (node->as.delete_stmt.key) {
        check_expression(node->as.delete_stmt.key, text, symbols, ast,
                         diagnostics, pos, remaining, has_diagnostics,
                         seen_vars, seen_count, capacity);
      }

      if (lsp_delete_key_missing_in_static_map(node->as.delete_stmt.target,
                                               node->as.delete_stmt.key, ast,
                                               i)) {
        size_t line = 1, col = 0;
        find_node_position(node, text, "at", &line, &col);

        char escaped_msg[LSP_ERROR_MSG_SIZE] = "Map key not found";
        char escaped_msg_final[LSP_ERROR_MSG_SIZE];
        json_escape(escaped_msg, escaped_msg_final, sizeof(escaped_msg_final));

        size_t needed = strlen(escaped_msg_final) + 200;
        SAFE_DIAGNOSTICS_WRITE(
            diagnostics, capacity, pos, remaining, needed,
            "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
            "\"end\":{\"line\":%zu,\"character\":%zu}},"
            "\"severity\":1,"
            "\"message\":\"%s\"}",
            *has_diagnostics ? "," : "", line - 1, col, line - 1, col + 20,
            escaped_msg_final);
        *has_diagnostics = true;
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
                                  remaining, has_diagnostics, capacity);
      }
      // Check expressions in catch blocks
      for (size_t j = 0; j < node->as.try_stmt.catch_block_count; j++) {
        if (node->as.try_stmt.catch_blocks[j].catch_block) {
          AST catch_ast = {node->as.try_stmt.catch_blocks[j].catch_block,
                           node->as.try_stmt.catch_blocks[j].catch_block_size,
                           node->as.try_stmt.catch_blocks[j].catch_block_size};
          check_undefined_variables(&catch_ast, text, symbols, diagnostics, pos,
                                    remaining, has_diagnostics, capacity);
        }
      }
      // Check expressions in finally block
      if (node->as.try_stmt.finally_block) {
        AST finally_ast = {node->as.try_stmt.finally_block,
                           node->as.try_stmt.finally_block_size,
                           node->as.try_stmt.finally_block_size};
        check_undefined_variables(&finally_ast, text, symbols, diagnostics, pos,
                                  remaining, has_diagnostics, capacity);
      }
    }

    // Check expressions in raise statements
    if (node->type == AST_RAISE) {
      // Check error message expression
      if (node->as.raise_stmt.message) {
        check_expression(node->as.raise_stmt.message, text, symbols, ast,
                         diagnostics, pos, remaining, has_diagnostics,
                         seen_vars, seen_count, capacity);
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
          if (strcmp(module_name, "math") == 0 ||
              strcmp(module_name, "regex") == 0) {
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
          func_sym->read = true;
        }
      }

      // Check built-in function argument types
      if (strcmp(actual_func_name, "add") == 0 ||
          strcmp(actual_func_name, "subtract") == 0 ||
          strcmp(actual_func_name, "multiply") == 0 ||
          strcmp(actual_func_name, "divide") == 0 ||
          strcmp(actual_func_name, "power") == 0) {
        // These require number arguments
        size_t invalid_arg_indices[2] = {0, 0};
        ASTNode *invalid_arg_nodes[2] = {NULL, NULL};
        size_t invalid_arg_count = 0;
        for (size_t j = 0; j < node->as.call.arg_count && j < 2; j++) {
          ExprType arg_type =
              infer_type_with_ast(node->as.call.args[j], symbols, ast);
          if (arg_type != TYPE_NUMBER && arg_type != TYPE_UNKNOWN) {
            if (invalid_arg_count < 2) {
              invalid_arg_indices[invalid_arg_count] = j;
              invalid_arg_nodes[invalid_arg_count] = node->as.call.args[j];
              invalid_arg_count++;
            }
          }
        }

        if (invalid_arg_count > 0) {
          size_t line = 1, col = 0;
          size_t length = 20;
          bool have_specific_arg_range = false;

          if (invalid_arg_count == 1 && invalid_arg_nodes[0] != NULL) {
            ASTNodeType invalid_type = invalid_arg_nodes[0]->type;
            bool can_use_simple_arg_range =
                invalid_type == AST_STRING || invalid_type == AST_NUMBER ||
                invalid_type == AST_BOOL || invalid_type == AST_NULL ||
                invalid_type == AST_VAR;
            if (can_use_simple_arg_range) {
              have_specific_arg_range = find_call_argument_position_by_index(
                  text, func_name, node->line, invalid_arg_indices[0], &line,
                  &col, &length);
            }
          }

          if (!have_specific_arg_range || length == 0) {
            if (!find_call_expression_position(text, func_name, node->line,
                                               &line, &col, &length) ||
                length == 0) {
              find_call_position(text, func_name, &line, &col);
              length = 20;
            }
          }

          char escaped_msg[LSP_ERROR_MSG_SIZE];
          snprintf(escaped_msg, sizeof(escaped_msg),
                   "Function '%s' requires both arguments to be numbers",
                   func_name);
          char escaped_msg_final[LSP_ERROR_MSG_SIZE];
          json_escape(escaped_msg, escaped_msg_final,
                      sizeof(escaped_msg_final));

          size_t needed = strlen(escaped_msg_final) + 200;
          SAFE_DIAGNOSTICS_WRITE(
              diagnostics, capacity, pos, remaining, needed,
              "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
              "\"end\":{\"line\":%zu,\"character\":%zu}},"
              "\"severity\":1,"
              "\"message\":\"%s\"}",
              *has_diagnostics ? "," : "", line - 1, col, line - 1,
              col + length,
              escaped_msg_final);
          *has_diagnostics = true;
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

            char escaped_msg[LSP_ERROR_MSG_SIZE];
            snprintf(escaped_msg, sizeof(escaped_msg),
                     "Function '%s' requires a number argument", func_name);
            char escaped_msg_final[LSP_ERROR_MSG_SIZE];
            json_escape(escaped_msg, escaped_msg_final,
                        sizeof(escaped_msg_final));

            size_t needed = strlen(escaped_msg_final) + 200;
            SAFE_DIAGNOSTICS_WRITE(
                diagnostics, capacity, pos, remaining, needed,
                "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
                "\"end\":{\"line\":%zu,\"character\":%zu}},"
                "\"severity\":1,"
                "\"message\":\"%s\"}",
                *has_diagnostics ? "," : "", line - 1, col, line - 1, col + 20,
                escaped_msg_final);
            *has_diagnostics = true;
          }
        }
      } else if (strcmp(actual_func_name, "len") == 0) {
        // len accepts list, string, or range - no type error for any of these
        // No special validation needed here
        // node parameter not used in this branch - len accepts any type
        (void)node;
      } else if (strcmp(actual_func_name, "reverse") == 0 ||
                 strcmp(actual_func_name, "sort") == 0 ||
                 strcmp(actual_func_name, "filter") == 0 ||
                 strcmp(actual_func_name, "map") == 0) {
        // These require a list as the first argument (not string).
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

            char escaped_msg[LSP_ERROR_MSG_SIZE];
            snprintf(escaped_msg, sizeof(escaped_msg),
                     "Function '%s' requires a list argument", func_name);
            char escaped_msg_final[LSP_ERROR_MSG_SIZE];
            json_escape(escaped_msg, escaped_msg_final,
                        sizeof(escaped_msg_final));

            size_t needed = strlen(escaped_msg_final) + 200;
            SAFE_DIAGNOSTICS_WRITE(
                diagnostics, capacity, pos, remaining, needed,
                "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
                "\"end\":{\"line\":%zu,\"character\":%zu}},"
                "\"severity\":1,"
                "\"message\":\"%s\"}",
                *has_diagnostics ? "," : "", line - 1, col, line - 1, col + 20,
                escaped_msg_final);
            *has_diagnostics = true;
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

                char escaped_msg[LSP_ERROR_MSG_SIZE];
                snprintf(escaped_msg, sizeof(escaped_msg),
                         "Function 'sort' requires list items to be all "
                         "numbers or all strings");
                char escaped_msg_final[LSP_ERROR_MSG_SIZE];
                json_escape(escaped_msg, escaped_msg_final,
                            sizeof(escaped_msg_final));

                size_t needed = strlen(escaped_msg_final) + 200;
                SAFE_DIAGNOSTICS_WRITE(
                    diagnostics, capacity, pos, remaining, needed,
                    "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
                    "\"end\":{\"line\":%zu,\"character\":%zu}},"
                    "\"severity\":1,"
                    "\"message\":\"%s\"}",
                    *has_diagnostics ? "," : "", line - 1, col, line - 1,
                    col + 20, escaped_msg_final);
                *has_diagnostics = true;
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

            char escaped_msg[LSP_ERROR_MSG_SIZE];
            snprintf(escaped_msg, sizeof(escaped_msg),
                     "Function '%s' requires a string argument", func_name);
            char escaped_msg_final[LSP_ERROR_MSG_SIZE];
            json_escape(escaped_msg, escaped_msg_final,
                        sizeof(escaped_msg_final));

            size_t needed = strlen(escaped_msg_final) + 200;
            SAFE_DIAGNOSTICS_WRITE(
                diagnostics, capacity, pos, remaining, needed,
                "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
                "\"end\":{\"line\":%zu,\"character\":%zu}},"
                "\"severity\":1,"
                "\"message\":\"%s\"}",
                *has_diagnostics ? "," : "", line - 1, col, line - 1, col + 20,
                escaped_msg_final);
            *has_diagnostics = true;
          }
        }
      } else if (strcmp(actual_func_name, "read_file") == 0 ||
                 strcmp(actual_func_name, "read_lines") == 0 ||
                 strcmp(actual_func_name, "file_exists") == 0) {
        // These require string argument.
        if (node->as.call.arg_count > 0) {
          ExprType arg_type =
              infer_type_with_ast(node->as.call.args[0], symbols, ast);
          if (arg_type != TYPE_STRING && arg_type != TYPE_UNKNOWN) {
            size_t line = 1, col = 0, length = 0;
            if (!find_call_argument_position_by_index(text, func_name, node->line,
                                                      0, &line, &col, &length) ||
                length == 0) {
              if (!find_call_expression_position(text, func_name, node->line,
                                                 &line, &col, &length) ||
                  length == 0) {
                find_call_position(text, func_name, &line, &col);
                length = 20;
              }
            }

            char escaped_msg[LSP_ERROR_MSG_SIZE];
            snprintf(escaped_msg, sizeof(escaped_msg),
                     "Function '%s' requires a string argument", func_name);
            char escaped_msg_final[LSP_ERROR_MSG_SIZE];
            json_escape(escaped_msg, escaped_msg_final,
                        sizeof(escaped_msg_final));

            size_t needed = strlen(escaped_msg_final) + length + 200;
            SAFE_DIAGNOSTICS_WRITE(
                diagnostics, capacity, pos, remaining, needed,
                "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
                "\"end\":{\"line\":%zu,\"character\":%zu}},"
                "\"severity\":1,"
                "\"message\":\"%s\"}",
                *has_diagnostics ? "," : "", line - 1, col, line - 1,
                col + length, escaped_msg_final);
            *has_diagnostics = true;
          }
        }
      } else if (strcmp(actual_func_name, "write_file") == 0) {
        // write_file requires two string arguments.
        size_t invalid_arg_indices[2] = {0, 0};
        size_t invalid_arg_count = 0;
        for (size_t j = 0; j < node->as.call.arg_count && j < 2; j++) {
          ExprType arg_type =
              infer_type_with_ast(node->as.call.args[j], symbols, ast);
          if (arg_type != TYPE_STRING && arg_type != TYPE_UNKNOWN) {
            if (invalid_arg_count < 2) {
              invalid_arg_indices[invalid_arg_count] = j;
              invalid_arg_count++;
            }
          }
        }

        if (invalid_arg_count > 0) {
          size_t line = 1, col = 0, length = 20;
          bool have_specific_arg_range = false;

          if (invalid_arg_count == 1) {
            have_specific_arg_range = find_call_argument_position_by_index(
                text, func_name, node->line, invalid_arg_indices[0], &line, &col,
                &length);
          }

          if (!have_specific_arg_range || length == 0) {
            if (!find_call_expression_position(text, func_name, node->line, &line,
                                               &col, &length) ||
                length == 0) {
              find_call_position(text, func_name, &line, &col);
              length = 20;
            }
          }

          char escaped_msg[LSP_ERROR_MSG_SIZE];
          snprintf(escaped_msg, sizeof(escaped_msg),
                   "Function '%s' requires two string arguments", func_name);
          char escaped_msg_final[LSP_ERROR_MSG_SIZE];
          json_escape(escaped_msg, escaped_msg_final,
                      sizeof(escaped_msg_final));

          size_t needed = strlen(escaped_msg_final) + length + 200;
          SAFE_DIAGNOSTICS_WRITE(
              diagnostics, capacity, pos, remaining, needed,
              "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
              "\"end\":{\"line\":%zu,\"character\":%zu}},"
              "\"severity\":1,"
              "\"message\":\"%s\"}",
              *has_diagnostics ? "," : "", line - 1, col, line - 1, col + length,
              escaped_msg_final);
          *has_diagnostics = true;
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

            char escaped_msg[LSP_ERROR_MSG_SIZE];
            snprintf(escaped_msg, sizeof(escaped_msg),
                     "to_number requires string or number");
            char escaped_msg_final[LSP_ERROR_MSG_SIZE];
            json_escape(escaped_msg, escaped_msg_final,
                        sizeof(escaped_msg_final));

            size_t needed = strlen(escaped_msg_final) + arg_length + 200;
            SAFE_DIAGNOSTICS_WRITE(
                diagnostics, capacity, pos, remaining, needed,
                "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
                "\"end\":{\"line\":%zu,\"character\":%zu}},"
                "\"severity\":1,"
                "\"message\":\"%s\"}",
                *has_diagnostics ? "," : "", line - 1, col, line - 1,
                col + arg_length, escaped_msg_final);
            *has_diagnostics = true;
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

                char escaped_msg[LSP_ERROR_MSG_SIZE];
                snprintf(escaped_msg, sizeof(escaped_msg),
                         "cannot convert string to number");
                char escaped_msg_final[LSP_ERROR_MSG_SIZE];
                json_escape(escaped_msg, escaped_msg_final,
                            sizeof(escaped_msg_final));

                size_t needed = strlen(escaped_msg_final) + arg_length + 200;
                SAFE_DIAGNOSTICS_WRITE(
                    diagnostics, capacity, pos, remaining, needed,
                    "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
                    "\"end\":{\"line\":%zu,\"character\":%zu}},"
                    "\"severity\":1,"
                    "\"message\":\"%s\"}",
                    *has_diagnostics ? "," : "", line - 1, col, line - 1,
                    col + arg_length, escaped_msg_final);
                *has_diagnostics = true;
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

                  char escaped_msg[LSP_ERROR_MSG_SIZE];
                  snprintf(escaped_msg, sizeof(escaped_msg),
                           "cannot convert string to number");
                  char escaped_msg_final[LSP_ERROR_MSG_SIZE];
                  json_escape(escaped_msg, escaped_msg_final,
                              sizeof(escaped_msg_final));

                  size_t needed = strlen(escaped_msg_final) + arg_length + 200;
                  SAFE_DIAGNOSTICS_WRITE(
                      diagnostics, capacity, pos, remaining, needed,
                      "%s{\"range\":{\"start\":{\"line\":%zu,"
                      "\"character\":%zu},"
                      "\"end\":{\"line\":%zu,\"character\":%zu}},"
                      "\"severity\":1,"
                      "\"message\":\"%s\"}",
                      *has_diagnostics ? "," : "", line - 1, col, line - 1,
                      col + arg_length, escaped_msg_final);
                  *has_diagnostics = true;
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

            char escaped_msg[LSP_ERROR_MSG_SIZE];
            snprintf(escaped_msg, sizeof(escaped_msg),
                     "cannot convert type to boolean");
            char escaped_msg_final[LSP_ERROR_MSG_SIZE];
            json_escape(escaped_msg, escaped_msg_final,
                        sizeof(escaped_msg_final));

            size_t needed = strlen(escaped_msg_final) + arg_length + 200;
            SAFE_DIAGNOSTICS_WRITE(
                diagnostics, capacity, pos, remaining, needed,
                "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
                "\"end\":{\"line\":%zu,\"character\":%zu}},"
                "\"severity\":1,"
                "\"message\":\"%s\"}",
                *has_diagnostics ? "," : "", line - 1, col, line - 1,
                col + arg_length, escaped_msg_final);
            *has_diagnostics = true;
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

          char escaped_msg[LSP_ERROR_MSG_SIZE] =
              "Function 'sqrt' cannot take negative argument";
          char escaped_msg_final[LSP_ERROR_MSG_SIZE];
          json_escape(escaped_msg, escaped_msg_final,
                      sizeof(escaped_msg_final));

          size_t needed = strlen(escaped_msg_final) + 200;
          SAFE_DIAGNOSTICS_WRITE(
              diagnostics, capacity, pos, remaining, needed,
              "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
              "\"end\":{\"line\":%zu,\"character\":%zu}},"
              "\"severity\":1,"
              "\"message\":\"%s\"}",
              *has_diagnostics ? "," : "", line - 1, col, line - 1, col + 20,
              escaped_msg_final);
          *has_diagnostics = true;
        }
      }
    }

    // Recursively check nested structures
    if (node->type == AST_IF) {
      if (node->as.if_stmt.block) {
        AST temp_ast = {node->as.if_stmt.block, node->as.if_stmt.block_size,
                        node->as.if_stmt.block_size};
        check_undefined_variables(&temp_ast, text, symbols, diagnostics, pos,
                                  remaining, has_diagnostics, capacity);
      }
      for (size_t j = 0; j < node->as.if_stmt.else_if_count; j++) {
        if (node->as.if_stmt.else_if_blocks[j]) {
          AST temp_ast = {node->as.if_stmt.else_if_blocks[j],
                          node->as.if_stmt.else_if_block_sizes[j],
                          node->as.if_stmt.else_if_block_sizes[j]};
          check_undefined_variables(&temp_ast, text, symbols, diagnostics, pos,
                                    remaining, has_diagnostics, capacity);
        }
      }
      if (node->as.if_stmt.else_block) {
        AST temp_ast = {node->as.if_stmt.else_block,
                        node->as.if_stmt.else_block_size,
                        node->as.if_stmt.else_block_size};
        check_undefined_variables(&temp_ast, text, symbols, diagnostics, pos,
                                  remaining, has_diagnostics, capacity);
      }
    } else if (node->type == AST_MATCH) {
      for (size_t j = 0; j < node->as.match_stmt.case_count; j++) {
        if (node->as.match_stmt.case_blocks[j]) {
          AST temp_ast = {node->as.match_stmt.case_blocks[j],
                          node->as.match_stmt.case_block_sizes[j],
                          node->as.match_stmt.case_block_sizes[j]};
          check_undefined_variables(&temp_ast, text, symbols, diagnostics, pos,
                                    remaining, has_diagnostics, capacity);
        }
      }
      if (node->as.match_stmt.default_block) {
        AST temp_ast = {node->as.match_stmt.default_block,
                        node->as.match_stmt.default_block_size,
                        node->as.match_stmt.default_block_size};
        check_undefined_variables(&temp_ast, text, symbols, diagnostics, pos,
                                  remaining, has_diagnostics, capacity);
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
                                  remaining, has_diagnostics, capacity);
      }
    } else if (node->type == AST_FUNCTION) {
      if (node->as.function.block) {
        AST temp_ast = {node->as.function.block, node->as.function.block_size,
                        node->as.function.block_size};
        check_undefined_variables(&temp_ast, text, symbols, diagnostics, pos,
                                  remaining, has_diagnostics, capacity);
      }
    } else if (node->type == AST_LAMBDA) {
      // Check undefined variables in lambda body (multi-line lambdas)
      if (node->as.lambda.block) {
        AST temp_ast = {node->as.lambda.block, node->as.lambda.block_size,
                        node->as.lambda.block_size};
        check_undefined_variables(&temp_ast, text, symbols, diagnostics, pos,
                                  remaining, has_diagnostics, capacity);
      }
    }
  }

  // Free seen_vars
  for (size_t i = 0; i < seen_count; i++) {
    free(seen_vars[i].name);
  }
  free(seen_vars);
}

void check_unused_symbols(Symbol *symbols, const char *text, AST *ast,
                          char **diagnostics, size_t *pos, size_t *remaining,
                          bool *has_diagnostics, size_t *capacity) {
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
      char pattern[LSP_PATTERN_BUFFER_SIZE];
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

      char escaped_msg[LSP_ERROR_MSG_SIZE];
      snprintf(escaped_msg, sizeof(escaped_msg),
               "Variable '%s' is defined but never read (memory allocation not "
               "utilized)",
               sym->name);
      char escaped_msg_final[LSP_ERROR_MSG_SIZE];
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

      size_t needed = strlen(escaped_msg_final) + strlen(sym->name) + 200;
      SAFE_DIAGNOSTICS_WRITE(
          diagnostics, capacity, pos, remaining, needed,
          "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
          "\"end\":{\"line\":%zu,\"character\":%zu}},"
          "\"severity\":1,"
          "\"message\":\"%s\"}",
          *has_diagnostics ? "," : "", line - 1, var_col, line - 1,
          var_col + strlen(sym->name), escaped_msg_final);
      *has_diagnostics = true;
    }

    // Check for completely unused variables and functions (not written or read)
    // Skip variables that are written (they're used, just not read - already
    // reported above)
    if (!sym->read && !sym->written &&
        (sym->type == SYMBOL_VARIABLE || sym->type == SYMBOL_FUNCTION)) {
      // Find the actual position of the symbol in source text
      size_t line = 1, col = 0;

      if (sym->type == SYMBOL_FUNCTION) {
        // Find function declaration: "function <name> with"
        char pattern[LSP_PATTERN_BUFFER_SIZE];
        snprintf(pattern, sizeof(pattern), "function %s with", sym->name);
        find_node_position(NULL, text, pattern, &line, &col);
        // If not found, try without "with" (function might not have parameters)
        if (line == 1 && col == 0) {
          snprintf(pattern, sizeof(pattern), "function %s", sym->name);
          find_node_position(NULL, text, pattern, &line, &col);
        }
      } else {
        // Find variable declaration: "let <name> to" or "set <name> to"
        char pattern[LSP_PATTERN_BUFFER_SIZE];
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
      char escaped_msg[LSP_ERROR_MSG_SIZE];
      snprintf(escaped_msg, sizeof(escaped_msg), "Unused %s '%s'", symbol_type,
               sym->name);
      char escaped_msg_final[LSP_ERROR_MSG_SIZE];
      json_escape(escaped_msg, escaped_msg_final, sizeof(escaped_msg_final));

      size_t needed = strlen(escaped_msg_final) + strlen(sym->name) + 200;
      SAFE_DIAGNOSTICS_WRITE(
          diagnostics, capacity, pos, remaining, needed,
          "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
          "\"end\":{\"line\":%zu,\"character\":%zu}},"
          "\"severity\":2,"
          "\"message\":\"%s\"}",
          *has_diagnostics ? "," : "", line - 1, name_col, line - 1,
          name_col + strlen(sym->name), escaped_msg_final);
      *has_diagnostics = true;
    }
  }
}

void check_diagnostics(const char *uri, const char *text,
                       bool allow_blocking_import_io) {
  lsp_module_cache_update_document_source(uri, text);

  TokenizeError *tokenize_err = NULL;
  TokenArray *tokens = tokenize(text, &tokenize_err);

  // Start with a reasonable initial size
  size_t diagnostics_capacity = LSP_INITIAL_BUFFER_SIZE;
  char *diagnostics = malloc(diagnostics_capacity);
  if (!diagnostics) {
    if (tokenize_err)
      tokenize_error_free(tokenize_err);
    return;
  }

  size_t pos = 0;
  size_t remaining = diagnostics_capacity;

  // Start building JSON
  int written = snprintf(diagnostics + pos, remaining,
                         "{\"uri\":\"%s\",\"diagnostics\":[", uri);
  if (written < 0) {
    free(diagnostics);
    if (tokenize_err)
      tokenize_error_free(tokenize_err);
    return;
  }

  // Grow buffer if needed
  if ((size_t)written >= remaining) {
    if (!grow_diagnostics_buffer(&diagnostics, &diagnostics_capacity, pos,
                                 (size_t)written + 1)) {
      free(diagnostics);
      if (tokenize_err)
        tokenize_error_free(tokenize_err);
      return;
    }
    remaining = diagnostics_capacity - pos;
    // Retry snprintf with larger buffer
    written = snprintf(diagnostics + pos, remaining,
                       "{\"uri\":\"%s\",\"diagnostics\":[", uri);
    if (written < 0 || (size_t)written >= remaining) {
      free(diagnostics);
      if (tokenize_err)
        tokenize_error_free(tokenize_err);
      return;
    }
  }

  pos += (size_t)written;
  remaining = diagnostics_capacity - pos;

  bool has_diagnostics = false;

  // Check tokenization errors
  if (tokenize_err) {
    // Validate line/column to prevent integer overflow
    size_t line = (tokenize_err->line > 0 && tokenize_err->line <= SIZE_MAX)
                      ? tokenize_err->line - 1
                      : 0;
    size_t col = (tokenize_err->column > 0 && tokenize_err->column <= SIZE_MAX)
                     ? tokenize_err->column - 1
                     : 0;

    char escaped_msg[LSP_ERROR_MSG_SIZE];
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
    ast = parse(tokens, NULL);
    if (!ast || ast->count == 0) {
      // Parse error - use first token position as estimate
      if (tokens->count > 0) {
        size_t line = 1, col = 1;
        // Estimate position (tokens don't store line/column, so we estimate)
        size_t needed = 200; // Estimate for error message
        if (grow_diagnostics_buffer(&diagnostics, &diagnostics_capacity, pos,
                                    needed)) {
          remaining = diagnostics_capacity - pos;
          written = snprintf(
              diagnostics + pos, remaining,
              "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
              "\"end\":{\"line\":%zu,\"character\":%zu}},"
              "\"severity\":1,"
              "\"message\":\"Syntax error: failed to parse\"}",
              has_diagnostics ? "," : "", line - 1, col - 1, line - 1, col);
          if (written > 0 && (size_t)written < remaining) {
            pos += (size_t)written;
            remaining = diagnostics_capacity - pos;
            has_diagnostics = true;
          }
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
      // Pass capacity pointer so helper functions can grow buffer
      size_t *capacity_ptr = &diagnostics_capacity;

      // Check import-related diagnostics (missing files, circular imports)
      check_import_diagnostics(ast, text, uri, allow_blocking_import_io,
                               &diagnostics, &pos, &remaining, &has_diagnostics,
                               capacity_ptr);

      check_function_calls(ast, text, symbols, &diagnostics, &pos, &remaining,
                           &has_diagnostics, capacity_ptr);

      // Check for undefined variables, type errors, and other diagnostics
      check_undefined_variables(ast, text, symbols, &diagnostics, &pos,
                                &remaining, &has_diagnostics, capacity_ptr);

      // Check for unused variables and functions
      check_unused_symbols(symbols, text, ast, &diagnostics, &pos, &remaining,
                           &has_diagnostics, capacity_ptr);

      // Free AST if not stored in document
      if (!g_doc) {
        ast_free(ast);
      }
    }
    token_array_free(tokens);
  }

  // Close the JSON array and object
  if (grow_diagnostics_buffer(&diagnostics, &diagnostics_capacity, pos, 10)) {
    remaining = diagnostics_capacity - pos;
    written = snprintf(diagnostics + pos, remaining, "]}");
    if (written > 0 && (size_t)written < remaining) {
      pos += (size_t)written;
    }
  }

  send_notification("textDocument/publishDiagnostics", diagnostics);
  free(diagnostics);
}
