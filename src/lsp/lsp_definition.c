/**
 * @file lsp_definition.c
 * @brief Go-to-definition and references for LSP server
 */

#include "lsp.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern DocumentState *g_doc;

// Helper structure for reference search context
typedef struct {
  const char *symbol_name;
  const Symbol *target_symbol;
  char *result;
  size_t *pos;
  size_t *remaining;
  bool *first;
} ReferenceSearchContext;

void handle_definition(const char *id, const char *body) {
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

  size_t line, character;
  if (!safe_strtoul(line_str, &line) || !safe_strtoul(character_str, &character)) {
    free(line_str);
    free(character_str);
    send_response(id, "null");
    return;
  }
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
    // Parse module name and function name
    size_t module_len = (size_t)(dot - word);
    char *module_name = malloc(module_len + 1);
    if (!module_name) {
      free(word);
      send_response(id, "null");
      return;
    }
    strncpy(module_name, word, module_len);
    module_name[module_len] = '\0';
    const char *func_name = dot + 1;

    // Check if it's a built-in module (math, regex)
    if (strcmp(module_name, "math") == 0 || strcmp(module_name, "regex") == 0) {
      // Built-in modules don't have source files - return null
      free(module_name);
      free(word);
      send_response(id, "null");
      return;
    }

    // Look up imported module
    if (!g_doc || !is_module_imported(module_name)) {
      free(module_name);
      free(word);
      send_response(id, "null");
      return;
    }

    // Find the module in imported modules
    ImportedModule *mod = g_doc->imported_modules;
    while (mod) {
      if (mod->name && strcmp(mod->name, module_name) == 0) {
        // Load exports if needed
        if (!mod->exports && mod->file_path) {
          mod->exports = load_module_exports(mod->file_path);
        }

        // Find the function in exports
        Symbol *func_sym = mod->exports;
        while (func_sym) {
          if (func_sym->type == SYMBOL_FUNCTION &&
              strcmp(func_sym->name, func_name) == 0) {
            // Found the function - return definition location
            // Convert file path to URI format (file://)
            char *file_uri = malloc(strlen(mod->file_path) + 8);
            if (file_uri) {
              snprintf(file_uri, strlen(mod->file_path) + 8, "file://%s",
                       mod->file_path);
              char escaped_uri[LSP_PATTERN_BUFFER_SIZE];
              json_escape(file_uri, escaped_uri, sizeof(escaped_uri));

              char result[LSP_ERROR_MSG_SIZE];
              // Use approximate position (line 1, col 1) since we don't track exact positions
              snprintf(
                  result, sizeof(result),
                  "{\"uri\":\"%s\",\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
                  "\"end\":{\"line\":%zu,\"character\":%zu}}}",
                  escaped_uri, func_sym->line - 1, func_sym->column - 1,
                  func_sym->line - 1, func_sym->column - 1 + strlen(func_sym->name));

              free(file_uri);
              free(module_name);
              free(word);
              send_response(id, result);
              return;
            }
            break;
          }
          func_sym = func_sym->next;
        }
        // Function not found in module
        free(module_name);
        free(word);
        send_response(id, "null");
        return;
      }
      mod = mod->next;
    }

    // Module not found
    free(module_name);
    free(word);
    send_response(id, "null");
    return;
  }

  // Find symbol
  Symbol *sym = find_symbol_at_position(word, line, character);
  free(word);

  if (!sym) {
    send_response(id, "null");
    return;
  }

  // Return definition location
  char result[LSP_ERROR_MSG_SIZE];
  char escaped_uri[LSP_PATTERN_BUFFER_SIZE];
  json_escape(g_doc->uri, escaped_uri, sizeof(escaped_uri));
  snprintf(
      result, sizeof(result),
      "{\"uri\":\"%s\",\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
      "\"end\":{\"line\":%zu,\"character\":%zu}}}",
      escaped_uri, sym->line - 1, sym->column - 1, sym->line - 1,
      sym->column - 1 + strlen(sym->name));
  send_response(id, result);
}

