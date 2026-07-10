/**
 * @file lsp_utils.c
 * @brief Helper functions for LSP server
 */

// Enable POSIX function declarations (e.g., strdup) on strict C builds.
#define _POSIX_C_SOURCE 200809L

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
  const Symbol *target_symbol;
  size_t count;
} ReferenceCountContext;

typedef struct {
  size_t start_line;
  size_t start_column;
  size_t end_line;
  size_t end_column;
  bool has_position;
} NodePositionBounds;

static Symbol *allocate_symbol(void) { return calloc(1, sizeof(Symbol)); }

static int compare_source_positions(size_t line_a, size_t col_a, size_t line_b,
                                    size_t col_b) {
  if (line_a < line_b) {
    return -1;
  }
  if (line_a > line_b) {
    return 1;
  }
  if (col_a < col_b) {
    return -1;
  }
  if (col_a > col_b) {
    return 1;
  }
  return 0;
}

static bool position_in_range(size_t line, size_t col, size_t start_line,
                              size_t start_col, size_t end_line,
                              size_t end_col) {
  if (compare_source_positions(line, col, start_line, start_col) < 0) {
    return false;
  }
  if (compare_source_positions(line, col, end_line, end_col) > 0) {
    return false;
  }
  return true;
}

static void update_node_bounds(NodePositionBounds *bounds, size_t line,
                               size_t column) {
  if (!bounds) {
    return;
  }
  if (!bounds->has_position) {
    bounds->start_line = line;
    bounds->start_column = column;
    bounds->end_line = line;
    bounds->end_column = column;
    bounds->has_position = true;
    return;
  }

  if (compare_source_positions(line, column, bounds->start_line,
                               bounds->start_column) < 0) {
    bounds->start_line = line;
    bounds->start_column = column;
  }
  if (compare_source_positions(line, column, bounds->end_line,
                               bounds->end_column) > 0) {
    bounds->end_line = line;
    bounds->end_column = column;
  }
}

static void collect_node_position_bounds_recursive(ASTNode *node,
                                                   NodePositionBounds *bounds,
                                                   int depth) {
  if (!node || !bounds || depth > MAX_AST_DEPTH) {
    return;
  }

  size_t line = 1;
  size_t col = 1;
  get_node_position(node, &line, &col);
  update_node_bounds(bounds, line, col);

  switch (node->type) {
  case AST_LIST_COMPREHENSION:
    collect_node_position_bounds_recursive(node->as.list_comprehension.element_expr,
                                           bounds, depth + 1);
    collect_node_position_bounds_recursive(node->as.list_comprehension.iterable,
                                           bounds, depth + 1);
    collect_node_position_bounds_recursive(node->as.list_comprehension.condition,
                                           bounds, depth + 1);
    break;
  case AST_ASSIGN:
    collect_node_position_bounds_recursive(node->as.assign.value, bounds,
                                           depth + 1);
    break;
  case AST_PRINT:
    collect_node_position_bounds_recursive(node->as.print.value, bounds,
                                           depth + 1);
    break;
  case AST_DEBUG:
    for (size_t i = 0; i < node->as.debug_stmt.value_count; i++) {
      collect_node_position_bounds_recursive(node->as.debug_stmt.values[i],
                                             bounds, depth + 1);
    }
    break;
  case AST_BINOP:
    collect_node_position_bounds_recursive(node->as.binop.left, bounds,
                                           depth + 1);
    collect_node_position_bounds_recursive(node->as.binop.right, bounds,
                                           depth + 1);
    break;
  case AST_IF:
    collect_node_position_bounds_recursive(node->as.if_stmt.condition, bounds,
                                           depth + 1);
    for (size_t i = 0; i < node->as.if_stmt.block_size; i++) {
      collect_node_position_bounds_recursive(node->as.if_stmt.block[i], bounds,
                                             depth + 1);
    }
    for (size_t i = 0; i < node->as.if_stmt.else_if_count; i++) {
      collect_node_position_bounds_recursive(
          node->as.if_stmt.else_if_conditions[i], bounds, depth + 1);
      for (size_t j = 0; j < node->as.if_stmt.else_if_block_sizes[i]; j++) {
        collect_node_position_bounds_recursive(node->as.if_stmt.else_if_blocks[i][j],
                                               bounds, depth + 1);
      }
    }
    for (size_t i = 0; i < node->as.if_stmt.else_block_size; i++) {
      collect_node_position_bounds_recursive(node->as.if_stmt.else_block[i],
                                             bounds, depth + 1);
    }
    break;
  case AST_MATCH:
    collect_node_position_bounds_recursive(node->as.match_stmt.value, bounds,
                                           depth + 1);
    for (size_t i = 0; i < node->as.match_stmt.case_count; i++) {
      collect_node_position_bounds_recursive(node->as.match_stmt.case_patterns[i],
                                             bounds, depth + 1);
      for (size_t j = 0; j < node->as.match_stmt.case_block_sizes[i]; j++) {
        collect_node_position_bounds_recursive(node->as.match_stmt.case_blocks[i][j],
                                               bounds, depth + 1);
      }
    }
    for (size_t i = 0; i < node->as.match_stmt.default_block_size; i++) {
      collect_node_position_bounds_recursive(node->as.match_stmt.default_block[i],
                                             bounds, depth + 1);
    }
    break;
  case AST_FOR:
    collect_node_position_bounds_recursive(node->as.for_stmt.iterable, bounds,
                                           depth + 1);
    collect_node_position_bounds_recursive(node->as.for_stmt.end, bounds,
                                           depth + 1);
    collect_node_position_bounds_recursive(node->as.for_stmt.step, bounds,
                                           depth + 1);
    for (size_t i = 0; i < node->as.for_stmt.block_size; i++) {
      collect_node_position_bounds_recursive(node->as.for_stmt.block[i], bounds,
                                             depth + 1);
    }
    break;
  case AST_WHILE:
    collect_node_position_bounds_recursive(node->as.while_stmt.condition, bounds,
                                           depth + 1);
    for (size_t i = 0; i < node->as.while_stmt.block_size; i++) {
      collect_node_position_bounds_recursive(node->as.while_stmt.block[i], bounds,
                                             depth + 1);
    }
    break;
  case AST_FUNCTION:
    for (size_t i = 0; i < node->as.function.block_size; i++) {
      collect_node_position_bounds_recursive(node->as.function.block[i], bounds,
                                             depth + 1);
    }
    break;
  case AST_CALL:
    for (size_t i = 0; i < node->as.call.arg_count; i++) {
      collect_node_position_bounds_recursive(node->as.call.args[i], bounds,
                                             depth + 1);
    }
    break;
  case AST_RETURN:
    for (size_t i = 0; i < node->as.return_stmt.value_count; i++) {
      collect_node_position_bounds_recursive(node->as.return_stmt.values[i], bounds,
                                             depth + 1);
    }
    break;
  case AST_LIST:
    for (size_t i = 0; i < node->as.list.element_count; i++) {
      collect_node_position_bounds_recursive(node->as.list.elements[i], bounds,
                                             depth + 1);
    }
    break;
  case AST_RANGE:
    collect_node_position_bounds_recursive(node->as.range.start, bounds,
                                           depth + 1);
    collect_node_position_bounds_recursive(node->as.range.end, bounds, depth + 1);
    collect_node_position_bounds_recursive(node->as.range.step, bounds,
                                           depth + 1);
    break;
  case AST_MAP:
    for (size_t i = 0; i < node->as.map.entry_count; i++) {
      collect_node_position_bounds_recursive(node->as.map.keys[i], bounds,
                                             depth + 1);
      collect_node_position_bounds_recursive(node->as.map.values[i], bounds,
                                             depth + 1);
    }
    break;
  case AST_INDEX:
    collect_node_position_bounds_recursive(node->as.index.list_expr, bounds,
                                           depth + 1);
    collect_node_position_bounds_recursive(node->as.index.index, bounds,
                                           depth + 1);
    break;
  case AST_SLICE:
    collect_node_position_bounds_recursive(node->as.slice.list_expr, bounds,
                                           depth + 1);
    collect_node_position_bounds_recursive(node->as.slice.start, bounds,
                                           depth + 1);
    collect_node_position_bounds_recursive(node->as.slice.end, bounds, depth + 1);
    break;
  case AST_ASSIGN_INDEX:
    collect_node_position_bounds_recursive(node->as.assign_index.target, bounds,
                                           depth + 1);
    collect_node_position_bounds_recursive(node->as.assign_index.index, bounds,
                                           depth + 1);
    collect_node_position_bounds_recursive(node->as.assign_index.value, bounds,
                                           depth + 1);
    break;
  case AST_DELETE:
    collect_node_position_bounds_recursive(node->as.delete_stmt.target, bounds,
                                           depth + 1);
    collect_node_position_bounds_recursive(node->as.delete_stmt.key, bounds,
                                           depth + 1);
    break;
  case AST_TRY:
    for (size_t i = 0; i < node->as.try_stmt.try_block_size; i++) {
      collect_node_position_bounds_recursive(node->as.try_stmt.try_block[i], bounds,
                                             depth + 1);
    }
    for (size_t i = 0; i < node->as.try_stmt.catch_block_count; i++) {
      for (size_t j = 0; j < node->as.try_stmt.catch_blocks[i].catch_block_size;
           j++) {
        collect_node_position_bounds_recursive(
            node->as.try_stmt.catch_blocks[i].catch_block[j], bounds, depth + 1);
      }
    }
    for (size_t i = 0; i < node->as.try_stmt.finally_block_size; i++) {
      collect_node_position_bounds_recursive(node->as.try_stmt.finally_block[i],
                                             bounds, depth + 1);
    }
    break;
  case AST_RAISE:
    collect_node_position_bounds_recursive(node->as.raise_stmt.message, bounds,
                                           depth + 1);
    break;
  case AST_LAMBDA:
    if (node->as.lambda.is_single_line) {
      collect_node_position_bounds_recursive(node->as.lambda.body_expr, bounds,
                                             depth + 1);
    } else {
      for (size_t i = 0; i < node->as.lambda.block_size; i++) {
        collect_node_position_bounds_recursive(node->as.lambda.block[i], bounds,
                                               depth + 1);
      }
    }
    break;
  case AST_FSTRING:
    for (size_t i = 0; i < node->as.fstring.part_count; i++) {
      collect_node_position_bounds_recursive(node->as.fstring.parts[i], bounds,
                                             depth + 1);
    }
    break;
  case AST_TUPLE:
    for (size_t i = 0; i < node->as.tuple.element_count; i++) {
      collect_node_position_bounds_recursive(node->as.tuple.elements[i], bounds,
                                             depth + 1);
    }
    break;
  case AST_UNPACK_ASSIGN:
    collect_node_position_bounds_recursive(node->as.unpack_assign.value, bounds,
                                           depth + 1);
    break;
  default:
    break;
  }
}

