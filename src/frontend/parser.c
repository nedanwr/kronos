/**
 * @file parser.c
 * @brief Parser for Kronos source code
 *
 * Implements recursive descent parsing to build an Abstract Syntax Tree (AST)
 * from tokens. Handles all Kronos language constructs:
 * - Expressions (arithmetic, comparisons, logical operators)
 * - Statements (assignments, conditionals, loops, functions)
 * - Control flow (if/else, for, while, break, continue)
 * - Data structures (lists, indexing, slicing)
 * - F-strings with embedded expressions
 */

#define _POSIX_C_SOURCE 200809L
#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Parser state structure
 * Tracks current position in the token stream
 */
typedef struct {
  TokenArray *tokens;  /**< Array of tokens to parse */
  size_t pos;          /**< Current position in token array */
} Parser;

/**
 * @brief Look ahead at a token without consuming it
 *
 * @param p Parser state
 * @param offset How many tokens ahead to look (0 = current token)
 * @return Token pointer, or NULL if out of bounds
 */
static Token *peek(Parser *p, int offset) {
  size_t idx = p->pos + offset;
  if (idx >= p->tokens->count)
    return NULL;
  return &p->tokens->tokens[idx];
}

/**
 * @brief Consume a token of the expected type
 *
 * Advances the parser position if the current token matches the expected type.
 * Reports an error if the token doesn't match.
 *
 * @param p Parser state
 * @param expected Expected token type
 * @return Token pointer on success, NULL on mismatch or end of input
 */
static Token *consume(Parser *p, TokenType expected) {
  if (p->pos >= p->tokens->count) {
    fprintf(stderr, "Unexpected end of input\n");
    return NULL;
  }

  Token *tok = &p->tokens->tokens[p->pos];
  if (tok->type != expected) {
    fprintf(stderr, "Expected token type %d, got %d\n", expected, tok->type);
    return NULL;
  }

  p->pos++;
  return tok;
}

/**
 * @brief Consume any token (advance parser position)
 *
 * @param p Parser state
 * @return Token pointer, or NULL if at end of input
 */
static Token *consume_any(Parser *p) {
  if (p->pos >= p->tokens->count)
    return NULL;
  return &p->tokens->tokens[p->pos++];
}

// Forward declarations for recursive parsing functions
static ASTNode *parse_expression(Parser *p);
static ASTNode *parse_condition(Parser *p);
static ASTNode **parse_block(Parser *p, int parent_indent, size_t *block_size);
static ASTNode *parse_assignment(Parser *p, int indent);
static ASTNode *parse_print(Parser *p, int indent);
static ASTNode *parse_if(Parser *p, int indent);
static ASTNode *parse_for(Parser *p, int indent);
static ASTNode *parse_while(Parser *p, int indent);
static ASTNode *parse_function(Parser *p, int indent);
static ASTNode *parse_call(Parser *p, int indent);
static ASTNode *parse_return(Parser *p, int indent);
static ASTNode *parse_import(Parser *p, int indent);
static ASTNode *parse_break(Parser *p, int indent);
static ASTNode *parse_continue(Parser *p, int indent);
static ASTNode *parse_list_literal(Parser *p);
static ASTNode *parse_range_literal(Parser *p);
static ASTNode *parse_map_literal(Parser *p);
static ASTNode *parse_primary(Parser *p);
static ASTNode *parse_fstring(Parser *p);

// Create AST node helpers
static ASTNode *ast_node_new(ASTNodeType type) {
  ASTNode *node = calloc(1, sizeof(ASTNode));
  if (!node)
    return NULL;
  node->type = type;
  return node;
}

static ASTNode *ast_node_new_checked(ASTNodeType type) {
  ASTNode *node = ast_node_new(type);
  if (!node) {
    fprintf(stderr, "Fatal: failed to allocate AST node (type=%d)\n", type);
    exit(EXIT_FAILURE);
  }
  return node;
}

/**
 * @brief Parse a value (literal or variable reference)
 *
 * Handles: numbers, strings, f-strings, booleans, null, variables,
 * list literals, and function calls (when used as expressions).
 *
 * @param p Parser state
 * @return AST node, or NULL on error
 */
static ASTNode *parse_value(Parser *p) {
  Token *tok = peek(p, 0);
  if (!tok)
    return NULL;

  if (tok->type == TOK_NUMBER) {
    consume_any(p);
    ASTNode *node = ast_node_new_checked(AST_NUMBER);
    node->as.number = atof(tok->text);
    return node;
  }

  if (tok->type == TOK_TRUE) {
    consume_any(p);
    ASTNode *node = ast_node_new_checked(AST_BOOL);
    node->as.boolean = true;
    return node;
  }

  if (tok->type == TOK_FALSE) {
    consume_any(p);
    ASTNode *node = ast_node_new_checked(AST_BOOL);
    node->as.boolean = false;
    return node;
  }

  if (tok->type == TOK_NULL || tok->type == TOK_UNDEFINED) {
    consume_any(p);
    ASTNode *node = ast_node_new_checked(AST_NULL);
    return node;
  }

  if (tok->type == TOK_STRING) {
    consume_any(p);
    // Tokenizer already strips quotes, so tok->text contains only the content
    ASTNode *node = ast_node_new_checked(AST_STRING);
    size_t len = tok->length;
    node->as.string.value = malloc(len + 1);
    if (!node->as.string.value) {
      fprintf(stderr, "Memory allocation failed for string value\n");
      free(node);
      return NULL;
    }
    strncpy(node->as.string.value, tok->text, len);
    node->as.string.value[len] = '\0';
    node->as.string.length = len;
    return node;
  }

  if (tok->type == TOK_FSTRING) {
    return parse_fstring(p);
  }

  if (tok->type == TOK_NAME) {
    consume_any(p);
    ASTNode *node = ast_node_new_checked(AST_VAR);
    char *var_name = strdup(tok->text);
    if (!var_name) {
      fprintf(stderr, "Memory allocation failed for variable name\n");
      free(node);
      return NULL;
    }
    node->as.var_name = var_name;
    return node;
  }

  if (tok->type == TOK_LIST) {
    return parse_list_literal(p);
  }

  if (tok->type == TOK_RANGE) {
    return parse_range_literal(p);
  }

  if (tok->type == TOK_MAP) {
    return parse_map_literal(p);
  }

  if (tok->type == TOK_CALL) {
    // Function calls can be used as expressions
    return parse_call(
        p, -1); // -1 indicates expression context (no newline required)
  }

  fprintf(stderr, "Unexpected token in value position\n");
  return NULL;
}

/**
 * @brief Get operator precedence for expression parsing
 *
 * Higher numbers indicate tighter binding (higher precedence).
 * Used by the Pratt parser to correctly handle operator associativity.
 *
 * @param type Token type to check
 * @return Precedence value (0 if not a binary operator)
 */
static int get_precedence(TokenType type) {
  switch (type) {
  case TOK_OR:
    return 1; // Lowest precedence
  case TOK_AND:
    return 2; // Higher than OR
  case TOK_PLUS:
  case TOK_MINUS:
    return 4; // Arithmetic (lower than multiplication)
  case TOK_TIMES:
  case TOK_DIVIDED:
  case TOK_MOD:
    return 5; // Highest arithmetic precedence
  default:
    return 0; // Not a binary operator
  }
}

/**
 * @brief Match natural-language comparison operators
 *
 * Handles Kronos's natural-language comparisons:
 * - "is equal" or "is equal to" -> ==
 * - "is not equal" -> !=
 * - "is greater than" -> >
 * - "is less than" -> <
 * - "is greater than or equal" -> >=
 * - "is less than or equal" -> <=
 *
 * @param p Parser state
 * @param out_op Output parameter for the binary operator type
 * @param tokens_to_consume Output parameter for number of tokens to consume
 * @return true if a comparison operator was matched
 */