void add_reference_location(ReferenceSearchContext *ctx, size_t line,
                                   size_t col, size_t length) {
  if (*ctx->remaining < 200)
    return; // Not enough space

  char escaped_uri[LSP_PATTERN_BUFFER_SIZE];
  json_escape(g_doc->uri, escaped_uri, sizeof(escaped_uri));

  int written = snprintf(
      ctx->result + *ctx->pos, *ctx->remaining,
      "%s{\"uri\":\"%s\",\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
      "\"end\":{\"line\":%zu,\"character\":%zu}}}",
      *ctx->first ? "" : ",", escaped_uri, line - 1, col - 1, line - 1,
      col - 1 + length);
  if (written > 0 && (size_t)written < *ctx->remaining) {
    *ctx->pos += (size_t)written;
    *ctx->remaining -= (size_t)written;
    *ctx->first = false;
  }
}

static bool reference_matches_symbol(const ReferenceSearchContext *ctx,
                                     ASTNode *node, const char *name) {
  if (!ctx || !name || !ctx->symbol_name) {
    return false;
  }
  if (strcmp(name, ctx->symbol_name) != 0) {
    return false;
  }
  if (!ctx->target_symbol || !node) {
    return true;
  }

  size_t line = 1;
  size_t col = 1;
  get_node_position(node, &line, &col);
  if (line == 0 || col == 0) {
    return true;
  }

  Symbol *resolved = find_symbol_at_position(ctx->symbol_name, line - 1, col - 1);
  return resolved == ctx->target_symbol;
}