static void get_node_position_bounds(ASTNode *node, NodePositionBounds *bounds) {
  if (!bounds) {
    return;
  }
  memset(bounds, 0, sizeof(*bounds));
  collect_node_position_bounds_recursive(node, bounds, 0);
}

static void process_comprehension_symbols_recursive(ASTNode *node,
                                                    Symbol ***tail) {
  if (!node || !tail) {
    return;
  }

  switch (node->type) {
  case AST_LIST_COMPREHENSION: {
    if (node->as.list_comprehension.var) {
      Symbol *sym = allocate_symbol();
      if (sym) {
        sym->name = strdup(node->as.list_comprehension.var);
        if (sym->name) {
          NodePositionBounds bounds;
          sym->type = SYMBOL_VARIABLE;
          sym->is_mutable = false;
          sym->type_name = NULL;
          sym->param_count = 0;
          sym->required_param_count = 0;
          sym->has_variadic = false;
          sym->param_names = NULL;
          sym->written = true;
          sym->read = false;
          get_node_position(node, &sym->line, &sym->column);
          get_node_position_bounds(node, &bounds);
          if (bounds.has_position) {
            sym->is_block_local = true;
            sym->scope_start_line = bounds.start_line;
            sym->scope_start_column = bounds.start_column;
            sym->scope_end_line = bounds.end_line;
            sym->scope_end_column = bounds.end_column;
          }
          sym->next = NULL;
          **tail = sym;
          *tail = &sym->next;
        } else {
          free(sym);
        }
      }
    }
    process_comprehension_symbols_recursive(
        node->as.list_comprehension.element_expr, tail);
    process_comprehension_symbols_recursive(node->as.list_comprehension.iterable,
                                            tail);
    process_comprehension_symbols_recursive(
        node->as.list_comprehension.condition, tail);
    break;
  }
  case AST_ASSIGN:
    process_comprehension_symbols_recursive(node->as.assign.value, tail);
    break;
  case AST_PRINT:
    process_comprehension_symbols_recursive(node->as.print.value, tail);
    break;
  case AST_DEBUG:
    for (size_t i = 0; i < node->as.debug_stmt.value_count; i++) {
      process_comprehension_symbols_recursive(node->as.debug_stmt.values[i],
                                              tail);
    }
    break;
  case AST_BINOP:
    process_comprehension_symbols_recursive(node->as.binop.left, tail);
    process_comprehension_symbols_recursive(node->as.binop.right, tail);
    break;
  case AST_IF:
    process_comprehension_symbols_recursive(node->as.if_stmt.condition, tail);
    for (size_t i = 0; i < node->as.if_stmt.block_size; i++) {
      process_comprehension_symbols_recursive(node->as.if_stmt.block[i], tail);
    }
    for (size_t i = 0; i < node->as.if_stmt.else_if_count; i++) {
      process_comprehension_symbols_recursive(
          node->as.if_stmt.else_if_conditions[i], tail);
      for (size_t j = 0; j < node->as.if_stmt.else_if_block_sizes[i]; j++) {
        process_comprehension_symbols_recursive(
            node->as.if_stmt.else_if_blocks[i][j], tail);
      }
    }
    for (size_t i = 0; i < node->as.if_stmt.else_block_size; i++) {
      process_comprehension_symbols_recursive(node->as.if_stmt.else_block[i],
                                              tail);
    }
    break;
  case AST_MATCH:
    process_comprehension_symbols_recursive(node->as.match_stmt.value, tail);
    for (size_t i = 0; i < node->as.match_stmt.case_count; i++) {
      process_comprehension_symbols_recursive(
          node->as.match_stmt.case_patterns[i], tail);
      for (size_t j = 0; j < node->as.match_stmt.case_block_sizes[i]; j++) {
        process_comprehension_symbols_recursive(
            node->as.match_stmt.case_blocks[i][j], tail);
      }
    }
    for (size_t i = 0; i < node->as.match_stmt.default_block_size; i++) {
      process_comprehension_symbols_recursive(
          node->as.match_stmt.default_block[i], tail);
    }
    break;
  case AST_FOR:
    process_comprehension_symbols_recursive(node->as.for_stmt.iterable, tail);
    process_comprehension_symbols_recursive(node->as.for_stmt.end, tail);
    process_comprehension_symbols_recursive(node->as.for_stmt.step, tail);
    for (size_t i = 0; i < node->as.for_stmt.block_size; i++) {
      process_comprehension_symbols_recursive(node->as.for_stmt.block[i], tail);
    }
    break;
  case AST_WHILE:
    process_comprehension_symbols_recursive(node->as.while_stmt.condition, tail);
    for (size_t i = 0; i < node->as.while_stmt.block_size; i++) {
      process_comprehension_symbols_recursive(node->as.while_stmt.block[i],
                                              tail);
    }
    break;
  case AST_FUNCTION:
    for (size_t i = 0; i < node->as.function.block_size; i++) {
      process_comprehension_symbols_recursive(node->as.function.block[i], tail);
    }
    break;
  case AST_CALL:
    for (size_t i = 0; i < node->as.call.arg_count; i++) {
      process_comprehension_symbols_recursive(node->as.call.args[i], tail);
    }
    break;
  case AST_RETURN:
    for (size_t i = 0; i < node->as.return_stmt.value_count; i++) {
      process_comprehension_symbols_recursive(node->as.return_stmt.values[i],
                                              tail);
    }
    break;
  case AST_LIST:
    for (size_t i = 0; i < node->as.list.element_count; i++) {
      process_comprehension_symbols_recursive(node->as.list.elements[i], tail);
    }
    break;
  case AST_RANGE:
    process_comprehension_symbols_recursive(node->as.range.start, tail);
    process_comprehension_symbols_recursive(node->as.range.end, tail);
    process_comprehension_symbols_recursive(node->as.range.step, tail);
    break;
  case AST_MAP:
    for (size_t i = 0; i < node->as.map.entry_count; i++) {
      process_comprehension_symbols_recursive(node->as.map.keys[i], tail);
      process_comprehension_symbols_recursive(node->as.map.values[i], tail);
    }
    break;
  case AST_INDEX:
    process_comprehension_symbols_recursive(node->as.index.list_expr, tail);
    process_comprehension_symbols_recursive(node->as.index.index, tail);
    break;
  case AST_SLICE:
    process_comprehension_symbols_recursive(node->as.slice.list_expr, tail);
    process_comprehension_symbols_recursive(node->as.slice.start, tail);
    process_comprehension_symbols_recursive(node->as.slice.end, tail);
    break;
  case AST_ASSIGN_INDEX:
    process_comprehension_symbols_recursive(node->as.assign_index.target, tail);
    process_comprehension_symbols_recursive(node->as.assign_index.index, tail);
    process_comprehension_symbols_recursive(node->as.assign_index.value, tail);
    break;
  case AST_DELETE:
    process_comprehension_symbols_recursive(node->as.delete_stmt.target, tail);
    process_comprehension_symbols_recursive(node->as.delete_stmt.key, tail);
    break;
  case AST_TRY:
    for (size_t i = 0; i < node->as.try_stmt.try_block_size; i++) {
      process_comprehension_symbols_recursive(node->as.try_stmt.try_block[i],
                                              tail);
    }
    for (size_t i = 0; i < node->as.try_stmt.catch_block_count; i++) {
      for (size_t j = 0; j < node->as.try_stmt.catch_blocks[i].catch_block_size;
           j++) {
        process_comprehension_symbols_recursive(
            node->as.try_stmt.catch_blocks[i].catch_block[j], tail);
      }
    }
    for (size_t i = 0; i < node->as.try_stmt.finally_block_size; i++) {
      process_comprehension_symbols_recursive(node->as.try_stmt.finally_block[i],
                                              tail);
    }
    break;
  case AST_RAISE:
    process_comprehension_symbols_recursive(node->as.raise_stmt.message, tail);
    break;
  case AST_LAMBDA:
    if (node->as.lambda.is_single_line) {
      process_comprehension_symbols_recursive(node->as.lambda.body_expr, tail);
    } else {
      for (size_t i = 0; i < node->as.lambda.block_size; i++) {
        process_comprehension_symbols_recursive(node->as.lambda.block[i], tail);
      }
    }
    break;
  case AST_FSTRING:
    for (size_t i = 0; i < node->as.fstring.part_count; i++) {
      process_comprehension_symbols_recursive(node->as.fstring.parts[i], tail);
    }
    break;
  case AST_TUPLE:
    for (size_t i = 0; i < node->as.tuple.element_count; i++) {
      process_comprehension_symbols_recursive(node->as.tuple.elements[i], tail);
    }
    break;
  case AST_UNPACK_ASSIGN:
    process_comprehension_symbols_recursive(node->as.unpack_assign.value, tail);
    break;
  default:
    break;
  }
}