static bool match_comparison_operator(Parser *p, BinOp *out_op,
                                      size_t *tokens_to_consume) {
  Token *current = peek(p, 0);
  if (!current || current->type != TOK_IS)
    return false;

  Token *next = peek(p, 1);
  if (!next)
    return false;

  size_t consumed = 0;
  BinOp op = BINOP_EQ;

  if (next->type == TOK_NOT) {
    Token *after_not = peek(p, 2);
    if (!after_not || after_not->type != TOK_EQUAL)
      return false;
    op = BINOP_NEQ;
    consumed = 3; // is not equal
  } else if (next->type == TOK_EQUAL) {
    op = BINOP_EQ;
    consumed = 2; // is equal
  } else if (next->type == TOK_GREATER || next->type == TOK_LESS) {
    bool is_greater = (next->type == TOK_GREATER);
    consumed = 2; // is greater/less
    Token *look = peek(p, consumed);
    if (look && look->type == TOK_THAN) {
      consumed++;
      look = peek(p, consumed);
    }
    bool or_equal = false;
    if (look && look->type == TOK_OR) {
      Token *after_or = peek(p, consumed + 1);
      if (after_or && after_or->type == TOK_EQUAL) {
        or_equal = true;
        consumed += 2;
      }
    }
    op = is_greater ? (or_equal ? BINOP_GTE : BINOP_GT)
                    : (or_equal ? BINOP_LTE : BINOP_LT);
  } else {
    return false;
  }

  // Allow optional trailing "to" (e.g., "is equal to")
  Token *maybe_to = peek(p, consumed);
  if (maybe_to && maybe_to->type == TOK_TO) {
    consumed++;
  }

  if (tokens_to_consume)
    *tokens_to_consume = consumed;
  if (out_op)
    *out_op = op;
  return true;
}

/**
 * @brief Parse a list literal
 *
 * Handles: "list 1, 2, 3" or "list" (empty list).
 * Elements are comma-separated expressions.
 *
 * @param p Parser state
 * @return AST node for the list, or NULL on error
 */
static ASTNode *parse_list_literal(Parser *p) {
  consume(p, TOK_LIST);

  ASTNode **elements = NULL;
  size_t element_count = 0;
  size_t element_capacity = 4;

  elements = malloc(sizeof(ASTNode *) * element_capacity);
  if (!elements) {
    fprintf(stderr, "Failed to allocate memory for list elements\n");
    return NULL;
  }

  // Check if list is empty (next token is not a comma or expression-starting
  // token)
  Token *next = peek(p, 0);
  if (!next || (next->type != TOK_NUMBER && next->type != TOK_STRING &&
                next->type != TOK_TRUE && next->type != TOK_FALSE &&
                next->type != TOK_NULL && next->type != TOK_NAME &&
                next->type != TOK_LIST && next->type != TOK_NOT)) {
    // Empty list
    ASTNode *node = ast_node_new_checked(AST_LIST);
    node->as.list.elements = elements;
    node->as.list.element_count = 0;
    return node;
  }

  // Parse first element
  ASTNode *first = parse_expression(p);
  if (!first) {
    free(elements);
    return NULL;
  }
  elements[element_count++] = first;

  // Parse remaining elements (comma-separated)
  while (peek(p, 0) && peek(p, 0)->type == TOK_COMMA) {
    consume_any(p); // consume comma

    if (element_count == element_capacity) {
      size_t new_capacity = element_capacity * 2;
      ASTNode **new_elements =
          realloc(elements, sizeof(ASTNode *) * new_capacity);
      if (!new_elements) {
        fprintf(stderr, "Failed to grow list elements array\n");
        // Cleanup
        for (size_t i = 0; i < element_count; i++) {
          ast_node_free(elements[i]);
        }
        free(elements);
        return NULL;
      }
      elements = new_elements;
      element_capacity = new_capacity;
    }

    ASTNode *elem = parse_expression(p);
    if (!elem) {
      // Cleanup
      for (size_t i = 0; i < element_count; i++) {
        ast_node_free(elements[i]);
      }
      free(elements);
      return NULL;
    }
    elements[element_count++] = elem;
  }

  ASTNode *node = ast_node_new_checked(AST_LIST);
  node->as.list.elements = elements;
  node->as.list.element_count = element_count;
  return node;
}

/**
 * @brief Parse a range literal
 *
 * Handles: "range start to end" or "range start to end by step".
 * The step is optional and defaults to 1.0.
 *
 * @param p Parser state
 * @return AST node for the range, or NULL on error
 */
static ASTNode *parse_range_literal(Parser *p) {
  consume(p, TOK_RANGE); // consume "range"

  // Parse start expression
  ASTNode *start = parse_expression(p);
  if (!start) {
    return NULL;
  }

  // Expect "to" keyword
  if (!consume(p, TOK_TO)) {
    ast_node_free(start);
    return NULL;
  }

  // Parse end expression
  ASTNode *end = parse_expression(p);
  if (!end) {
    ast_node_free(start);
    return NULL;
  }

  // Check for optional "by step" clause
  ASTNode *step = NULL;
  Token *next = peek(p, 0);
  if (next && next->type == TOK_BY) {
    consume_any(p); // consume "by"
    step = parse_expression(p);
    if (!step) {
      ast_node_free(start);
      ast_node_free(end);
      return NULL;
    }
  }

  ASTNode *node = ast_node_new_checked(AST_RANGE);
  node->as.range.start = start;
  node->as.range.end = end;
  node->as.range.step = step; // NULL means step=1
  return node;
}

/**
 * @brief Parse a map literal
 *
 * Handles: "map key: value, key2: value2" or "map" (empty map).
 * Entries are comma-separated key-value pairs with colon separator.
 *
 * @param p Parser state
 * @return AST node for the map, or NULL on error
 */
static ASTNode *parse_map_literal(Parser *p) {
  consume(p, TOK_MAP);

  ASTNode **keys = NULL;
  ASTNode **values = NULL;
  size_t entry_count = 0;
  size_t entry_capacity = 4;

  keys = malloc(sizeof(ASTNode *) * entry_capacity);
  values = malloc(sizeof(ASTNode *) * entry_capacity);
  if (!keys || !values) {
    if (keys) free(keys);
    if (values) free(values);
    fprintf(stderr, "Failed to allocate memory for map entries\n");
    return NULL;
  }

  // Check if map is empty
  Token *next = peek(p, 0);
  if (!next || (next->type != TOK_NUMBER && next->type != TOK_STRING &&
                next->type != TOK_TRUE && next->type != TOK_FALSE &&
                next->type != TOK_NULL && next->type != TOK_NAME &&
                next->type != TOK_LIST && next->type != TOK_NOT)) {
    // Empty map
    ASTNode *node = ast_node_new_checked(AST_MAP);
    node->as.map.keys = keys;
    node->as.map.values = values;
    node->as.map.entry_count = 0;
    return node;
  }

  // Parse first entry
  // Keys can be identifiers (converted to strings) or expressions
  Token *key_tok = peek(p, 0);
  ASTNode *key = NULL;
  if (key_tok && key_tok->type == TOK_NAME) {
    // Convert identifier to string literal
    consume_any(p); // consume identifier
    key = ast_node_new_checked(AST_STRING);
    key->as.string.value = strdup(key_tok->text);
    key->as.string.length = key_tok->length;
  } else {
    // Parse as expression (for number, string, bool, null keys)
    key = parse_expression(p);
    if (!key) {
      free(keys);
      free(values);
      return NULL;
    }
  }

  // Expect colon
  if (!consume(p, TOK_COLON)) {
    ast_node_free(key);
    free(keys);
    free(values);
    return NULL;
  }

  ASTNode *value = parse_expression(p);
  if (!value) {
    ast_node_free(key);
    free(keys);
    free(values);
    return NULL;
  }

  keys[entry_count] = key;
  values[entry_count] = value;
  entry_count++;

  // Parse remaining entries (comma-separated)
  while (peek(p, 0) && peek(p, 0)->type == TOK_COMMA) {
    consume_any(p); // consume comma

    if (entry_count == entry_capacity) {
      size_t new_capacity = entry_capacity * 2;
      ASTNode **new_keys = realloc(keys, sizeof(ASTNode *) * new_capacity);
      ASTNode **new_values = realloc(values, sizeof(ASTNode *) * new_capacity);
      if (!new_keys || !new_values) {
        if (new_keys) free(new_keys);
        if (new_values) free(new_values);
        // Cleanup
        for (size_t i = 0; i < entry_count; i++) {
          ast_node_free(keys[i]);
          ast_node_free(values[i]);
        }
        free(keys);
        free(values);
        return NULL;
      }
      keys = new_keys;
      values = new_values;
      entry_capacity = new_capacity;
    }

    // Keys can be identifiers (converted to strings) or expressions
    key_tok = peek(p, 0);
    if (key_tok && key_tok->type == TOK_NAME) {
      // Convert identifier to string literal
      consume_any(p); // consume identifier
      key = ast_node_new_checked(AST_STRING);
      key->as.string.value = strdup(key_tok->text);
      key->as.string.length = key_tok->length;
    } else {
      // Parse as expression (for number, string, bool, null keys)
      key = parse_expression(p);
      if (!key) {
        // Cleanup
        for (size_t i = 0; i < entry_count; i++) {
          ast_node_free(keys[i]);
          ast_node_free(values[i]);
        }
        free(keys);
        free(values);
        return NULL;
      }
    }

    // Expect colon
    if (!consume(p, TOK_COLON)) {
      ast_node_free(key);
      // Cleanup
      for (size_t i = 0; i < entry_count; i++) {
        ast_node_free(keys[i]);
        ast_node_free(values[i]);
      }
      free(keys);
      free(values);
      return NULL;
    }

    value = parse_expression(p);
    if (!value) {
      ast_node_free(key);
      // Cleanup
      for (size_t i = 0; i < entry_count; i++) {
        ast_node_free(keys[i]);
        ast_node_free(values[i]);
      }
      free(keys);
      free(values);
      return NULL;
    }

    keys[entry_count] = key;
    values[entry_count] = value;
    entry_count++;
  }

  ASTNode *node = ast_node_new_checked(AST_MAP);
  node->as.map.keys = keys;
  node->as.map.values = values;
  node->as.map.entry_count = entry_count;
  return node;
}