// Internal recursive version with depth tracking
static void search_node_for_references_recursive(ASTNode *node, size_t *line_num,
                                                  ReferenceSearchContext *ctx,
                                                  int depth) {
  // Prevent stack overflow from deeply nested AST structures
  if (depth > MAX_AST_DEPTH) {
    return;
  }

  if (!node)
    return;

  switch (node->type) {
  case AST_PRINT:
    if (node->as.print.value) {
      search_node_for_references_recursive(node->as.print.value, line_num, ctx,
                                           depth + 1);
    }
    break;
  case AST_DEBUG:
    for (size_t i = 0; i < node->as.debug_stmt.value_count; i++) {
      if (node->as.debug_stmt.values[i]) {
        search_node_for_references_recursive(node->as.debug_stmt.values[i],
                                             line_num, ctx, depth + 1);
      }
    }
    break;

  case AST_ASSIGN:
    if (node->as.assign.name &&
        reference_matches_symbol(ctx, node, node->as.assign.name)) {
      // This is the definition, also count as a reference
      add_reference_location(ctx, *line_num, 1, strlen(ctx->symbol_name));
    }
    if (node->as.assign.value) {
      search_node_for_references_recursive(node->as.assign.value, line_num, ctx,
                                           depth + 1);
    }
    break;

  case AST_UNPACK_ASSIGN:
    // Check each unpacking target for references
    for (size_t i = 0; i < node->as.unpack_assign.name_count; i++) {
      if (node->as.unpack_assign.names[i] &&
          reference_matches_symbol(ctx, node, node->as.unpack_assign.names[i])) {
        add_reference_location(ctx, *line_num, 1, strlen(ctx->symbol_name));
      }
    }
    if (node->as.unpack_assign.value) {
      search_node_for_references_recursive(node->as.unpack_assign.value, line_num, ctx,
                                           depth + 1);
    }
    break;

  case AST_TUPLE:
    // Search each tuple element for references
    for (size_t i = 0; i < node->as.tuple.element_count; i++) {
      if (node->as.tuple.elements[i]) {
        search_node_for_references_recursive(node->as.tuple.elements[i], line_num,
                                             ctx, depth + 1);
      }
    }
    break;

  case AST_VAR:
    if (node->as.var_name &&
        reference_matches_symbol(ctx, node, node->as.var_name)) {
      add_reference_location(ctx, *line_num, 1, strlen(ctx->symbol_name));
    }
    break;

  case AST_CALL:
    if (node->as.call.name &&
        reference_matches_symbol(ctx, node, node->as.call.name)) {
      add_reference_location(ctx, *line_num, 1, strlen(ctx->symbol_name));
    }
    for (size_t i = 0; i < node->as.call.arg_count; i++) {
      if (node->as.call.args[i]) {
        search_node_for_references_recursive(node->as.call.args[i], line_num,
                                             ctx, depth + 1);
      }
    }
    break;

  case AST_FUNCTION:
    if (node->as.function.name &&
        reference_matches_symbol(ctx, node, node->as.function.name)) {
      add_reference_location(ctx, *line_num, 1, strlen(ctx->symbol_name));
    }
    for (size_t i = 0; i < node->as.function.block_size; i++) {
      if (node->as.function.block[i]) {
        search_node_for_references_recursive(node->as.function.block[i],
                                             line_num, ctx, depth + 1);
      }
    }
    break;

  case AST_BINOP:
    if (node->as.binop.left) {
      search_node_for_references_recursive(node->as.binop.left, line_num, ctx,
                                           depth + 1);
    }
    if (node->as.binop.right) {
      search_node_for_references_recursive(node->as.binop.right, line_num, ctx,
                                           depth + 1);
    }
    break;

    // Unary operations are handled via BINOP_NEG in binop

  case AST_IF:
    if (node->as.if_stmt.condition) {
      search_node_for_references_recursive(node->as.if_stmt.condition, line_num,
                                           ctx, depth + 1);
    }
    for (size_t i = 0; i < node->as.if_stmt.block_size; i++) {
      if (node->as.if_stmt.block[i]) {
        search_node_for_references_recursive(node->as.if_stmt.block[i],
                                             line_num, ctx, depth + 1);
      }
    }
    if (node->as.if_stmt.else_block) {
      for (size_t i = 0; i < node->as.if_stmt.else_block_size; i++) {
        if (node->as.if_stmt.else_block[i]) {
          search_node_for_references_recursive(
              node->as.if_stmt.else_block[i], line_num, ctx, depth + 1);
        }
      }
    }
    break;

  case AST_MATCH:
    if (node->as.match_stmt.value) {
      size_t value_line =
          node->as.match_stmt.value->line ? node->as.match_stmt.value->line
                                          : *line_num;
      search_node_for_references_recursive(node->as.match_stmt.value, &value_line,
                                           ctx, depth + 1);
    }
    for (size_t i = 0; i < node->as.match_stmt.case_count; i++) {
      ASTNode *case_pattern = node->as.match_stmt.case_patterns
                                  ? node->as.match_stmt.case_patterns[i]
                                  : NULL;
      if (case_pattern) {
        size_t pattern_line = case_pattern->line ? case_pattern->line : *line_num;
        search_node_for_references_recursive(case_pattern, &pattern_line, ctx,
                                             depth + 1);
      }
      if (node->as.match_stmt.case_blocks && node->as.match_stmt.case_blocks[i]) {
        for (size_t j = 0; j < node->as.match_stmt.case_block_sizes[i]; j++) {
          ASTNode *case_stmt = node->as.match_stmt.case_blocks[i][j];
          if (case_stmt) {
            size_t case_stmt_line =
                case_stmt->line ? case_stmt->line : *line_num;
            search_node_for_references_recursive(case_stmt, &case_stmt_line, ctx,
                                                 depth + 1);
          }
        }
      }
    }
    if (node->as.match_stmt.default_block) {
      for (size_t i = 0; i < node->as.match_stmt.default_block_size; i++) {
        ASTNode *default_stmt = node->as.match_stmt.default_block[i];
        if (default_stmt) {
          size_t default_stmt_line =
              default_stmt->line ? default_stmt->line : *line_num;
          search_node_for_references_recursive(default_stmt, &default_stmt_line,
                                               ctx, depth + 1);
        }
      }
    }
    break;

  case AST_FOR:
    if (node->as.for_stmt.var &&
        reference_matches_symbol(ctx, node, node->as.for_stmt.var)) {
      add_reference_location(ctx, *line_num, 1, strlen(ctx->symbol_name));
    }
    if (node->as.for_stmt.iterable) {
      search_node_for_references_recursive(node->as.for_stmt.iterable, line_num,
                                           ctx, depth + 1);
    }
    for (size_t i = 0; i < node->as.for_stmt.block_size; i++) {
      if (node->as.for_stmt.block[i]) {
        search_node_for_references_recursive(node->as.for_stmt.block[i],
                                             line_num, ctx, depth + 1);
      }
    }
    break;

  case AST_WHILE:
    if (node->as.while_stmt.condition) {
      search_node_for_references_recursive(node->as.while_stmt.condition,
                                           line_num, ctx, depth + 1);
    }
    for (size_t i = 0; i < node->as.while_stmt.block_size; i++) {
      if (node->as.while_stmt.block[i]) {
        search_node_for_references_recursive(node->as.while_stmt.block[i],
                                             line_num, ctx, depth + 1);
      }
    }
    break;

  case AST_RETURN:
    for (size_t i = 0; i < node->as.return_stmt.value_count; i++) {
      if (node->as.return_stmt.values[i]) {
        search_node_for_references_recursive(node->as.return_stmt.values[i], line_num,
                                             ctx, depth + 1);
      }
    }
    break;

  case AST_INDEX:
    if (node->as.index.list_expr) {
      search_node_for_references_recursive(node->as.index.list_expr, line_num,
                                           ctx, depth + 1);
    }
    if (node->as.index.index) {
      search_node_for_references_recursive(node->as.index.index, line_num, ctx,
                                           depth + 1);
    }
    break;

  case AST_SLICE:
    if (node->as.slice.list_expr) {
      search_node_for_references_recursive(node->as.slice.list_expr, line_num,
                                           ctx, depth + 1);
    }
    if (node->as.slice.start) {
      search_node_for_references_recursive(node->as.slice.start, line_num, ctx,
                                           depth + 1);
    }
    if (node->as.slice.end) {
      search_node_for_references_recursive(node->as.slice.end, line_num, ctx,
                                           depth + 1);
    }
    break;

  case AST_LIST:
    for (size_t i = 0; i < node->as.list.element_count; i++) {
      if (node->as.list.elements[i]) {
        search_node_for_references_recursive(node->as.list.elements[i],
                                             line_num, ctx, depth + 1);
      }
    }
    break;

  case AST_LIST_COMPREHENSION:
    if (node->as.list_comprehension.var &&
        reference_matches_symbol(ctx, node, node->as.list_comprehension.var)) {
      add_reference_location(ctx, *line_num, 1, strlen(ctx->symbol_name));
    }
    search_node_for_references_recursive(node->as.list_comprehension.element_expr,
                                         line_num, ctx, depth + 1);
    search_node_for_references_recursive(node->as.list_comprehension.iterable,
                                         line_num, ctx, depth + 1);
    search_node_for_references_recursive(node->as.list_comprehension.condition,
                                         line_num, ctx, depth + 1);
    break;

  case AST_MAP:
    for (size_t i = 0; i < node->as.map.entry_count; i++) {
      if (node->as.map.keys[i]) {
        search_node_for_references_recursive(node->as.map.keys[i], line_num,
                                             ctx, depth + 1);
      }
      if (node->as.map.values[i]) {
        search_node_for_references_recursive(node->as.map.values[i], line_num,
                                             ctx, depth + 1);
      }
    }
    break;

  case AST_FSTRING:
    for (size_t i = 0; i < node->as.fstring.part_count; i++) {
      if (node->as.fstring.parts[i]) {
        search_node_for_references_recursive(node->as.fstring.parts[i],
                                             line_num, ctx, depth + 1);
      }
    }
    break;

  case AST_TRY:
    for (size_t i = 0; i < node->as.try_stmt.try_block_size; i++) {
      if (node->as.try_stmt.try_block[i]) {
        search_node_for_references_recursive(node->as.try_stmt.try_block[i],
                                             line_num, ctx, depth + 1);
      }
    }
    for (size_t i = 0; i < node->as.try_stmt.catch_block_count; i++) {
      if (node->as.try_stmt.catch_blocks[i].catch_var &&
          reference_matches_symbol(ctx, node,
                                   node->as.try_stmt.catch_blocks[i].catch_var)) {
        add_reference_location(ctx, *line_num, 1, strlen(ctx->symbol_name));
      }
      for (size_t j = 0; j < node->as.try_stmt.catch_blocks[i].catch_block_size;
           j++) {
        if (node->as.try_stmt.catch_blocks[i].catch_block[j]) {
          search_node_for_references_recursive(
              node->as.try_stmt.catch_blocks[i].catch_block[j], line_num, ctx,
              depth + 1);
        }
      }
    }
    if (node->as.try_stmt.finally_block) {
      for (size_t i = 0; i < node->as.try_stmt.finally_block_size; i++) {
        if (node->as.try_stmt.finally_block[i]) {
          search_node_for_references_recursive(
              node->as.try_stmt.finally_block[i], line_num, ctx, depth + 1);
        }
      }
    }
    break;

  default:
    break;
  }
}