static bool symbol_has_known_position(const Symbol *sym) {
  return sym && sym->line > 0 && sym->column > 0;
}

static bool node_scope_contains_symbol(ASTNode *node, const Symbol *sym) {
  if (!node || !symbol_has_known_position(sym)) {
    return false;
  }

  NodePositionBounds bounds;
  get_node_position_bounds(node, &bounds);
  if (!bounds.has_position) {
    return false;
  }

  return position_in_range(sym->line, sym->column, bounds.start_line,
                           bounds.start_column, bounds.end_line,
                           bounds.end_column);
}

static bool should_search_within_scope(ASTNode *node, const Symbol *target_sym) {
  if (!target_sym || !symbol_has_known_position(target_sym)) {
    return true;
  }

  return node_scope_contains_symbol(node, target_sym);
}

static bool loop_declaration_matches_symbol(ASTNode *node, const char *decl_name,
                                            const char *target_name,
                                            const Symbol *target_sym) {
  if (!decl_name || !target_name || strcmp(decl_name, target_name) != 0) {
    return false;
  }

  if (!target_sym || !symbol_has_known_position(target_sym)) {
    return true;
  }

  size_t decl_line = 1;
  size_t decl_col = 1;
  get_node_position(node, &decl_line, &decl_col);
  return decl_line == target_sym->line && decl_col == target_sym->column;
}