/**
 * @brief Parse an f-string with embedded expressions
 *
 * F-strings allow embedding expressions: f"Hello {name}".
 * Handles nested braces and escape sequences. Each expression
 * is tokenized and parsed separately.
 *
 * @param p Parser state
 * @return AST node for the f-string, or NULL on error
 */
static ASTNode *parse_fstring(Parser *p) {
  Token *tok = consume(p, TOK_FSTRING);
  if (!tok)
    return NULL;

  const char *content = tok->text;
  size_t content_len = tok->length;

  // Allocate parts array (alternating: string, expr, string, expr, ...)
  size_t part_capacity = 4;
  size_t part_count = 0;
  ASTNode **parts = malloc(sizeof(ASTNode *) * part_capacity);
  if (!parts) {
    fprintf(stderr, "Failed to allocate memory for f-string parts\n");
    return NULL;
  }

  size_t i = 0;
  while (i < content_len) {
    // Find next { or end of string
    size_t start = i;
    size_t brace_start = content_len;

    // Look for { (not escaped)
    while (i < content_len) {
      if (content[i] == '\\' && i + 1 < content_len) {
        i += 2; // Skip escaped character
      } else if (content[i] == '{') {
        brace_start = i;
        break;
      } else {
        i++;
      }
    }

    // Add string literal part (from start to brace_start)
    if (brace_start > start) {
      size_t str_len = brace_start - start;
      ASTNode *str_node = ast_node_new_checked(AST_STRING);
      str_node->as.string.value = malloc(str_len + 1);
      if (!str_node->as.string.value) {
        // Cleanup
        for (size_t j = 0; j < part_count; j++) {
          ast_node_free(parts[j]);
        }
        free(parts);
        free(str_node);
        return NULL;
      }
      memcpy(str_node->as.string.value, content + start, str_len);
      str_node->as.string.value[str_len] = '\0';
      str_node->as.string.length = str_len;

      if (part_count >= part_capacity) {
        size_t new_capacity = part_capacity * 2;
        ASTNode **new_parts = realloc(parts, sizeof(ASTNode *) * new_capacity);
        if (!new_parts) {
          ast_node_free(str_node);
          for (size_t j = 0; j < part_count; j++) {
            ast_node_free(parts[j]);
          }
          free(parts);
          return NULL;
        }
        parts = new_parts;
        part_capacity = new_capacity;
      }
      parts[part_count++] = str_node;
    }

    // If we found a {, parse the expression inside
    if (brace_start < content_len) {
      i = brace_start + 1; // Skip {

      // Find matching }
      size_t expr_start = i;
      size_t brace_end = content_len;
      int depth = 1;

      while (i < content_len && depth > 0) {
        if (content[i] == '\\' && i + 1 < content_len) {
          i += 2;
        } else if (content[i] == '{') {
          depth++;
          i++;
        } else if (content[i] == '}') {
          depth--;
          if (depth == 0) {
            brace_end = i;
            break;
          }
          i++;
        } else {
          i++;
        }
      }

      if (depth > 0) {
        fprintf(stderr, "Unmatched { in f-string\n");
        // Cleanup
        for (size_t j = 0; j < part_count; j++) {
          ast_node_free(parts[j]);
        }
        free(parts);
        return NULL;
      }

      // Extract expression string
      size_t expr_len = brace_end - expr_start;
      char *expr_str = malloc(expr_len + 1);
      if (!expr_str) {
        for (size_t j = 0; j < part_count; j++) {
          ast_node_free(parts[j]);
        }
        free(parts);
        return NULL;
      }
      memcpy(expr_str, content + expr_start, expr_len);
      expr_str[expr_len] = '\0';

      // Tokenize and parse the expression
      TokenArray *expr_tokens = tokenize(expr_str, NULL);
      free(expr_str);
      if (!expr_tokens) {
        for (size_t j = 0; j < part_count; j++) {
          ast_node_free(parts[j]);
        }
        free(parts);
        return NULL;
      }

      // Create a temporary parser for the expression
      Parser expr_parser = {expr_tokens, 0};

      // Skip INDENT token if present (tokenizer adds it for each line)
      if (expr_parser.pos < expr_tokens->count &&
          expr_tokens->tokens[expr_parser.pos].type == TOK_INDENT) {
        expr_parser.pos++;
      }

      ASTNode *expr_node = parse_expression(&expr_parser);
      token_array_free(expr_tokens);

      if (!expr_node) {
        for (size_t j = 0; j < part_count; j++) {
          ast_node_free(parts[j]);
        }
        free(parts);
        return NULL;
      }

      if (part_count >= part_capacity) {
        size_t new_capacity = part_capacity * 2;
        ASTNode **new_parts = realloc(parts, sizeof(ASTNode *) * new_capacity);
        if (!new_parts) {
          ast_node_free(expr_node);
          for (size_t j = 0; j < part_count; j++) {
            ast_node_free(parts[j]);
          }
          free(parts);
          return NULL;
        }
        parts = new_parts;
        part_capacity = new_capacity;
      }
      parts[part_count++] = expr_node;

      i = brace_end + 1; // Skip }
    }
  }

  // If no parts, add empty string
  if (part_count == 0) {
    ASTNode *empty_str = ast_node_new_checked(AST_STRING);
    empty_str->as.string.value = malloc(1);
    if (!empty_str->as.string.value) {
      free(parts);
      free(empty_str);
      return NULL;
    }
    empty_str->as.string.value[0] = '\0';
    empty_str->as.string.length = 0;
    parts[part_count++] = empty_str;
  }

  ASTNode *node = ast_node_new_checked(AST_FSTRING);
  node->as.fstring.parts = parts;
  node->as.fstring.part_count = part_count;
  return node;
}

/**
 * @brief Parse a primary expression with postfix operations
 *
 * Parses values, then handles postfix operations:
 * - Indexing: "list at 0"
 * - Slicing: "list from 1 to 5" or "list from 1 to end"
 *
 * @param p Parser state
 * @return AST node, or NULL on error
 */
