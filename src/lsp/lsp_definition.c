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
  case AST_ASSIGN:
    if (node->as.assign.name &&
        strcmp(node->as.assign.name, ctx->symbol_name) == 0) {
      // This is the definition, also count as a reference
      add_reference_location(ctx, *line_num, 1, strlen(ctx->symbol_name));
    }
    if (node->as.assign.value) {
      search_node_for_references_recursive(node->as.assign.value, line_num, ctx,
                                           depth + 1);
    }
    break;

  case AST_VAR:
    if (node->as.var_name && strcmp(node->as.var_name, ctx->symbol_name) == 0) {
      add_reference_location(ctx, *line_num, 1, strlen(ctx->symbol_name));
    }
    break;

  case AST_CALL:
    if (node->as.call.name &&
        strcmp(node->as.call.name, ctx->symbol_name) == 0) {
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
        strcmp(node->as.function.name, ctx->symbol_name) == 0) {
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

  case AST_FOR:
    if (node->as.for_stmt.var &&
        strcmp(node->as.for_stmt.var, ctx->symbol_name) == 0) {
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
    if (node->as.return_stmt.value) {
      search_node_for_references_recursive(node->as.return_stmt.value, line_num,
                                           ctx, depth + 1);
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
          strcmp(node->as.try_stmt.catch_blocks[i].catch_var,
                 ctx->symbol_name) == 0) {
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
                                       const char *text __attribute__((unused)),
                                       AST *ast, char *result, size_t *pos,
                                       size_t *remaining, bool *first) {
  // TODO: Use text parameter for more accurate position tracking
  // Currently uses approximate line counting from AST structure
  (void)text; // Placeholder for improved position tracking
  if (!symbol_name || !ast || !ast->statements)
    return;

  ReferenceSearchContext ctx = {symbol_name, result, pos, remaining, first};

  // Search through all top-level statements
  size_t line_num = 1;
  for (size_t i = 0; i < ast->count; i++) {
    if (ast->statements[i]) {
      search_node_for_references_recursive(ast->statements[i], &line_num, &ctx,
                                           0);
    }
    // Approximate line increment (this is rough since AST doesn't store exact
    // positions)
    line_num++;
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

  // Handle module.function syntax - skip for now
  if (strchr(word, '.')) {
    free(word);
    send_response(id, "[]");
    return;
  }

  // Find symbol to get its definition location
  Symbol *sym = find_symbol(word);
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
  find_all_references_in_ast(word, g_doc->text, g_doc->ast, result, &pos,
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

  // Handle module.function syntax - skip for now
  if (strchr(word, '.')) {
    free(word);
    send_response(id, "null");
    return;
  }

  // Find symbol
  Symbol *sym = find_symbol(word);
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
  Symbol *sym = find_symbol(word);
  if (!sym) {
    free(word);
    free(new_name);
    send_response(id, "null");
    return;
  }

  // Build references array (similar to handle_references)
  char references[LSP_REFERENCES_BUFFER_SIZE];
  size_t ref_pos = 0;
  size_t ref_remaining = sizeof(references);
  bool first_ref = true;

  // Add definition location
  char escaped_uri[LSP_PATTERN_BUFFER_SIZE];
  json_escape(g_doc->uri, escaped_uri, sizeof(escaped_uri));
  int written = snprintf(
      references + ref_pos, ref_remaining,
      "{\"uri\":\"%s\",\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
      "\"end\":{\"line\":%zu,\"character\":%zu}}}",
      escaped_uri, sym->line - 1, sym->column - 1, sym->line - 1,
      sym->column - 1 + strlen(sym->name));
  if (written > 0 && (size_t)written < ref_remaining) {
    ref_pos += (size_t)written;
    ref_remaining -= (size_t)written;
    first_ref = false;
  }

  // Find all references in AST
  find_all_references_in_ast(word, g_doc->text, g_doc->ast, references,
                             &ref_pos, &ref_remaining, &first_ref);

  // Build WorkspaceEdit with TextEdits
  char result[LSP_LARGE_BUFFER_SIZE];
  size_t pos = 0;
  size_t remaining = sizeof(result);

  // Escape new_name for JSON
  char escaped_new_name[LSP_PATTERN_BUFFER_SIZE];
  json_escape(new_name, escaped_new_name, sizeof(escaped_new_name));

  // Parse references JSON array and build TextEdits
  // For simplicity, we'll create a TextEdit for each reference
  // In a real implementation, we'd parse the references array properly
  // For now, we'll use the same approach as references but create edits

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

  // Parse references and add TextEdits
  // For now, we'll use a simpler approach: find all occurrences in text
  // and create edits for them
  const char *text = g_doc->text;
  size_t text_len = strlen(text);
  size_t word_len = strlen(word);
  size_t current_line = 1;
  size_t current_col = 0;
  bool in_string = false;
  bool in_comment = false;

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
    if (text[i] == '"' && (i == 0 || text[i - 1] != '\\')) {
      in_string = !in_string;
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
        // Calculate line and column for this occurrence
        size_t edit_line = current_line;
        size_t edit_col = current_col;

        // Skip if this is the definition (already added)
        if (edit_line == sym->line && edit_col == sym->column - 1) {
          current_col++;
          continue;
        }

        // Add TextEdit
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