static bool node_declares_loop_variable(ASTNode *node, const char *name,
                                        const Symbol *target_sym) {
  if (!node || !name) {
    return false;
  }

  switch (node->type) {
  case AST_FOR:
    if (loop_declaration_matches_symbol(node, node->as.for_stmt.var, name,
                                        target_sym)) {
      return true;
    }
    if (node_declares_loop_variable(node->as.for_stmt.iterable, name,
                                    target_sym) ||
        node_declares_loop_variable(node->as.for_stmt.end, name, target_sym) ||
        node_declares_loop_variable(node->as.for_stmt.step, name, target_sym)) {
      return true;
    }
    for (size_t i = 0; i < node->as.for_stmt.block_size; i++) {
      if (node_declares_loop_variable(node->as.for_stmt.block[i], name,
                                      target_sym)) {
        return true;
      }
    }
    return false;
  case AST_LIST_COMPREHENSION:
    if (!should_search_within_scope(node, target_sym)) {
      return false;
    }
    if (loop_declaration_matches_symbol(node, node->as.list_comprehension.var,
                                        name, target_sym)) {
      return true;
    }
    return node_declares_loop_variable(
               node->as.list_comprehension.element_expr, name, target_sym) ||
           node_declares_loop_variable(node->as.list_comprehension.iterable,
                                       name, target_sym) ||
           node_declares_loop_variable(node->as.list_comprehension.condition,
                                       name, target_sym);
  case AST_ASSIGN:
    return node_declares_loop_variable(node->as.assign.value, name, target_sym);
  case AST_PRINT:
    return node_declares_loop_variable(node->as.print.value, name, target_sym);
  case AST_DEBUG:
    for (size_t i = 0; i < node->as.debug_stmt.value_count; i++) {
      if (node_declares_loop_variable(node->as.debug_stmt.values[i], name,
                                      target_sym)) {
        return true;
      }
    }
    return false;
  case AST_BINOP:
    return node_declares_loop_variable(node->as.binop.left, name, target_sym) ||
           node_declares_loop_variable(node->as.binop.right, name, target_sym);
  case AST_IF:
    if (node_declares_loop_variable(node->as.if_stmt.condition, name,
                                    target_sym)) {
      return true;
    }
    for (size_t i = 0; i < node->as.if_stmt.block_size; i++) {
      if (node_declares_loop_variable(node->as.if_stmt.block[i], name,
                                      target_sym)) {
        return true;
      }
    }
    for (size_t i = 0; i < node->as.if_stmt.else_if_count; i++) {
      if (node_declares_loop_variable(node->as.if_stmt.else_if_conditions[i],
                                      name, target_sym)) {
        return true;
      }
      for (size_t j = 0; j < node->as.if_stmt.else_if_block_sizes[i]; j++) {
        if (node_declares_loop_variable(node->as.if_stmt.else_if_blocks[i][j],
                                        name, target_sym)) {
          return true;
        }
      }
    }
    for (size_t i = 0; i < node->as.if_stmt.else_block_size; i++) {
      if (node_declares_loop_variable(node->as.if_stmt.else_block[i], name,
                                      target_sym)) {
        return true;
      }
    }
    return false;
  case AST_MATCH:
    if (node_declares_loop_variable(node->as.match_stmt.value, name,
                                    target_sym)) {
      return true;
    }
    for (size_t i = 0; i < node->as.match_stmt.case_count; i++) {
      if (node_declares_loop_variable(node->as.match_stmt.case_patterns[i],
                                      name, target_sym)) {
        return true;
      }
      for (size_t j = 0; j < node->as.match_stmt.case_block_sizes[i]; j++) {
        if (node_declares_loop_variable(node->as.match_stmt.case_blocks[i][j],
                                        name, target_sym)) {
          return true;
        }
      }
    }
    for (size_t i = 0; i < node->as.match_stmt.default_block_size; i++) {
      if (node_declares_loop_variable(node->as.match_stmt.default_block[i],
                                      name, target_sym)) {
        return true;
      }
    }
    return false;
  case AST_WHILE:
    if (node_declares_loop_variable(node->as.while_stmt.condition, name,
                                    target_sym)) {
      return true;
    }
    for (size_t i = 0; i < node->as.while_stmt.block_size; i++) {
      if (node_declares_loop_variable(node->as.while_stmt.block[i], name,
                                      target_sym)) {
        return true;
      }
    }
    return false;
  case AST_FUNCTION:
    if (!should_search_within_scope(node, target_sym)) {
      return false;
    }
    for (size_t i = 0; i < node->as.function.block_size; i++) {
      if (node_declares_loop_variable(node->as.function.block[i], name,
                                      target_sym)) {
        return true;
      }
    }
    return false;
  case AST_CALL:
    for (size_t i = 0; i < node->as.call.arg_count; i++) {
      if (node_declares_loop_variable(node->as.call.args[i], name,
                                      target_sym)) {
        return true;
      }
    }
    return false;
  case AST_RETURN:
    for (size_t i = 0; i < node->as.return_stmt.value_count; i++) {
      if (node_declares_loop_variable(node->as.return_stmt.values[i], name,
                                      target_sym)) {
        return true;
      }
    }
    return false;
  case AST_LIST:
    for (size_t i = 0; i < node->as.list.element_count; i++) {
      if (node_declares_loop_variable(node->as.list.elements[i], name,
                                      target_sym)) {
        return true;
      }
    }
    return false;
  case AST_RANGE:
    return node_declares_loop_variable(node->as.range.start, name, target_sym) ||
           node_declares_loop_variable(node->as.range.end, name, target_sym) ||
           node_declares_loop_variable(node->as.range.step, name, target_sym);
  case AST_MAP:
    for (size_t i = 0; i < node->as.map.entry_count; i++) {
      if (node_declares_loop_variable(node->as.map.keys[i], name, target_sym) ||
          node_declares_loop_variable(node->as.map.values[i], name,
                                      target_sym)) {
        return true;
      }
    }
    return false;
  case AST_INDEX:
    return node_declares_loop_variable(node->as.index.list_expr, name,
                                       target_sym) ||
           node_declares_loop_variable(node->as.index.index, name, target_sym);
  case AST_SLICE:
    return node_declares_loop_variable(node->as.slice.list_expr, name,
                                       target_sym) ||
           node_declares_loop_variable(node->as.slice.start, name, target_sym) ||
           node_declares_loop_variable(node->as.slice.end, name, target_sym);
  case AST_ASSIGN_INDEX:
    return node_declares_loop_variable(node->as.assign_index.target, name,
                                       target_sym) ||
           node_declares_loop_variable(node->as.assign_index.index, name,
                                       target_sym) ||
           node_declares_loop_variable(node->as.assign_index.value, name,
                                       target_sym);
  case AST_DELETE:
    return node_declares_loop_variable(node->as.delete_stmt.target, name,
                                       target_sym) ||
           node_declares_loop_variable(node->as.delete_stmt.key, name,
                                       target_sym);
  case AST_TRY:
    for (size_t i = 0; i < node->as.try_stmt.try_block_size; i++) {
      if (node_declares_loop_variable(node->as.try_stmt.try_block[i], name,
                                      target_sym)) {
        return true;
      }
    }
    for (size_t i = 0; i < node->as.try_stmt.catch_block_count; i++) {
      for (size_t j = 0; j < node->as.try_stmt.catch_blocks[i].catch_block_size;
           j++) {
        if (node_declares_loop_variable(
                node->as.try_stmt.catch_blocks[i].catch_block[j], name,
                target_sym)) {
          return true;
        }
      }
    }
    for (size_t i = 0; i < node->as.try_stmt.finally_block_size; i++) {
      if (node_declares_loop_variable(node->as.try_stmt.finally_block[i],
                                      name, target_sym)) {
        return true;
      }
    }
    return false;
  case AST_RAISE:
    return node_declares_loop_variable(node->as.raise_stmt.message, name,
                                       target_sym);
  case AST_LAMBDA:
    if (!should_search_within_scope(node, target_sym)) {
      return false;
    }
    if (node->as.lambda.is_single_line) {
      return node_declares_loop_variable(node->as.lambda.body_expr, name,
                                         target_sym);
    }
    for (size_t i = 0; i < node->as.lambda.block_size; i++) {
      if (node_declares_loop_variable(node->as.lambda.block[i], name,
                                      target_sym)) {
        return true;
      }
    }
    return false;
  case AST_FSTRING:
    for (size_t i = 0; i < node->as.fstring.part_count; i++) {
      if (node_declares_loop_variable(node->as.fstring.parts[i], name,
                                      target_sym)) {
        return true;
      }
    }
    return false;
  case AST_TUPLE:
    for (size_t i = 0; i < node->as.tuple.element_count; i++) {
      if (node_declares_loop_variable(node->as.tuple.elements[i], name,
                                      target_sym)) {
        return true;
      }
    }
    return false;
  case AST_UNPACK_ASSIGN:
    return node_declares_loop_variable(node->as.unpack_assign.value, name,
                                       target_sym);
  default:
    return false;
  }
}

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
    // Free parameter names array for functions
    if (sym->param_names) {
      for (size_t i = 0; i < sym->param_count; i++) {
        free(sym->param_names[i]);
      }
      free(sym->param_names);
    }
    free(sym);
    sym = next;
  }
}