static ASTNode *parse_primary(Parser *p) {
  ASTNode *expr = parse_value(p);
  if (!expr)
    return NULL;

  // Handle postfix operations: at, from ... to
  while (true) {
    Token *tok = peek(p, 0);
    if (!tok)
      break;

    if (tok->type == TOK_AT) {
      // Indexing: expr at index
      consume_any(p); // consume 'at'
      ASTNode *index = parse_expression(p);
      if (!index) {
        ast_node_free(expr);
        return NULL;
      }

      ASTNode *index_node = ast_node_new_checked(AST_INDEX);
      index_node->as.index.list_expr = expr;
      index_node->as.index.index = index;
      expr = index_node;
    } else if (tok->type == TOK_FROM) {
      // Slicing: expr from start to end
      consume_any(p); // consume 'from'
      ASTNode *start = parse_expression(p);
      if (!start) {
        ast_node_free(expr);
        return NULL;
      }

      if (!peek(p, 0) || peek(p, 0)->type != TOK_TO) {
        ast_node_free(expr);
        ast_node_free(start);
        fprintf(stderr, "Expected 'to' after 'from' in slice\n");
        return NULL;
      }
      consume_any(p); // consume 'to'

      ASTNode *end = NULL;
      if (peek(p, 0) && peek(p, 0)->type == TOK_END) {
        consume_any(p); // consume 'end'
        end = NULL;     // NULL means to end
      } else {
        end = parse_expression(p);
        if (!end) {
          ast_node_free(expr);
          ast_node_free(start);
          return NULL;
        }
      }

      ASTNode *slice_node = ast_node_new_checked(AST_SLICE);
      slice_node->as.slice.list_expr = expr;
      slice_node->as.slice.start = start;
      slice_node->as.slice.end = end;
      expr = slice_node;
    } else {
      // Not a postfix operation, stop
      break;
    }
  }

  return expr;
}

/**
 * @brief Parse an expression using precedence-climbing (Pratt parser)
 *
 * Recursively parses expressions respecting operator precedence and associativity.
 * Handles unary operators (NOT) and binary operators (arithmetic, comparisons, logical).
 *
 * @param p Parser state
 * @param min_prec Minimum precedence to parse (stops when encountering lower precedence)
 * @return AST node for the expression, or NULL on error
 */
static ASTNode *parse_expression_prec(Parser *p, int min_prec) {
  // Handle unary operators (NOT and negation)
  ASTNode *left = NULL;
  Token *tok = peek(p, 0);
  if (tok && tok->type == TOK_NOT) {
    consume_any(p); // consume NOT
    ASTNode *operand =
        parse_expression_prec(p, 10); // High precedence to bind tightly
    if (!operand)
      return NULL;
    ASTNode *node = ast_node_new_checked(AST_BINOP);
    node->as.binop.left = operand; // For unary, we'll use left operand
    node->as.binop.op = BINOP_NOT;
    node->as.binop.right = NULL; // NULL indicates unary operation
    left = node;
  } else if (tok && tok->type == TOK_MINUS) {
    // Check if this is unary negation (not binary subtraction)
    // Peek ahead to see if there's a value after the minus
    Token *next = peek(p, 1);
    if (next && (next->type == TOK_NUMBER || next->type == TOK_NAME ||
                 next->type == TOK_LIST || next->type == TOK_RANGE ||
                 next->type == TOK_MAP || next->type == TOK_CALL ||
                 next->type == TOK_MINUS || next->type == TOK_NOT ||
                 next->type == TOK_TRUE || next->type == TOK_FALSE ||
                 next->type == TOK_NULL || next->type == TOK_UNDEFINED ||
                 next->type == TOK_STRING || next->type == TOK_FSTRING)) {
      consume_any(p); // consume MINUS
      // Parse the operand recursively to handle nested unary operators
      ASTNode *operand = parse_expression_prec(p, 10); // High precedence to bind tightly
      if (!operand)
        return NULL;
      ASTNode *node = ast_node_new_checked(AST_BINOP);
      node->as.binop.left = operand;
      node->as.binop.op = BINOP_NEG; // Use a new unary operator
      node->as.binop.right = NULL; // NULL indicates unary operation
      left = node;
    } else {
      // Binary subtraction - parse primary and continue
      left = parse_primary(p);
      if (!left)
        return NULL;
    }
  } else {
    // Parse primary expression (values, list literals, postfix operations)
    left = parse_primary(p);
    if (!left)
      return NULL;
  }

  // While there's an operator with precedence >= min_prec
  while (1) {
    // Peek at the next token to check if it's a binary operator
    Token *tok = peek(p, 0);
    if (!tok)
      break;

    // Check if it's a binary operator and get its precedence
    int prec = 0;
    BinOp op;
    bool is_binary_op = false;
    size_t comparison_consumed = 0;

    switch (tok->type) {
    case TOK_OR:
      prec = get_precedence(TOK_OR);
      op = BINOP_OR;
      is_binary_op = true;
      break;
    case TOK_AND:
      prec = get_precedence(TOK_AND);
      op = BINOP_AND;
      is_binary_op = true;
      break;
    case TOK_PLUS:
      prec = get_precedence(TOK_PLUS);
      op = BINOP_ADD;
      is_binary_op = true;
      break;
    case TOK_MINUS:
      prec = get_precedence(TOK_MINUS);
      op = BINOP_SUB;
      is_binary_op = true;
      break;
    case TOK_TIMES:
      prec = get_precedence(TOK_TIMES);
      op = BINOP_MUL;
      is_binary_op = true;
      break;
    case TOK_DIVIDED: {
      Token *next = peek(p, 1);
      if (next && next->type == TOK_BY) {
        prec = get_precedence(TOK_DIVIDED);
        op = BINOP_DIV;
        is_binary_op = true;
      }
      break;
    }
    case TOK_MOD:
      prec = get_precedence(TOK_MOD);
      op = BINOP_MOD;
      is_binary_op = true;
      break;
    case TOK_IS: {
      size_t consume_count = 0;
      if (match_comparison_operator(p, &op, &consume_count)) {
        prec = 3; // Comparison precedence (between AND and arithmetic)
        is_binary_op = true;
        comparison_consumed = consume_count;
      }
      break;
    }
    default:
      break;
    }

    // If not a binary operator or precedence too low, stop
    if (!is_binary_op || prec < min_prec) {
      break;
    }

    // Now consume the operator(s)
    if (tok->type == TOK_DIVIDED) {
      consume_any(p); // consume DIVIDED
      consume_any(p); // consume BY
    } else if (tok->type == TOK_IS) {
      match_comparison_operator(p, NULL, &comparison_consumed);
      for (size_t i = 0; i < comparison_consumed; i++) {
        consume_any(p);
      }
    } else {
      consume_any(p); // consume the operator
    }

    // Parse right operand with precedence + 1 (for left-associativity)
    ASTNode *right = parse_expression_prec(p, prec + 1);
    if (!right) {
      ast_node_free(left);
      return NULL;
    }

    // Build AST node
    ASTNode *node = ast_node_new_checked(AST_BINOP);
    node->as.binop.left = left;
    node->as.binop.op = op;
    node->as.binop.right = right;
    left = node;
  }

  return left;
}

/**
 * @brief Parse an expression (entry point)
 *
 * Starts precedence-climbing parsing with minimum precedence.
 *
 * @param p Parser state
 * @return AST node for the expression, or NULL on error
 */
static ASTNode *parse_expression(Parser *p) {
  return parse_expression_prec(p, 1); // Start with minimum precedence
}

/**
 * @brief Parse a condition for if/while statements
 *
 * Conditions are full expressions (can include comparisons and logical operators).
 *
 * @param p Parser state
 * @return AST node for the condition, or NULL on error
 */
static ASTNode *parse_condition(Parser *p) {
  // Parse as a full expression - the expression parser now handles comparisons
  // with "is"
  return parse_expression(p);
}

/**
 * @brief Parse a variable assignment statement
 *
 * Handles: "set x to 10" (immutable) or "let x to 10" (mutable).
 * Supports optional type annotations: "set x to 10 as number".
 *
 * @param p Parser state
 * @param indent Indentation level of this statement
 * @return AST node for the assignment, or NULL on error
 */