// Public wrapper that starts with depth 0
void search_node_for_references(ASTNode *node, size_t *line_num,
                                ReferenceSearchContext *ctx) {
  search_node_for_references_recursive(node, line_num, ctx, 0);
}

void find_all_references_in_ast(const char *symbol_name,
                                const Symbol *target_symbol, const char *text,
                                AST *ast, char *result, size_t *pos,
                                size_t *remaining, bool *first) {
  if (!symbol_name || !ast || !ast->statements)
    return;

  ReferenceSearchContext ctx = {symbol_name, target_symbol, result, pos,
                                remaining, first};

  // Use text-based position tracking when available for more accuracy
  if (text) {
    // Count actual lines in source text for better accuracy
  size_t line_num = 1;
  for (size_t i = 0; i < ast->count; i++) {
    if (ast->statements[i]) {
        // Try to find the actual line number by searching for the statement
        // pattern in the source text
        size_t found_line = line_num;
        // For better accuracy, we could search for statement patterns, but
        // for now we'll use the text-based line counting which is more accurate
        // than the previous simple increment
        search_node_for_references_recursive(ast->statements[i], &found_line,
                                             &ctx, 0);
        // Update line_num based on text position if we found references
        // Otherwise increment conservatively
        if (found_line > line_num) {
          line_num = found_line;
        } else {
          // Estimate: each statement is roughly on a new line
          // Count newlines up to a reasonable estimate
          line_num++;
        }
      } else {
        line_num++;
      }
    }
  } else {
    // Fallback to approximate counting when text is not available
    size_t line_num = 1;
    for (size_t i = 0; i < ast->count; i++) {
      if (ast->statements[i]) {
        search_node_for_references_recursive(ast->statements[i], &line_num, &ctx,
                                             0);
      }
      // Approximate line increment (rough estimate)
    line_num++;
    }
  }
}