void get_node_position(ASTNode *node, size_t *line, size_t *col) {
  *line = 1;
  *col = 1;
  if (!node) {
    return;
  }

  // Prefer parser-provided source positions when available.
  if (node->line > 0 && node->column > 0) {
    *line = node->line;
    *col = node->column;
    return;
  }

  // Fallback for nodes that still don't have explicit source positions.
  if (node->indent >= 0) {
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
      Symbol *sym = allocate_symbol();
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
        sym->required_param_count = node->as.function.required_param_count;
        sym->has_variadic = node->as.function.has_variadic;
        // Copy parameter names for signature display
        sym->param_names = NULL;
        if (node->as.function.param_count > 0 && node->as.function.params) {
          sym->param_names = malloc(sizeof(char *) * node->as.function.param_count);
          if (sym->param_names) {
            for (size_t pi = 0; pi < node->as.function.param_count; pi++) {
              sym->param_names[pi] = node->as.function.params[pi]
                  ? strdup(node->as.function.params[pi])
                  : NULL;
            }
          }
        }
        sym->written = false;
        sym->read = false;
        sym->next = NULL;
        *tail = sym;
        tail = &sym->next;
      }
    }
    // Extract variable declarations (top-level only)
    else if (node->type == AST_ASSIGN && node->as.assign.name) {
      Symbol *sym = allocate_symbol();
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
        sym->required_param_count = 0;
        sym->has_variadic = false;
        sym->param_names = NULL;
        sym->written = false;
        sym->read = false;
        sym->next = NULL;
        *tail = sym;
        tail = &sym->next;
      }
    } else if (node->type == AST_TYPE_ALIAS && node->as.type_alias.name) {
      Symbol *sym = allocate_symbol();
      if (sym) {
        sym->name = strdup(node->as.type_alias.name);
        sym->type = SYMBOL_TYPE_ALIAS;
        if (source_for_pos) {
          char pattern[LSP_PATTERN_BUFFER_SIZE];
          int n = snprintf(pattern, sizeof(pattern), "type %s to", sym->name);
          if (n >= 0 && (size_t)n < sizeof(pattern)) {
            find_node_position(node, source_for_pos, pattern, &sym->line,
                               &sym->column);
            if (sym->line == 1 && sym->column == 0) {
              get_node_position(node, &sym->line, &sym->column);
            }
          } else {
            get_node_position(node, &sym->line, &sym->column);
          }
        } else {
          get_node_position(node, &sym->line, &sym->column);
        }
        sym->type_name = node->as.type_alias.target_type
                             ? strdup(node->as.type_alias.target_type)
                             : NULL;
        sym->is_mutable = false;
        sym->param_count = 0;
        sym->required_param_count = 0;
        sym->has_variadic = false;
        sym->param_names = NULL;
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
  int ret;

  // Write module name
  size_t remaining = buffer_size - pos;
  if (remaining > 0) {
    ret =
        snprintf(hover_text + pos, remaining, "**module** `%s`\n\n", mod->name);
    if (ret < 0)
      ret = 0;
    if ((size_t)ret >= remaining) {
      pos = buffer_size - 1;
    } else {
      pos += (size_t)ret;
    }
  }

  if (mod->file_path) {
    remaining = buffer_size - pos;
    if (remaining > 0) {
      ret = snprintf(hover_text + pos, remaining, "**Path:** `%s`\n\n",
                     mod->file_path);
      if (ret < 0)
        ret = 0;
      if ((size_t)ret >= remaining) {
        pos = buffer_size - 1;
      } else {
        pos += (size_t)ret;
      }
    }
  } else {
    remaining = buffer_size - pos;
    if (remaining > 0) {
      ret = snprintf(hover_text + pos, remaining,
                     "**Type:** Built-in module\n\n");
      if (ret < 0)
        ret = 0;
      if ((size_t)ret >= remaining) {
        pos = buffer_size - 1;
      } else {
        pos += (size_t)ret;
      }
    }
  }

  if (mod->exports) {
    remaining = buffer_size - pos;
    if (remaining > 0) {
      ret = snprintf(hover_text + pos, remaining, "**Exports:**\n\n");
      if (ret < 0)
        ret = 0;
      if ((size_t)ret >= remaining) {
        pos = buffer_size - 1;
      } else {
        pos += (size_t)ret;
      }
    }
    Symbol *sym = mod->exports;
    int func_count = 0;
    int var_count = 0;
    int alias_count = 0;
    while (sym) {
      if (sym->type == SYMBOL_FUNCTION) {
        func_count++;
        remaining = buffer_size - pos;
        if (remaining > 0) {
          ret = snprintf(hover_text + pos, remaining, "• `%s` (function",
                         sym->name);
          if (ret < 0)
            ret = 0;
          if ((size_t)ret >= remaining) {
            pos = buffer_size - 1;
          } else {
            pos += (size_t)ret;
          }
        }
        if (sym->param_count > 0) {
          remaining = buffer_size - pos;
          if (remaining > 0) {
            ret = snprintf(hover_text + pos, remaining, ", %zu parameter%s",
                           sym->param_count, sym->param_count == 1 ? "" : "s");
            if (ret < 0)
              ret = 0;
            if ((size_t)ret >= remaining) {
              pos = buffer_size - 1;
            } else {
              pos += (size_t)ret;
            }
          }
        } else {
          remaining = buffer_size - pos;
          if (remaining > 0) {
            ret = snprintf(hover_text + pos, remaining, ", no parameters");
            if (ret < 0)
              ret = 0;
            if ((size_t)ret >= remaining) {
              pos = buffer_size - 1;
            } else {
              pos += (size_t)ret;
            }
          }
        }
        remaining = buffer_size - pos;
        if (remaining > 0) {
          ret = snprintf(hover_text + pos, remaining, ")\n");
          if (ret < 0)
            ret = 0;
          if ((size_t)ret >= remaining) {
            pos = buffer_size - 1;
          } else {
            pos += (size_t)ret;
          }
        }
      } else if (sym->type == SYMBOL_VARIABLE) {
        var_count++;
        remaining = buffer_size - pos;
        if (remaining > 0) {
          ret = snprintf(hover_text + pos, remaining, "• `%s` (%s variable",
                         sym->name, sym->is_mutable ? "mutable" : "immutable");
          if (ret < 0)
            ret = 0;
          if ((size_t)ret >= remaining) {
            pos = buffer_size - 1;
          } else {
            pos += (size_t)ret;
          }
        }
        if (sym->type_name) {
          remaining = buffer_size - pos;
          if (remaining > 0) {
            ret = snprintf(hover_text + pos, remaining, ", type: `%s`",
                           sym->type_name);
            if (ret < 0)
              ret = 0;
            if ((size_t)ret >= remaining) {
              pos = buffer_size - 1;
            } else {
              pos += (size_t)ret;
            }
          }
        }
        remaining = buffer_size - pos;
        if (remaining > 0) {
          ret = snprintf(hover_text + pos, remaining, ")\n");
          if (ret < 0)
            ret = 0;
          if ((size_t)ret >= remaining) {
            pos = buffer_size - 1;
          } else {
            pos += (size_t)ret;
          }
        }
      } else if (sym->type == SYMBOL_TYPE_ALIAS) {
        alias_count++;
        remaining = buffer_size - pos;
        if (remaining > 0) {
          ret = snprintf(hover_text + pos, remaining, "• `%s` (type alias",
                         sym->name);
          if (ret < 0)
            ret = 0;
          if ((size_t)ret >= remaining) {
            pos = buffer_size - 1;
          } else {
            pos += (size_t)ret;
          }
        }
        if (sym->type_name) {
          remaining = buffer_size - pos;
          if (remaining > 0) {
            ret = snprintf(hover_text + pos, remaining, " -> `%s`",
                           sym->type_name);
            if (ret < 0)
              ret = 0;
            if ((size_t)ret >= remaining) {
              pos = buffer_size - 1;
            } else {
              pos += (size_t)ret;
            }
          }
        }
        remaining = buffer_size - pos;
        if (remaining > 0) {
          ret = snprintf(hover_text + pos, remaining, ")\n");
          if (ret < 0)
            ret = 0;
          if ((size_t)ret >= remaining) {
            pos = buffer_size - 1;
          } else {
            pos += (size_t)ret;
          }
        }
      }
      sym = sym->next;
    }
    if (func_count == 0 && var_count == 0 && alias_count == 0) {
      remaining = buffer_size - pos;
      if (remaining > 0) {
        ret = snprintf(hover_text + pos, remaining, "No exports found\n");
        if (ret < 0)
          ret = 0;
        if ((size_t)ret >= remaining) {
          pos = buffer_size - 1;
        } else {
          pos += (size_t)ret;
        }
      }
    }
  } else if (mod->file_path) {
    remaining = buffer_size - pos;
    if (remaining > 0) {
      ret = snprintf(hover_text + pos, remaining,
                     "**Exports:** Unable to load\n");
      if (ret < 0)
        ret = 0;
      if ((size_t)ret >= remaining) {
        pos = buffer_size - 1;
      } else {
        pos += (size_t)ret;
      }
    }
  }

  // Ensure null termination
  if (pos >= buffer_size) {
    pos = buffer_size - 1;
  }
  hover_text[pos] = '\0';

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
        sym = allocate_symbol();
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
        sym->required_param_count = 0;
        sym->has_variadic = false;
        sym->param_names = NULL;
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
    case AST_UNPACK_ASSIGN: {
      // Create a symbol for each unpacked variable
      for (size_t j = 0; j < node->as.unpack_assign.name_count; j++) {
        // Check if symbol already exists (for reassignments)
        Symbol *existing = head ? *head : NULL;
        while (existing) {
          if (existing->name &&
              strcmp(existing->name, node->as.unpack_assign.names[j]) == 0 &&
              existing->type == SYMBOL_VARIABLE) {
            existing->written = true;
            break;
          }
          existing = existing->next;
        }

        // Only create new symbol if it doesn't exist
        if (!existing) {
          sym = allocate_symbol();
          if (!sym)
            continue;
          sym->name = strdup(node->as.unpack_assign.names[j]);
          sym->type = SYMBOL_VARIABLE;
          sym->is_mutable = node->as.unpack_assign.is_mutable;
          sym->type_name = NULL;  // No type annotation for unpacking
          sym->param_count = 0;
          sym->required_param_count = 0;
          sym->has_variadic = false;
          sym->param_names = NULL;
          sym->written = true;
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
    case AST_FUNCTION: {
      sym = allocate_symbol();
      if (!sym)
        continue;
      sym->name = strdup(node->as.function.name);
      sym->type = SYMBOL_FUNCTION;
      sym->is_mutable = false;
      sym->type_name = NULL;
      sym->param_count = node->as.function.param_count;
      sym->required_param_count = node->as.function.required_param_count;
      sym->has_variadic = node->as.function.has_variadic;
      // Copy parameter names for signature display
      sym->param_names = NULL;
      if (node->as.function.param_count > 0 && node->as.function.params) {
        sym->param_names = malloc(sizeof(char *) * node->as.function.param_count);
        if (sym->param_names) {
          for (size_t pi = 0; pi < node->as.function.param_count; pi++) {
            sym->param_names[pi] = node->as.function.params[pi]
                ? strdup(node->as.function.params[pi])
                : NULL;
          }
        }
      }
      sym->written = false;
      sym->read = false;
      get_node_position(node, &line, &col);
      sym->line = line;
      sym->column = col;
      sym->next = NULL;
      **tail = sym;
      *tail = &sym->next;

      // Add parameters as symbols
      if (node->as.function.param_count > 0 && node->as.function.params) {
        for (size_t j = 0; j < node->as.function.param_count; j++) {
          const char *param_name = node->as.function.params[j];
          if (!param_name)
            continue;

          Symbol *param = allocate_symbol();
          if (!param)
            continue;

          param->name = strdup(param_name);
          if (!param->name) {
            free(param);
            continue;
          }

          param->type = SYMBOL_PARAMETER;
          param->is_mutable = false;
          param->type_name = NULL;
          param->param_count = 0;
          param->required_param_count = 0;
          param->has_variadic = false;
          param->param_names = NULL;
          param->written = false; // Parameters are passed in, not written
          param->read = false;
          param->line = line;
          param->column = col;
          param->next = NULL;
          **tail = param;
          *tail = &param->next;
        }
      }

      // Recursively process function body to add local variables
      if (node->as.function.block && node->as.function.block_size > 0) {
        process_statements_for_symbols(
            node->as.function.block, node->as.function.block_size, tail, head);
      }
      break;
    }
    case AST_TYPE_ALIAS: {
      Symbol *existing = head ? *head : NULL;
      while (existing) {
        if (existing->name &&
            strcmp(existing->name, node->as.type_alias.name) == 0 &&
            existing->type == SYMBOL_TYPE_ALIAS) {
          break;
        }
        existing = existing->next;
      }

      if (!existing) {
        sym = allocate_symbol();
        if (!sym)
          break;
        sym->name = strdup(node->as.type_alias.name);
        sym->type = SYMBOL_TYPE_ALIAS;
        sym->is_mutable = false;
        sym->type_name = node->as.type_alias.target_type
                             ? strdup(node->as.type_alias.target_type)
                             : NULL;
        sym->param_count = 0;
        sym->required_param_count = 0;
        sym->has_variadic = false;
        sym->param_names = NULL;
        sym->written = false;
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
    case AST_FOR: {
      // Add loop variable to symbol table
      if (node->as.for_stmt.var) {
        sym = allocate_symbol();
        if (!sym)
          break;
        sym->name = strdup(node->as.for_stmt.var);
        sym->type = SYMBOL_VARIABLE;
        sym->is_mutable =
            false; // Loop variables are immutable (assigned by loop)
        sym->type_name = NULL; // Type depends on what's being iterated
        sym->param_count = 0;
        sym->required_param_count = 0;
        sym->has_variadic = false;
        sym->param_names = NULL;
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
          sym = allocate_symbol();
          if (!sym)
            break;
          sym->name = strdup(node->as.try_stmt.catch_blocks[j].catch_var);
          sym->type = SYMBOL_VARIABLE;
          sym->is_mutable = false;           // Catch variables are immutable
          sym->type_name = strdup("string"); // Error messages are strings
          sym->param_count = 0;
          sym->required_param_count = 0;
          sym->has_variadic = false;
          sym->param_names = NULL;
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
    case AST_MATCH: {
      for (size_t j = 0; j < node->as.match_stmt.case_count; j++) {
        if (node->as.match_stmt.case_blocks[j] &&
            node->as.match_stmt.case_block_sizes[j] > 0) {
          process_statements_for_symbols(node->as.match_stmt.case_blocks[j],
                                         node->as.match_stmt.case_block_sizes[j],
                                         tail, head);
        }
      }
      if (node->as.match_stmt.default_block &&
          node->as.match_stmt.default_block_size > 0) {
        process_statements_for_symbols(node->as.match_stmt.default_block,
                                       node->as.match_stmt.default_block_size,
                                       tail, head);
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

  for (size_t i = 0; i < ast->count; i++) {
    process_comprehension_symbols_recursive(ast->statements[i], &tail);
  }

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
      strcmp(func_name, "filter") == 0 || strcmp(func_name, "map") == 0 ||
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

static bool is_match_inside_quoted_string(const char *line_start,
                                          const char *pos) {
  if (!line_start || !pos || pos < line_start) {
    return false;
  }

  bool in_single_quote = false;
  bool in_double_quote = false;
  bool escaped = false;
  for (const char *cursor = line_start; cursor < pos; cursor++) {
    char ch = *cursor;

    if (escaped) {
      escaped = false;
      continue;
    }

    if (ch == '\\') {
      escaped = true;
      continue;
    }

    if (ch == '"' && !in_single_quote) {
      in_double_quote = !in_double_quote;
      continue;
    }

    if (ch == '\'' && !in_double_quote) {
      in_single_quote = !in_single_quote;
      continue;
    }
  }

  return in_single_quote || in_double_quote;
}

bool find_call_expression_position(const char *text, const char *func_name,
                                   size_t preferred_line, size_t *line,
                                   size_t *col, size_t *length) {
  *line = 1;
  *col = 0;
  *length = 0;
  if (!text || !func_name)
    return false;

  // Search for "call <func_name> with" pattern
  char pattern[256];
  int n = snprintf(pattern, sizeof(pattern), "call %s with", func_name);
  if (n < 0 || (size_t)n >= sizeof(pattern)) {
    // Pattern too long, cannot search
    return false;
  }

  const char *pos = text;
  bool have_fallback = false;
  size_t fallback_line = 1;
  size_t fallback_col = 0;
  size_t fallback_length = 0;
  while ((pos = strstr(pos, pattern)) != NULL) {
    // Check if this is the actual call (not part of a comment)
    const char *line_start = pos;
    while (line_start > text && *(line_start - 1) != '\n') {
      line_start--;
    }

    const char *line_end = pos;
    while (*line_end != '\0' && *line_end != '\n') {
      line_end++;
    }

    // Skip leading whitespace
    const char *first_non_ws = line_start;
    while (first_non_ws < line_end &&
           (*first_non_ws == ' ' || *first_non_ws == '\t')) {
      first_non_ws++;
    }
    // Skip comment lines
    if (first_non_ws < line_end && *first_non_ws == '#') {
      pos += strlen(pattern);
      continue;
    }

    if (is_match_inside_quoted_string(line_start, pos)) {
      pos += strlen(pattern);
      continue;
    }

    // Count lines up to this position
    size_t current_line = 1;
    for (const char *p = text; p < pos; p++) {
      if (*p == '\n')
        current_line++;
    }
    size_t current_col = (size_t)(pos - line_start);

    // Highlight only the call expression, excluding inline comments and trailing
    // whitespace.
    const char *range_end = line_end;
    bool in_single_quote = false;
    bool in_double_quote = false;
    bool escaped = false;
    for (const char *cursor = pos; cursor < line_end; cursor++) {
      char ch = *cursor;

      if (escaped) {
        escaped = false;
        continue;
      }

      if (ch == '\\') {
        escaped = true;
        continue;
      }

      if (ch == '"' && !in_single_quote) {
        in_double_quote = !in_double_quote;
        continue;
      }

      if (ch == '\'' && !in_double_quote) {
        in_single_quote = !in_single_quote;
        continue;
      }

      if (ch == '#' && !in_single_quote && !in_double_quote) {
        range_end = cursor;
        break;
      }
    }

    while (range_end > pos &&
           (*(range_end - 1) == ' ' || *(range_end - 1) == '\t' ||
            *(range_end - 1) == '\r')) {
      range_end--;
    }

    if (range_end <= pos) {
      pos += strlen(pattern);
      continue;
    }

    size_t current_length = (size_t)(range_end - pos);
    if (current_length == 0) {
      pos += strlen(pattern);
      continue;
    }

    if (preferred_line == 0 || current_line == preferred_line) {
      *line = current_line;
      *col = current_col;
      *length = current_length;
      return true;
    }

    if (!have_fallback) {
      have_fallback = true;
      fallback_line = current_line;
      fallback_col = current_col;
      fallback_length = current_length;
    }

    pos += strlen(pattern);
  }

  if (have_fallback) {
    *line = fallback_line;
    *col = fallback_col;
    *length = fallback_length;
    return true;
  }

  return false;
}

bool find_call_argument_position_by_index(const char *text,
                                          const char *func_name,
                                          size_t preferred_line,
                                          size_t arg_index, size_t *line,
                                          size_t *col, size_t *length) {
  *line = 1;
  *col = 0;
  *length = 0;
  if (!text || !func_name)
    return false;

  char pattern[256];
  int n = snprintf(pattern, sizeof(pattern), "call %s with", func_name);
  if (n < 0 || (size_t)n >= sizeof(pattern)) {
    return false;
  }

  const char *pos = text;
  while ((pos = strstr(pos, pattern)) != NULL) {
    const char *line_start = pos;
    while (line_start > text && *(line_start - 1) != '\n') {
      line_start--;
    }

    const char *line_end = pos;
    while (*line_end != '\0' && *line_end != '\n') {
      line_end++;
    }

    const char *first_non_ws = line_start;
    while (first_non_ws < line_end &&
           (*first_non_ws == ' ' || *first_non_ws == '\t')) {
      first_non_ws++;
    }
    if (first_non_ws < line_end && *first_non_ws == '#') {
      pos += strlen(pattern);
      continue;
    }

    if (is_match_inside_quoted_string(line_start, pos)) {
      pos += strlen(pattern);
      continue;
    }

    size_t current_line = 1;
    for (const char *p = text; p < pos; p++) {
      if (*p == '\n')
        current_line++;
    }
    if (preferred_line != 0 && current_line != preferred_line) {
      pos += strlen(pattern);
      continue;
    }

    // Ignore inline comments when splitting arguments.
    const char *statement_end = line_end;
    bool in_single_quote = false;
    bool in_double_quote = false;
    bool escaped = false;
    for (const char *cursor = pos; cursor < line_end; cursor++) {
      char ch = *cursor;
      if (escaped) {
        escaped = false;
        continue;
      }
      if (ch == '\\') {
        escaped = true;
        continue;
      }
      if (ch == '"' && !in_single_quote) {
        in_double_quote = !in_double_quote;
        continue;
      }
      if (ch == '\'' && !in_double_quote) {
        in_single_quote = !in_single_quote;
        continue;
      }
      if (ch == '#' && !in_single_quote && !in_double_quote) {
        statement_end = cursor;
        break;
      }
    }

    const char *args_start = pos + strlen(pattern);
    while (args_start < statement_end &&
           (*args_start == ' ' || *args_start == '\t')) {
      args_start++;
    }
    if (args_start >= statement_end) {
      if (preferred_line != 0)
        return false;
      pos += strlen(pattern);
      continue;
    }

    size_t current_arg_index = 0;
    const char *arg_start = args_start;
    while (arg_start < statement_end) {
      while (arg_start < statement_end &&
             (*arg_start == ' ' || *arg_start == '\t')) {
        arg_start++;
      }
      if (arg_start >= statement_end)
        break;

      const char *cursor = arg_start;
      bool arg_in_single_quote = false;
      bool arg_in_double_quote = false;
      bool arg_escaped = false;
      int paren_depth = 0;
      int bracket_depth = 0;
      int brace_depth = 0;
      for (; cursor < statement_end; cursor++) {
        char ch = *cursor;
        if (arg_escaped) {
          arg_escaped = false;
          continue;
        }
        if (ch == '\\') {
          arg_escaped = true;
          continue;
        }
        if (ch == '"' && !arg_in_single_quote) {
          arg_in_double_quote = !arg_in_double_quote;
          continue;
        }
        if (ch == '\'' && !arg_in_double_quote) {
          arg_in_single_quote = !arg_in_single_quote;
          continue;
        }
        if (arg_in_single_quote || arg_in_double_quote) {
          continue;
        }
        if (ch == '(') {
          paren_depth++;
          continue;
        }
        if (ch == ')' && paren_depth > 0) {
          paren_depth--;
          continue;
        }
        if (ch == '[') {
          bracket_depth++;
          continue;
        }
        if (ch == ']' && bracket_depth > 0) {
          bracket_depth--;
          continue;
        }
        if (ch == '{') {
          brace_depth++;
          continue;
        }
        if (ch == '}' && brace_depth > 0) {
          brace_depth--;
          continue;
        }
        if (ch == ',' && paren_depth == 0 && bracket_depth == 0 &&
            brace_depth == 0) {
          break;
        }
      }

      const char *arg_end = cursor;
      while (arg_end > arg_start &&
             (*(arg_end - 1) == ' ' || *(arg_end - 1) == '\t' ||
              *(arg_end - 1) == '\r')) {
        arg_end--;
      }

      if (current_arg_index == arg_index) {
        if (arg_end <= arg_start)
          return false;
        *line = current_line;
        *col = (size_t)(arg_start - line_start);
        *length = (size_t)(arg_end - arg_start);
        return true;
      }

      if (cursor >= statement_end)
        break;

      current_arg_index++;
      arg_start = cursor + 1;
    }

    if (preferred_line != 0)
      return false;

    pos += strlen(pattern);
  }

  return false;
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

  for (size_t i = 0; i < ast->count; i++) {
    if (node_declares_loop_variable(ast->statements[i], sym->name, sym)) {
      return true;
    }
  }

  return false;
}

static bool symbol_has_scope_range(const Symbol *sym) {
  return sym && sym->is_block_local && sym->scope_start_line > 0 &&
         sym->scope_start_column > 0 && sym->scope_end_line > 0 &&
         sym->scope_end_column > 0;
}

static bool symbol_visible_at_position(const Symbol *sym, size_t line,
                                       size_t col) {
  if (!sym) {
    return false;
  }
  if (!sym->is_block_local) {
    return true;
  }
  if (!symbol_has_scope_range(sym)) {
    // If a local symbol has missing range metadata, keep previous behavior.
    return true;
  }
  return position_in_range(line, col, sym->scope_start_line,
                           sym->scope_start_column, sym->scope_end_line,
                           sym->scope_end_column);
}

static void get_symbol_scope_span(const Symbol *sym, size_t *line_span,
                                  size_t *col_span) {
  if (!line_span || !col_span || !symbol_has_scope_range(sym)) {
    if (line_span) {
      *line_span = SIZE_MAX;
    }
    if (col_span) {
      *col_span = SIZE_MAX;
    }
    return;
  }

  if (sym->scope_end_line < sym->scope_start_line) {
    *line_span = 0;
    *col_span = 0;
    return;
  }

  *line_span = sym->scope_end_line - sym->scope_start_line;
  if (*line_span == 0 && sym->scope_end_column >= sym->scope_start_column) {
    *col_span = sym->scope_end_column - sym->scope_start_column;
  } else {
    *col_span = sym->scope_end_column;
  }
}

static bool prefer_symbol_candidate(const Symbol *candidate, const Symbol *best) {
  if (!candidate) {
    return false;
  }
  if (!best) {
    return true;
  }

  if (candidate->is_block_local != best->is_block_local) {
    return candidate->is_block_local;
  }

  if (candidate->is_block_local && best->is_block_local) {
    size_t candidate_line_span = 0;
    size_t candidate_col_span = 0;
    size_t best_line_span = 0;
    size_t best_col_span = 0;
    get_symbol_scope_span(candidate, &candidate_line_span, &candidate_col_span);
    get_symbol_scope_span(best, &best_line_span, &best_col_span);

    if (candidate_line_span != best_line_span) {
      return candidate_line_span < best_line_span;
    }
    if (candidate_col_span != best_col_span) {
      return candidate_col_span < best_col_span;
    }
  }

  // Preserve previous behavior for ties and global symbols (first declaration).
  return false;
}

static Symbol *find_symbol_at_source_position(const char *name, size_t line,
                                              size_t col) {
  if (!g_doc || !name) {
    return NULL;
  }

  Symbol *best = NULL;
  for (Symbol *sym = g_doc->symbols; sym; sym = sym->next) {
    if (!sym->name || strcmp(sym->name, name) != 0) {
      continue;
    }
    if (!symbol_visible_at_position(sym, line, col)) {
      continue;
    }
    if (prefer_symbol_candidate(sym, best)) {
      best = sym;
      continue;
    }
    if (!best) {
      best = sym;
    }
  }

  return best;
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

Symbol *find_symbol_at_position(const char *const name, size_t line,
                                size_t character) {
  if (!name) {
    return NULL;
  }

  if (line == SIZE_MAX || character == SIZE_MAX) {
    return find_symbol(name);
  }

  return find_symbol_at_source_position(name, line + 1, character + 1);
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

static bool node_reference_matches_symbol(const ReferenceCountContext *ctx,
                                          ASTNode *node, const char *name) {
  if (!ctx || !node || !name || !ctx->symbol_name) {
    return false;
  }
  if (strcmp(name, ctx->symbol_name) != 0) {
    return false;
  }
  if (!ctx->target_symbol) {
    return true;
  }

  size_t line = 1;
  size_t col = 1;
  get_node_position(node, &line, &col);
  Symbol *resolved = find_symbol_at_source_position(ctx->symbol_name, line, col);
  return resolved == ctx->target_symbol;
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
  case AST_PRINT:
    if (node->as.print.value) {
      count_references_in_node_recursive(node->as.print.value, ctx, depth + 1);
    }
    break;
  case AST_DEBUG:
    for (size_t i = 0; i < node->as.debug_stmt.value_count; i++) {
      if (node->as.debug_stmt.values[i]) {
        count_references_in_node_recursive(node->as.debug_stmt.values[i], ctx,
                                           depth + 1);
      }
    }
    break;

  case AST_ASSIGN:
    if (node->as.assign.name &&
        node_reference_matches_symbol(ctx, node, node->as.assign.name)) {
      ctx->count++; // Definition counts as a reference
    }
    if (node->as.assign.value) {
      count_references_in_node_recursive(node->as.assign.value, ctx, depth + 1);
    }
    break;

  case AST_UNPACK_ASSIGN:
    // Check each unpacking target for references
    for (size_t i = 0; i < node->as.unpack_assign.name_count; i++) {
      if (node->as.unpack_assign.names[i] &&
          node_reference_matches_symbol(ctx, node,
                                        node->as.unpack_assign.names[i])) {
        ctx->count++;
      }
    }
    if (node->as.unpack_assign.value) {
      count_references_in_node_recursive(node->as.unpack_assign.value, ctx, depth + 1);
    }
    break;

  case AST_TUPLE:
    // Search each tuple element for references
    for (size_t i = 0; i < node->as.tuple.element_count; i++) {
      if (node->as.tuple.elements[i]) {
        count_references_in_node_recursive(node->as.tuple.elements[i], ctx, depth + 1);
      }
    }
    break;

  case AST_VAR:
    if (node->as.var_name &&
        node_reference_matches_symbol(ctx, node, node->as.var_name)) {
      ctx->count++;
    }
    break;

  case AST_CALL:
    if (node->as.call.name &&
        node_reference_matches_symbol(ctx, node, node->as.call.name)) {
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
        node_reference_matches_symbol(ctx, node, node->as.function.name)) {
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

  case AST_MATCH:
    count_references_in_node_recursive(node->as.match_stmt.value, ctx,
                                       depth + 1);
    for (size_t i = 0; i < node->as.match_stmt.case_count; i++) {
      count_references_in_node_recursive(node->as.match_stmt.case_patterns[i],
                                         ctx, depth + 1);
      for (size_t j = 0; j < node->as.match_stmt.case_block_sizes[i]; j++) {
        count_references_in_node_recursive(node->as.match_stmt.case_blocks[i][j],
                                           ctx, depth + 1);
      }
    }
    for (size_t i = 0; i < node->as.match_stmt.default_block_size; i++) {
      count_references_in_node_recursive(node->as.match_stmt.default_block[i],
                                         ctx, depth + 1);
    }
    break;

  case AST_FOR:
    if (node->as.for_stmt.var &&
        node_reference_matches_symbol(ctx, node, node->as.for_stmt.var)) {
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
    for (size_t i = 0; i < node->as.return_stmt.value_count; i++) {
      if (node->as.return_stmt.values[i]) {
        count_references_in_node_recursive(node->as.return_stmt.values[i], ctx,
                                           depth + 1);
      }
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

  case AST_LIST_COMPREHENSION:
    if (node->as.list_comprehension.var &&
        node_reference_matches_symbol(ctx, node, node->as.list_comprehension.var)) {
      ctx->count++;
    }
    count_references_in_node_recursive(node->as.list_comprehension.element_expr,
                                       ctx, depth + 1);
    count_references_in_node_recursive(node->as.list_comprehension.iterable,
                                       ctx, depth + 1);
    count_references_in_node_recursive(node->as.list_comprehension.condition,
                                       ctx, depth + 1);
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
          node_reference_matches_symbol(
              ctx, node, node->as.try_stmt.catch_blocks[i].catch_var)) {
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

  const Symbol *target_symbol = find_symbol(symbol_name);
  ReferenceCountContext ctx = {symbol_name, target_symbol, 0};

  // Count references in all top-level statements
  for (size_t i = 0; i < ast->count; i++) {
    if (ast->statements[i]) {
      count_references_in_node_recursive(ast->statements[i], &ctx, 0);
    }
  }

  return ctx.count;
}

size_t count_symbol_references_for_symbol(const Symbol *symbol, AST *ast) {
  if (!symbol || !symbol->name || !ast || !ast->statements) {
    return 0;
  }

  ReferenceCountContext ctx = {symbol->name, symbol, 0};

  for (size_t i = 0; i < ast->count; i++) {
    if (ast->statements[i]) {
      count_references_in_node_recursive(ast->statements[i], &ctx, 0);
    }
  }

  return ctx.count;
}