static ASTNode *parse_assignment(Parser *p, int indent) {
  Token *first = peek(p, 0);
  bool is_mutable = (first->type == TOK_LET);

  // Consume 'set' or 'let'
  if (first->type == TOK_SET) {
    consume(p, TOK_SET);
  } else if (first->type == TOK_LET) {
    consume(p, TOK_LET);
  } else {
    return NULL;
  }

  Token *name = consume(p, TOK_NAME);
  if (!name)
    return NULL;

  if (!consume(p, TOK_TO))
    return NULL;

  ASTNode *value = parse_expression(p);
  if (!value)
    return NULL;

  // Optional type annotation: as <type>
  char *type_name = NULL;
  Token *next = peek(p, 0);
  if (next && next->type == TOK_AS) {
    consume(p, TOK_AS);
    Token *type_tok = consume(p, TOK_NAME);
    if (!type_tok) {
      ast_node_free(value);
      return NULL;
    }
    type_name = strdup(type_tok->text);
    if (!type_name) {
      fprintf(stderr,
              "Memory allocation failed for assignment type annotation\n");
      ast_node_free(value);
      return NULL;
    }
  }

  if (!consume(p, TOK_NEWLINE)) {
    ast_node_free(value);
    free(type_name);
    return NULL;
  }

  ASTNode *node = ast_node_new_checked(AST_ASSIGN);
  node->indent = indent;
  node->as.assign.name = strdup(name->text);
  if (!node->as.assign.name) {
    ast_node_free(value);
    free(type_name);
    free(node);
    return NULL;
  }
  node->as.assign.value = value;
  node->as.assign.is_mutable = is_mutable;
  node->as.assign.type_name = type_name;

  return node;
}

// Parse print
static ASTNode *parse_print(Parser *p, int indent) {
  consume(p, TOK_PRINT);

  ASTNode *value = parse_expression(p);
  if (!value)
    return NULL;

  if (!consume(p, TOK_NEWLINE)) {
    ast_node_free(value);
    return NULL;
  }

  ASTNode *node = ast_node_new_checked(AST_PRINT);
  node->indent = indent;
  node->as.print.value = value;

  return node;
}

/**
 * @brief Parse a block of indented statements
 *
 * Collects all statements with indentation greater than parent_indent.
 * Used for if/for/while bodies and function bodies.
 *
 * @param p Parser state
 * @param parent_indent Indentation level of the parent statement
 * @param block_size Output parameter for number of statements in block
 * @return Array of AST nodes, or NULL on error
 */
static ASTNode **parse_block(Parser *p, int parent_indent, size_t *block_size) {
  if (block_size)
    *block_size = 0;

  size_t capacity = 8;
  size_t count = 0;
  ASTNode **block = malloc(sizeof(ASTNode *) * capacity);
  if (!block) {
    fprintf(stderr, "Parser failed to allocate block statements\n");
    return NULL;
  }

  while (p->pos < p->tokens->count) {
    Token *tok = peek(p, 0);
    if (!tok || tok->type != TOK_INDENT)
      break;

    int next_indent = tok->indent_level;
    if (next_indent <= parent_indent)
      break;

    // Parse statement (we'll implement a full parse_statement later)
    consume_any(p); // consume INDENT

    tok = peek(p, 0);
    if (!tok)
      break;

    ASTNode *stmt = NULL;
    if (tok->type == TOK_SET || tok->type == TOK_LET) {
      stmt = parse_assignment(p, next_indent);
    } else if (tok->type == TOK_PRINT) {
      stmt = parse_print(p, next_indent);
    } else if (tok->type == TOK_IF) {
      stmt = parse_if(p, next_indent);
    } else if (tok->type == TOK_FOR) {
      stmt = parse_for(p, next_indent);
    } else if (tok->type == TOK_WHILE) {
      stmt = parse_while(p, next_indent);
    } else if (tok->type == TOK_FUNCTION) {
      stmt = parse_function(p, next_indent);
    } else if (tok->type == TOK_CALL) {
      stmt = parse_call(p, next_indent);
    } else if (tok->type == TOK_RETURN) {
      stmt = parse_return(p, next_indent);
    } else if (tok->type == TOK_IMPORT) {
      stmt = parse_import(p, next_indent);
    } else if (tok->type == TOK_BREAK) {
      stmt = parse_break(p, next_indent);
    } else if (tok->type == TOK_CONTINUE) {
      stmt = parse_continue(p, next_indent);
    }

    if (!stmt) {
      // Parsing failed; free previously parsed statements.
      for (size_t i = 0; i < count; i++) {
        ast_node_free(block[i]);
      }
      free(block);
      if (block_size)
        *block_size = 0;
      return NULL;
    }

    if (count >= capacity) {
      capacity *= 2;
      ASTNode **new_block = realloc(block, sizeof(ASTNode *) * capacity);
      if (!new_block) {
        fprintf(stderr, "Parser failed to grow block statements\n");
        for (size_t i = 0; i < count; i++) {
          ast_node_free(block[i]);
        }
        free(block);
        if (block_size)
          *block_size = 0;
        return NULL;
      }
      block = new_block;
    }
    block[count++] = stmt;
  }

  if (block_size)
    *block_size = count;
  return block;
}

// Parse if statement
static ASTNode *parse_if(Parser *p, int indent) {
  consume(p, TOK_IF);

  ASTNode *condition = parse_condition(p);
  if (!condition)
    return NULL;

  if (!consume(p, TOK_COLON)) {
    ast_node_free(condition);
    return NULL;
  }

  if (!consume(p, TOK_NEWLINE)) {
    ast_node_free(condition);
    return NULL;
  }

  size_t block_size = 0;
  ASTNode **block = parse_block(p, indent, &block_size);
  if (!block) {
    ast_node_free(condition);
    return NULL;
  }

  ASTNode *node = ast_node_new_checked(AST_IF);
  node->indent = indent;
  node->as.if_stmt.condition = condition;
  node->as.if_stmt.block = block;
  node->as.if_stmt.block_size = block_size;
  node->as.if_stmt.else_if_conditions = NULL;
  node->as.if_stmt.else_if_blocks = NULL;
  node->as.if_stmt.else_if_block_sizes = NULL;
  node->as.if_stmt.else_if_count = 0;
  node->as.if_stmt.else_block = NULL;
  node->as.if_stmt.else_block_size = 0;

  // Parse else-if chains
  while (p->pos < p->tokens->count) {
    Token *tok = peek(p, 0);
    if (!tok || tok->type != TOK_INDENT)
      break;

    int next_indent = tok->indent_level;
    if (next_indent != indent)
      break;

    // Check if next token is else
    Token *next_tok = peek(p, 1);
    if (!next_tok)
      break;

    if (next_tok->type == TOK_ELSE) {
      // Check if it's else-if or just else
      Token *after_else = peek(p, 2);
      if (after_else && after_else->type == TOK_IF) {
        // It's else-if
        consume_any(p); // consume INDENT
        consume(p, TOK_ELSE);
        consume(p, TOK_IF);

        ASTNode *else_if_condition = parse_condition(p);
        if (!else_if_condition) {
          ast_node_free(node);
          return NULL;
        }

        if (!consume(p, TOK_COLON)) {
          ast_node_free(else_if_condition);
          ast_node_free(node);
          return NULL;
        }

        if (!consume(p, TOK_NEWLINE)) {
          ast_node_free(else_if_condition);
          ast_node_free(node);
          return NULL;
        }

        size_t else_if_block_size = 0;
        ASTNode **else_if_block = parse_block(p, indent, &else_if_block_size);
        if (!else_if_block) {
          ast_node_free(else_if_condition);
          ast_node_free(node);
          return NULL;
        }

        // Grow arrays
        size_t new_count = node->as.if_stmt.else_if_count + 1;
        ASTNode **new_conditions = realloc(node->as.if_stmt.else_if_conditions,
                                           sizeof(ASTNode *) * new_count);
        ASTNode ***new_blocks = realloc(node->as.if_stmt.else_if_blocks,
                                        sizeof(ASTNode **) * new_count);
        size_t *new_block_sizes = realloc(node->as.if_stmt.else_if_block_sizes,
                                          sizeof(size_t) * new_count);

        if (!new_conditions || !new_blocks || !new_block_sizes) {
          ast_node_free(else_if_condition);
          for (size_t i = 0; i < else_if_block_size; i++) {
            ast_node_free(else_if_block[i]);
          }
          free(else_if_block);
          ast_node_free(node);
          return NULL;
        }

        node->as.if_stmt.else_if_conditions = new_conditions;
        node->as.if_stmt.else_if_blocks = new_blocks;
        node->as.if_stmt.else_if_block_sizes = new_block_sizes;
        node->as.if_stmt.else_if_conditions[new_count - 1] = else_if_condition;
        node->as.if_stmt.else_if_blocks[new_count - 1] = else_if_block;
        node->as.if_stmt.else_if_block_sizes[new_count - 1] = else_if_block_size;
        node->as.if_stmt.else_if_count = new_count;
      } else {
        // It's just else
        consume_any(p); // consume INDENT
        consume(p, TOK_ELSE);

        if (!consume(p, TOK_COLON)) {
          ast_node_free(node);
          return NULL;
        }

        if (!consume(p, TOK_NEWLINE)) {
          ast_node_free(node);
          return NULL;
        }

        size_t else_block_size = 0;
        ASTNode **else_block = parse_block(p, indent, &else_block_size);
        if (!else_block) {
          ast_node_free(node);
          return NULL;
        }

        node->as.if_stmt.else_block = else_block;
        node->as.if_stmt.else_block_size = else_block_size;
        break; // else is always last
      }
    } else {
      break; // Not an else/else-if, we're done
    }
  }

  return node;
}