void handle_references(const char *id, const char *body) {
  if (!g_doc || !g_doc->text || !g_doc->ast) {
    send_response(id, "[]");
    return;
  }

  char *line_str = json_get_nested_value(body, "params.position.line");
  char *character_str =
      json_get_nested_value(body, "params.position.character");

  if (!line_str || !character_str) {
    send_response(id, "[]");
    free(line_str);
    free(character_str);
    return;
  }

  size_t line, character;
  if (!safe_strtoul(line_str, &line) || !safe_strtoul(character_str, &character)) {
    free(line_str);
    free(character_str);
    send_response(id, "null");
    return;
  }
  free(line_str);
  free(character_str);

  // Find word at position
  char *word = get_word_at_position(g_doc->text, line, character);
  if (!word) {
    send_response(id, "[]");
    return;
  }

  // LIMITATION: Module.function syntax not fully supported (see handle_definition)
  if (strchr(word, '.')) {
    free(word);
    send_response(id, "[]");
    return;
  }

  // Find symbol to get its definition location
  Symbol *sym = find_symbol_at_position(word, line, character);
  if (!sym) {
    free(word);
    send_response(id, "[]");
    return;
  }

  // Build references array
  char result[LSP_REFERENCES_BUFFER_SIZE];
  size_t pos = 0;
  size_t remaining = sizeof(result);
  bool first = true;

  // Add definition location
  char escaped_uri[LSP_PATTERN_BUFFER_SIZE];
  json_escape(g_doc->uri, escaped_uri, sizeof(escaped_uri));
  int written = snprintf(
      result + pos, remaining,
      "{\"uri\":\"%s\",\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
      "\"end\":{\"line\":%zu,\"character\":%zu}}}",
      escaped_uri, sym->line - 1, sym->column - 1, sym->line - 1,
      sym->column - 1 + strlen(sym->name));
  if (written > 0 && (size_t)written < remaining) {
    pos += (size_t)written;
    remaining -= (size_t)written;
    first = false;
  }

  // Find all references in AST
  find_all_references_in_ast(word, sym, g_doc->text, g_doc->ast, result, &pos,
                             &remaining, &first);

  free(word);

  // Wrap in array
  char final_result[LSP_REFERENCES_BUFFER_SIZE];
  snprintf(final_result, sizeof(final_result), "[%s]", result);
  send_response(id, final_result);
}