// Parse for statement
static ASTNode *parse_for(Parser *p, int indent) {
  consume(p, TOK_FOR);

  Token *var = consume(p, TOK_NAME);
  if (!var)
    return NULL;

  if (!consume(p, TOK_IN))
    return NULL;

  Token *next = peek(p, 0);
  if (!next) {
    return NULL;
  }

  ASTNode *iterable = NULL;
  ASTNode *end = NULL;
  ASTNode *step = NULL;
  bool is_range = false;

  if (next->type == TOK_RANGE) {
    // Range iteration: for i in range start to end [by step]
    is_range = true;
    consume_any(p); // consume TOK_RANGE

    ASTNode *start = parse_expression(p);
    if (!start)
      return NULL;

    if (!consume(p, TOK_TO)) {
      ast_node_free(start);
      return NULL;
    }

    end = parse_expression(p);
    if (!end) {
      ast_node_free(start);
      return NULL;
    }
    iterable = start; // For range, iterable is the start value

    // Check for optional "by step" clause
    Token *after_end = peek(p, 0);
    if (after_end && after_end->type == TOK_BY) {
      consume_any(p); // consume TOK_BY
      step = parse_expression(p);
      if (!step) {
        ast_node_free(start);
        ast_node_free(end);
        return NULL;
      }
    }
  } else {
    // List iteration: for item in list_expr
    is_range = false;
    iterable = parse_expression(p);
    if (!iterable)
      return NULL;
  }

  if (!consume(p, TOK_COLON)) {
    ast_node_free(iterable);
    if (end)
      ast_node_free(end);
    if (step)
      ast_node_free(step);
    return NULL;
  }

  if (!consume(p, TOK_NEWLINE)) {
    ast_node_free(iterable);
    if (end)
      ast_node_free(end);
    if (step)
      ast_node_free(step);
    return NULL;
  }

  size_t block_size = 0;
  ASTNode **block = parse_block(p, indent, &block_size);
  if (!block) {
    ast_node_free(iterable);
    if (end)
      ast_node_free(end);
    if (step)
      ast_node_free(step);
    return NULL;
  }

  ASTNode *node = ast_node_new_checked(AST_FOR);
  node->indent = indent;
  node->as.for_stmt.var = strdup(var->text);
  if (!node->as.for_stmt.var) {
    ast_node_free(iterable);
    if (end)
      ast_node_free(end);
    if (step)
      ast_node_free(step);
    // Free block and its statements
    if (block) {
      for (size_t i = 0; i < block_size; i++) {
        ast_node_free(block[i]);
      }
      free(block);
    }
    free(node);
    return NULL;
  }
  node->as.for_stmt.iterable = iterable;
  node->as.for_stmt.is_range = is_range;
  node->as.for_stmt.end = end;
  node->as.for_stmt.step = step; // NULL means step=1
  node->as.for_stmt.block = block;
  node->as.for_stmt.block_size = block_size;

  return node;
}

// Parse while statement
static ASTNode *parse_while(Parser *p, int indent) {
  consume(p, TOK_WHILE);

  ASTNode *condition = parse_condition(p);
  if (!condition)
    return NULL;

  if (!consume(p, TOK_COLON)) {
    ast_node_free(condition);
    return NULL;
  }

  if (!consume(p, TOK_NEWLINE)) {
    ast_node_free(condition);
    return NULL;
  }

  size_t block_size = 0;
  ASTNode **block = parse_block(p, indent, &block_size);
  if (!block) {
    ast_node_free(condition);
    return NULL;
  }

  ASTNode *node = ast_node_new_checked(AST_WHILE);
  node->indent = indent;
  node->as.while_stmt.condition = condition;
  node->as.while_stmt.block = block;
  node->as.while_stmt.block_size = block_size;

  return node;
}

// Parse function definition
static ASTNode *parse_function(Parser *p, int indent) {
  consume(p, TOK_FUNCTION);

  Token *name = consume(p, TOK_NAME);
  if (!name)
    return NULL;

  // Parse parameters
  size_t param_capacity = 4;
  size_t param_count = 0;
  char **params = malloc(sizeof(char *) * param_capacity);
  if (!params) {
    fprintf(stderr, "parse_function: failed to allocate params array\n");
    return NULL;
  }

  Token *tok = peek(p, 0);
  if (tok && tok->type == TOK_WITH) {
    consume_any(p);

    Token *param = consume(p, TOK_NAME);
    if (!param) {
      free(params);
      return NULL;
    }
    char *param_name = strdup(param->text);
    if (!param_name) {
      free(params);
      return NULL;
    }
    params[param_count++] = param_name;

    while (peek(p, 0) && peek(p, 0)->type == TOK_COMMA) {
      consume_any(p);
      param = consume(p, TOK_NAME);
      if (!param) {
        for (size_t i = 0; i < param_count; i++)
          free(params[i]);
        free(params);
        return NULL;
      }

      if (param_count >= param_capacity) {
        size_t new_capacity = param_capacity * 2;
        char **new_params = realloc(params, sizeof(char *) * new_capacity);
        if (!new_params) {
          fprintf(stderr, "parse_function: failed to grow params array\n");
          for (size_t i = 0; i < param_count; i++)
            free(params[i]);
          free(params);
          return NULL;
        }
        params = new_params;
        param_capacity = new_capacity;
      }
      char *param_name_loop = strdup(param->text);
      if (!param_name_loop) {
        for (size_t i = 0; i < param_count; i++)
          free(params[i]);
        free(params);
        return NULL;
      }
      params[param_count++] = param_name_loop;
    }
  }

  if (!consume(p, TOK_COLON)) {
    for (size_t i = 0; i < param_count; i++)
      free(params[i]);
    free(params);
    return NULL;
  }

  if (!consume(p, TOK_NEWLINE)) {
    for (size_t i = 0; i < param_count; i++)
      free(params[i]);
    free(params);
    return NULL;
  }

  size_t block_size = 0;
  ASTNode **block = parse_block(p, indent, &block_size);
  if (!block) {
    for (size_t i = 0; i < param_count; i++)
      free(params[i]);
    free(params);
    return NULL;
  }

  ASTNode *node = ast_node_new_checked(AST_FUNCTION);
  node->indent = indent;
  node->as.function.name = strdup(name->text);
  if (!node->as.function.name) {
    // Free params
    for (size_t i = 0; i < param_count; i++)
      free(params[i]);
    free(params);
    // Free block and its statements
    if (block) {
      for (size_t i = 0; i < block_size; i++) {
        ast_node_free(block[i]);
      }
      free(block);
    }
    free(node);
    return NULL;
  }
  node->as.function.params = params;
  node->as.function.param_count = param_count;
  node->as.function.block = block;
  node->as.function.block_size = block_size;

  return node;
}

// Parse function call
// If indent >= 0, it's a statement (requires newline)
// If indent < 0, it's an expression (no newline required)
static ASTNode *parse_call(Parser *p, int indent) {
  consume(p, TOK_CALL);

  Token *name = consume(p, TOK_NAME);
  if (!name)
    return NULL;

  // Parse arguments
  size_t arg_capacity = 4;
  size_t arg_count = 0;
  ASTNode **args = malloc(sizeof(ASTNode *) * arg_capacity);
  if (!args) {
    fprintf(stderr, "parse_call: failed to allocate argument array\n");
    return NULL;
  }

  Token *tok = peek(p, 0);
  if (tok && tok->type == TOK_WITH) {
    consume_any(p);

    ASTNode *arg = parse_expression(p);
    if (!arg) {
      free(args);
      return NULL;
    }
    args[arg_count++] = arg;

    while (peek(p, 0) && peek(p, 0)->type == TOK_COMMA) {
      consume_any(p);
      arg = parse_expression(p);
      if (!arg) {
        for (size_t i = 0; i < arg_count; i++)
          ast_node_free(args[i]);
        free(args);
        return NULL;
      }

      if (arg_count >= arg_capacity) {
        size_t new_capacity = arg_capacity * 2;
        ASTNode **new_args = realloc(args, sizeof(ASTNode *) * new_capacity);
        if (!new_args) {
          fprintf(stderr, "parse_call: failed to grow argument array\n");
          for (size_t i = 0; i < arg_count; i++)
            ast_node_free(args[i]);
          free(args);
          return NULL;
        }
        args = new_args;
        arg_capacity = new_capacity;
      }
      args[arg_count++] = arg;
    }
  }

  // Only require newline if it's a statement (indent >= 0)
  if (indent >= 0) {
    if (!consume(p, TOK_NEWLINE)) {
      for (size_t i = 0; i < arg_count; i++)
        ast_node_free(args[i]);
      free(args);
      return NULL;
    }
  }

  ASTNode *node = ast_node_new_checked(AST_CALL);
  node->indent = indent;
  node->as.call.name = strdup(name->text);
  if (!node->as.call.name) {
    // Free args
    for (size_t i = 0; i < arg_count; i++)
      ast_node_free(args[i]);
    free(args);
    free(node);
    return NULL;
  }
  node->as.call.args = args;
  node->as.call.arg_count = arg_count;

  return node;
}

// Parse return statement
static ASTNode *parse_return(Parser *p, int indent) {
  consume(p, TOK_RETURN);

  ASTNode *value = parse_expression(p);
  if (!value)
    return NULL;

  if (!consume(p, TOK_NEWLINE)) {
    ast_node_free(value);
    return NULL;
  }

  ASTNode *node = ast_node_new_checked(AST_RETURN);
  node->indent = indent;
  node->as.return_stmt.value = value;

  return node;
}

// Parse import statement
// Supports:
//   import module_name
//   import module_name from "file.kr"
//   from module_name import func1, func2
static ASTNode *parse_import(Parser *p, int indent) {
  Token *first = peek(p, 0);
  bool is_from_import = (first->type == TOK_FROM);

  char *module_name = NULL;
  char *file_path = NULL;
  char **imported_names = NULL;
  size_t imported_count = 0;

  if (is_from_import) {
    // Parse: from module_name import func1, func2
    consume(p, TOK_FROM);

    Token *module_tok = consume(p, TOK_NAME);
    if (!module_tok)
      return NULL;
    module_name = strdup(module_tok->text);
    if (!module_name)
      return NULL;

    if (!consume(p, TOK_IMPORT)) {
      free(module_name);
      return NULL;
    }

    // Parse function names to import
    size_t capacity = 4;
    imported_names = malloc(sizeof(char *) * capacity);
    if (!imported_names) {
      free(module_name);
      return NULL;
    }

    // Parse first function name
    Token *func_tok = consume(p, TOK_NAME);
    if (!func_tok) {
      free(module_name);
      free(imported_names);
      return NULL;
    }
    imported_names[imported_count++] = strdup(func_tok->text);
    if (!imported_names[imported_count - 1]) {
      free(module_name);
      free(imported_names);
      return NULL;
    }

    // Parse remaining function names (comma-separated)
    while (peek(p, 0) && peek(p, 0)->type == TOK_COMMA) {
      consume_any(p); // consume comma

      if (imported_count == capacity) {
        size_t new_capacity = capacity * 2;
        char **new_names = realloc(imported_names, sizeof(char *) * new_capacity);
        if (!new_names) {
          // Cleanup
          for (size_t i = 0; i < imported_count; i++) {
            free(imported_names[i]);
          }
          free(module_name);
          free(imported_names);
          return NULL;
        }
        imported_names = new_names;
        capacity = new_capacity;
      }

      func_tok = consume(p, TOK_NAME);
      if (!func_tok) {
        // Cleanup
        for (size_t i = 0; i < imported_count; i++) {
          free(imported_names[i]);
        }
        free(module_name);
        free(imported_names);
        return NULL;
      }
      imported_names[imported_count++] = strdup(func_tok->text);
      if (!imported_names[imported_count - 1]) {
        // Cleanup
        for (size_t i = 0; i < imported_count - 1; i++) {
          free(imported_names[i]);
        }
        free(module_name);
        free(imported_names);
        return NULL;
      }
    }
  } else {
    // Parse: import module_name [from "file.kr"]
  consume(p, TOK_IMPORT);

    Token *module_tok = consume(p, TOK_NAME);
    if (!module_tok)
      return NULL;
    module_name = strdup(module_tok->text);
  if (!module_name)
    return NULL;

    // Check for "from" keyword
    Token *next = peek(p, 0);
    if (next && next->type == TOK_FROM) {
      consume(p, TOK_FROM);

      // Expect string literal for file path
      Token *file_tok = consume(p, TOK_STRING);
      if (!file_tok) {
        free(module_name);
        return NULL;
      }
      file_path = strdup(file_tok->text);
      if (!file_path) {
        free(module_name);
        return NULL;
      }
    }
  }

  if (!consume(p, TOK_NEWLINE)) {
    free(module_name);
    free(file_path);
    if (imported_names) {
      for (size_t i = 0; i < imported_count; i++) {
        free(imported_names[i]);
      }
      free(imported_names);
    }
    return NULL;
  }

  ASTNode *node = ast_node_new_checked(AST_IMPORT);
  node->indent = indent;
  node->as.import.module_name = module_name;
  node->as.import.file_path = file_path;
  node->as.import.imported_names = imported_names;
  node->as.import.imported_count = imported_count;
  node->as.import.is_from_import = is_from_import;

  return node;
}

// Parse break statement
static ASTNode *parse_break(Parser *p, int indent) {
  consume(p, TOK_BREAK);

  if (!consume(p, TOK_NEWLINE)) {
    return NULL;
  }

  ASTNode *node = ast_node_new_checked(AST_BREAK);
  node->indent = indent;

  return node;
}

// Parse continue statement
static ASTNode *parse_continue(Parser *p, int indent) {
  consume(p, TOK_CONTINUE);

  if (!consume(p, TOK_NEWLINE)) {
    return NULL;
  }

  ASTNode *node = ast_node_new_checked(AST_CONTINUE);
  node->indent = indent;

  return node;
}