void handle_prepare_rename(const char *id, const char *body) {
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

  size_t line, character;
  if (!safe_strtoul(line_str, &line) || !safe_strtoul(character_str, &character)) {
    free(line_str);
    free(character_str);
    send_response(id, "null");
    return;
  }
  free(line_str);
  free(character_str);

  // Find word at position
  char *word = get_word_at_position(g_doc->text, line, character);
  if (!word) {
    send_response(id, "null");
    return;
  }

  // LIMITATION: Module.function syntax not fully supported (see handle_definition)
  if (strchr(word, '.')) {
    free(word);
    send_response(id, "null");
    return;
  }

  // Find symbol
  Symbol *sym = find_symbol_at_position(word, line, character);
  if (!sym) {
    free(word);
    send_response(id, "null");
    return;
  }

  // Return the range of the symbol name
  char result[LSP_ERROR_MSG_SIZE];
  snprintf(result, sizeof(result),
           "{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
           "\"end\":{\"line\":%zu,\"character\":%zu}},\"placeholder\":\"%s\"}",
           sym->line - 1, sym->column - 1, sym->line - 1,
           sym->column - 1 + strlen(sym->name), word);
  free(word);
  send_response(id, result);
}

void handle_rename(const char *id, const char *body) {
  if (!g_doc || !g_doc->text || !g_doc->ast) {
    send_response(id, "null");
    return;
  }

  char *line_str = json_get_nested_value(body, "params.position.line");
  char *character_str =
      json_get_nested_value(body, "params.position.character");
  char *new_name = json_get_nested_value(body, "params.newName");

  if (!line_str || !character_str || !new_name) {
    send_response(id, "null");
    free(line_str);
    free(character_str);
    free(new_name);
    return;
  }

  size_t line, character;
  if (!safe_strtoul(line_str, &line) || !safe_strtoul(character_str, &character)) {
    free(line_str);
    free(character_str);
    send_response(id, "null");
    return;
  }
  free(line_str);
  free(character_str);

  // Find word at position
  char *word = get_word_at_position(g_doc->text, line, character);
  if (!word) {
    free(new_name);
    send_response(id, "null");
    return;
  }

  // Handle module.function syntax - skip for now
  if (strchr(word, '.')) {
    free(word);
    free(new_name);
    send_response(id, "null");
    return;
  }

  // Find symbol
  Symbol *sym = find_symbol_at_position(word, line, character);
  if (!sym) {
    free(word);
    free(new_name);
    send_response(id, "null");
    return;
  }

  char escaped_uri[LSP_PATTERN_BUFFER_SIZE];
  json_escape(g_doc->uri, escaped_uri, sizeof(escaped_uri));
  int written = 0;

  // Build WorkspaceEdit with TextEdits
  char result[LSP_LARGE_BUFFER_SIZE];
  size_t pos = 0;
  size_t remaining = sizeof(result);

  // Escape new_name for JSON
  char escaped_new_name[LSP_PATTERN_BUFFER_SIZE];
  json_escape(new_name, escaped_new_name, sizeof(escaped_new_name));

  // Start building WorkspaceEdit
  written =
      snprintf(result + pos, remaining, "{\"changes\":{\"%s\":[", escaped_uri);
  if (written > 0 && (size_t)written < remaining) {
    pos += (size_t)written;
    remaining -= (size_t)written;
  }

  // Add TextEdit for definition
  bool first_edit = true;
  written = snprintf(
      result + pos, remaining,
      "%s{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
      "\"end\":{\"line\":%zu,\"character\":%zu}},\"newText\":\"%s\"}",
      first_edit ? "" : ",", sym->line - 1, sym->column - 1, sym->line - 1,
      sym->column - 1 + strlen(sym->name), escaped_new_name);
  if (written > 0 && (size_t)written < remaining) {
    pos += (size_t)written;
    remaining -= (size_t)written;
    first_edit = false;
  }

  // Add TextEdits for references that resolve to the same symbol.
  const char *text = g_doc->text;
  size_t text_len = strlen(text);
  size_t word_len = strlen(word);
  size_t current_line = 1;
  size_t current_col = 0;
  bool in_string = false;
  bool in_comment = false;
  char string_delim = '\0';

  for (size_t i = 0; i < text_len && remaining > 200; i++) {
    if (text[i] == '\n') {
      current_line++;
      current_col = 0;
      in_comment = false;
      continue;
    }
    if (text[i] == '#') {
      in_comment = true;
    }
    bool is_unescaped_quote =
        (text[i] == '"' || text[i] == '\'') && (i == 0 || text[i - 1] != '\\');
    if (!in_comment && is_unescaped_quote) {
      if (!in_string) {
        in_string = true;
        string_delim = text[i];
      } else if (text[i] == string_delim) {
        in_string = false;
        string_delim = '\0';
      }
    }
    if (in_string || in_comment) {
      current_col++;
      continue;
    }

    // Check if we found the word at this position
    if (i + word_len <= text_len && strncmp(text + i, word, word_len) == 0) {
      // Check word boundaries (fixed operator precedence)
      bool is_word_start = (i == 0 || (!isalnum((unsigned char)text[i - 1]) &&
                                       text[i - 1] != '_'));
      bool is_word_end = (i + word_len >= text_len ||
                          (!isalnum((unsigned char)text[i + word_len]) &&
                           text[i + word_len] != '_'));

      if (is_word_start && is_word_end) {
        size_t edit_line = current_line;
        size_t edit_col = current_col;

        // Skip if this is the definition (already added)
        if (edit_line == sym->line && edit_col == sym->column - 1) {
          current_col++;
          continue;
        }

        Symbol *resolved = find_symbol_at_position(word, edit_line - 1, edit_col);
        if (resolved == sym) {
          written = snprintf(
              result + pos, remaining,
              ",{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
              "\"end\":{\"line\":%zu,\"character\":%zu}},\"newText\":\"%s\"}",
              edit_line - 1, edit_col, edit_line - 1, edit_col + word_len,
              escaped_new_name);
          if (written > 0 && (size_t)written < remaining) {
            pos += (size_t)written;
            remaining -= (size_t)written;
          } else {
            break; // Buffer full
          }
        }
      }
    }
    current_col++;
  }

  // Close arrays and object
  written = snprintf(result + pos, remaining, "]}}");
  if (written > 0 && (size_t)written < remaining) {
    pos += (size_t)written;
  }

  free(word);
  free(new_name);
  send_response(id, result);
}