// Parse statement
static ASTNode *parse_statement(Parser *p) {
  Token *tok = peek(p, 0);
  if (!tok)
    return NULL;

  int indent = 0;
  if (tok->type == TOK_INDENT) {
    indent = tok->indent_level;
    consume_any(p);
    tok = peek(p, 0);
  }

  if (!tok)
    return NULL;

  switch (tok->type) {
  case TOK_SET:
  case TOK_LET:
    return parse_assignment(p, indent);
  case TOK_PRINT:
    return parse_print(p, indent);
  case TOK_IF:
    return parse_if(p, indent);
  case TOK_FOR:
    return parse_for(p, indent);
  case TOK_WHILE:
    return parse_while(p, indent);
  case TOK_FUNCTION:
    return parse_function(p, indent);
  case TOK_CALL:
    return parse_call(p, indent);
  case TOK_RETURN:
    return parse_return(p, indent);
  case TOK_IMPORT:
    return parse_import(p, indent);
  case TOK_BREAK:
    return parse_break(p, indent);
  case TOK_CONTINUE:
    return parse_continue(p, indent);
  default:
    return NULL;
  }
}

/**
 * @brief Parse tokens into an Abstract Syntax Tree
 *
 * Main entry point for parsing. Processes all statements in the token stream
 * and builds a complete AST. Handles errors gracefully by skipping to the
 * next line on parse failures.
 *
 * @param tokens Token array to parse
 * @return AST containing all statements, or NULL on critical error
 */
AST *parse(TokenArray *tokens) {
  Parser p = {tokens, 0};

  AST *ast = malloc(sizeof(AST));
  if (!ast)
    return NULL;

  ast->capacity = 16;
  ast->count = 0;
  ast->statements = malloc(sizeof(ASTNode *) * ast->capacity);
  if (!ast->statements) {
    free(ast);
    return NULL;
  }

  while (p.pos < tokens->count) {
    Token *tok = peek(&p, 0);
    if (!tok || tok->type == TOK_EOF)
      break;

    if (tok->type == TOK_NEWLINE) {
      consume_any(&p);
      continue;
    }

    ASTNode *stmt = parse_statement(&p);
    if (stmt) {
      if (ast->count >= ast->capacity) {
        ast->capacity *= 2;
        ast->statements =
            realloc(ast->statements, sizeof(ASTNode *) * ast->capacity);
      }
      ast->statements[ast->count++] = stmt;
    } else {
      // Skip to next line on error
      while (tok && tok->type != TOK_NEWLINE && tok->type != TOK_EOF) {
        consume_any(&p);
        tok = peek(&p, 0);
      }
    }
  }

  return ast;
}

/**
 * @brief Free an AST node and all its children
 *
 * Recursively frees all memory associated with the node, including
 * strings, arrays, and child nodes.
 *
 * @param node Node to free (safe to pass NULL)
 */
void ast_node_free(ASTNode *node) {
  if (!node)
    return;

  switch (node->type) {
  case AST_STRING:
    free(node->as.string.value);
    break;
  case AST_BOOL:
  case AST_NULL:
    // No allocated memory
    break;
  case AST_VAR:
    free(node->as.var_name);
    break;
  case AST_ASSIGN:
    free(node->as.assign.name);
    ast_node_free(node->as.assign.value);
    free(node->as.assign.type_name);
    break;
  case AST_PRINT:
    ast_node_free(node->as.print.value);
    break;
  case AST_BINOP:
    ast_node_free(node->as.binop.left);
    ast_node_free(node->as.binop.right);
    break;
  case AST_IF:
    ast_node_free(node->as.if_stmt.condition);
    for (size_t i = 0; i < node->as.if_stmt.block_size; i++) {
      ast_node_free(node->as.if_stmt.block[i]);
    }
    free(node->as.if_stmt.block);
    // Free else-if chains
    for (size_t i = 0; i < node->as.if_stmt.else_if_count; i++) {
      ast_node_free(node->as.if_stmt.else_if_conditions[i]);
      for (size_t j = 0; j < node->as.if_stmt.else_if_block_sizes[i]; j++) {
        ast_node_free(node->as.if_stmt.else_if_blocks[i][j]);
      }
      free(node->as.if_stmt.else_if_blocks[i]);
    }
    free(node->as.if_stmt.else_if_conditions);
    free(node->as.if_stmt.else_if_blocks);
    free(node->as.if_stmt.else_if_block_sizes);
    // Free else block
    if (node->as.if_stmt.else_block) {
      for (size_t i = 0; i < node->as.if_stmt.else_block_size; i++) {
        ast_node_free(node->as.if_stmt.else_block[i]);
      }
      free(node->as.if_stmt.else_block);
    }
    break;
  case AST_BREAK:
  case AST_CONTINUE:
    // No allocated memory
    break;
  case AST_FOR:
    free(node->as.for_stmt.var);
    ast_node_free(node->as.for_stmt.iterable);
    if (node->as.for_stmt.end)
      ast_node_free(node->as.for_stmt.end);
    if (node->as.for_stmt.step)
      ast_node_free(node->as.for_stmt.step);
    for (size_t i = 0; i < node->as.for_stmt.block_size; i++) {
      ast_node_free(node->as.for_stmt.block[i]);
    }
    free(node->as.for_stmt.block);
    break;
  case AST_WHILE:
    ast_node_free(node->as.while_stmt.condition);
    for (size_t i = 0; i < node->as.while_stmt.block_size; i++) {
      ast_node_free(node->as.while_stmt.block[i]);
    }
    free(node->as.while_stmt.block);
    break;
  case AST_FUNCTION:
    free(node->as.function.name);
    for (size_t i = 0; i < node->as.function.param_count; i++) {
      free(node->as.function.params[i]);
    }
    free(node->as.function.params);
    for (size_t i = 0; i < node->as.function.block_size; i++) {
      ast_node_free(node->as.function.block[i]);
    }
    free(node->as.function.block);
    break;
  case AST_CALL:
    free(node->as.call.name);
    for (size_t i = 0; i < node->as.call.arg_count; i++) {
      ast_node_free(node->as.call.args[i]);
    }
    free(node->as.call.args);
    break;
  case AST_RETURN:
    ast_node_free(node->as.return_stmt.value);
    break;
  case AST_IMPORT:
    free(node->as.import.module_name);
    free(node->as.import.file_path);
    if (node->as.import.imported_names) {
      for (size_t i = 0; i < node->as.import.imported_count; i++) {
        free(node->as.import.imported_names[i]);
      }
      free(node->as.import.imported_names);
    }
    break;
  case AST_LIST:
    for (size_t i = 0; i < node->as.list.element_count; i++) {
      ast_node_free(node->as.list.elements[i]);
    }
    free(node->as.list.elements);
    break;
  case AST_RANGE:
    ast_node_free(node->as.range.start);
    ast_node_free(node->as.range.end);
    if (node->as.range.step) {
      ast_node_free(node->as.range.step);
    }
    break;
  case AST_MAP:
    for (size_t i = 0; i < node->as.map.entry_count; i++) {
      ast_node_free(node->as.map.keys[i]);
      ast_node_free(node->as.map.values[i]);
    }
    free(node->as.map.keys);
    free(node->as.map.values);
    break;
  case AST_INDEX:
    ast_node_free(node->as.index.list_expr);
    ast_node_free(node->as.index.index);
    break;
  case AST_SLICE:
    ast_node_free(node->as.slice.list_expr);
    ast_node_free(node->as.slice.start);
    ast_node_free(node->as.slice.end);
    break;
  case AST_FSTRING:
    for (size_t i = 0; i < node->as.fstring.part_count; i++) {
      ast_node_free(node->as.fstring.parts[i]);
    }
    free(node->as.fstring.parts);
    break;
  default:
    break;
  }

  free(node);
}

/**
 * @brief Free an AST and all its statements
 *
 * @param ast AST to free (safe to pass NULL)
 */
void ast_free(AST *ast) {
  if (!ast)
    return;

  for (size_t i = 0; i < ast->count; i++) {
    ast_node_free(ast->statements[i]);
  }
  free(ast->statements);
  free(ast);
}

// Debug print AST
void ast_print(AST *ast) {
  if (!ast)
    return;

  printf("AST with %zu statements\n", ast->count);
  for (size_t i = 0; i < ast->count; i++) {
    printf("Statement %zu: type=%d\n", i, ast->statements[i]->type);
  }
}
