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
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Maximum recursion depth to prevent stack exhaustion
#define MAX_RECURSION_DEPTH 512

// Initial capacity for dynamically growing arrays
#define INITIAL_ARRAY_CAPACITY 4

/**
 * @brief Generic function to grow a pointer array
 *
 * Doubles the capacity of an array and reallocates it. Updates both the array
 * pointer and capacity variable. Returns false on allocation failure.
 *
 * @param arr Pointer to the array pointer (void** to work with any pointer
 * type)
 * @param count Current count (used for bounds checking)
 * @param capacity Pointer to capacity variable
 * @param element_size Size of each element in bytes
 * @return true on success, false on allocation failure
 */
static bool grow_array(void **arr, size_t count, size_t *capacity,
                       size_t element_size) {
  if (count < *capacity) {
    return true; // No need to grow
  }

  size_t new_capacity = *capacity * 2;
  void *new_arr = realloc(*arr, element_size * new_capacity);
  if (!new_arr) {
    return false;
  }

  *arr = new_arr;
  *capacity = new_capacity;
  return true;
}

/**
 * Parser state structure
 * Tracks current position in the token stream and recursion depth
 */
typedef struct {
  TokenArray *tokens;     /**< Array of tokens to parse */
  size_t pos;             /**< Current position in token array */
  size_t recursion_depth; /**< Current recursion depth for stack overflow
                             protection */
  ParseError **error_out; /**< Optional pointer to error output (for structured
                             errors) */
} Parser;

// Forward declaration
static void parser_set_error(Parser *p, const char *message);

/**
 * @brief Convert token type to human-readable string
 *
 * @param type Token type enum value
 * @return Human-readable string name for the token type
 */
static const char *token_type_name(TokenType type) {
  switch (type) {
  case TOK_NUMBER:
    return "NUMBER";
  case TOK_STRING:
    return "STRING";
  case TOK_FSTRING:
    return "FSTRING";
  case TOK_SET:
    return "SET";
  case TOK_LET:
    return "LET";
  case TOK_TO:
    return "TO";
  case TOK_AS:
    return "AS";
  case TOK_IF:
    return "IF";
  case TOK_ELSE:
    return "ELSE";
  case TOK_ELSE_IF:
    return "ELSE_IF";
  case TOK_FOR:
    return "FOR";
  case TOK_WHILE:
    return "WHILE";
  case TOK_BREAK:
    return "BREAK";
  case TOK_CONTINUE:
    return "CONTINUE";
  case TOK_IN:
    return "IN";
  case TOK_RANGE:
    return "RANGE";
  case TOK_LIST:
    return "LIST";
  case TOK_MAP:
    return "MAP";
  case TOK_AT:
    return "AT";
  case TOK_FROM:
    return "FROM";
  case TOK_END:
    return "END";
  case TOK_FUNCTION:
    return "FUNCTION";
  case TOK_WITH:
    return "WITH";
  case TOK_CALL:
    return "CALL";
  case TOK_RETURN:
    return "RETURN";
  case TOK_IMPORT:
    return "IMPORT";
  case TOK_TRUE:
    return "TRUE";
  case TOK_FALSE:
    return "FALSE";
  case TOK_NULL:
    return "NULL";
  case TOK_UNDEFINED:
    return "UNDEFINED";
  case TOK_IS:
    return "IS";
  case TOK_EQUAL:
    return "EQUAL";
  case TOK_NOT:
    return "NOT";
  case TOK_GREATER:
    return "GREATER";
  case TOK_LESS:
    return "LESS";
  case TOK_THAN:
    return "THAN";
  case TOK_AND:
    return "AND";
  case TOK_OR:
    return "OR";
  case TOK_PRINT:
    return "PRINT";
  case TOK_PLUS:
    return "PLUS";
  case TOK_MINUS:
    return "MINUS";
  case TOK_TIMES:
    return "TIMES";
  case TOK_DIVIDED:
    return "DIVIDED";
  case TOK_BY:
    return "BY";
  case TOK_MOD:
    return "MOD";
  case TOK_DELETE:
    return "DELETE";
  case TOK_TRY:
    return "TRY";
  case TOK_CATCH:
    return "CATCH";
  case TOK_FINALLY:
    return "FINALLY";
  case TOK_RAISE:
    return "RAISE";
  case TOK_NAME:
    return "NAME";
  case TOK_COLON:
    return "COLON";
  case TOK_COMMA:
    return "COMMA";
  case TOK_NEWLINE:
    return "NEWLINE";
  case TOK_INDENT:
    return "INDENT";
  case TOK_EOF:
    return "EOF";
  default:
    return "UNKNOWN";
  }
}

/**
 * @brief Check and increment recursion depth
 *
 * @param p Parser state
 * @return true if depth is acceptable, false if max depth exceeded
 */
static bool check_recursion_depth(Parser *p) {
  if (p->recursion_depth >= MAX_RECURSION_DEPTH) {
    char msg[128];
    snprintf(msg, sizeof(msg), "Maximum recursion depth (%d) exceeded",
             MAX_RECURSION_DEPTH);
    parser_set_error(p, msg);
    return false;
  }
  p->recursion_depth++;
  return true;
}

/**
 * @brief Decrement recursion depth
 *
 * @param p Parser state
 */
static void decrement_recursion_depth(Parser *p) {
  if (p->recursion_depth > 0) {
    p->recursion_depth--;
  }
}

/**
 * @brief Report a parse error
 *
 * Sets the error in the output parameter if provided, otherwise falls back
 * to stderr for backward compatibility.
 *
 * @param p Parser state
 * @param message Error message (will be copied)
 */
static void parser_set_error(Parser *p, const char *message) {
  if (!p || !message)
    return;

  // If error output is provided, use structured error reporting
  if (p->error_out && *p->error_out == NULL) {
    ParseError *err = malloc(sizeof(ParseError));
    if (err) {
      err->message = strdup(message);
      if (!err->message) {
        // strdup failed - free the error structure and fall through to stderr
        free(err);
      } else {
        err->line = 0;   // TODO: Track line numbers from tokens
        err->column = 0; // TODO: Track column numbers from tokens
        *p->error_out = err;
        return;
      }
    }
  }

  // Fall back to stderr for backward compatibility
  fprintf(stderr, "%s\n", message);
}

/**
 * @brief Free a ParseError structure
 *
 * @param err ParseError to free (may be NULL)
 */
void parse_error_free(ParseError *err) {
  if (!err)
    return;
  free(err->message);
  free(err);
}

/**
 * @brief Look ahead at a token without consuming it
 *
 * @param p Parser state
 * @param offset How many tokens ahead to look (0 = current token, negative for
 * looking back)
 * @return Token pointer, or NULL if out of bounds
 */
static Token *peek(Parser *p, int offset) {
  // Handle negative offsets to prevent unsigned underflow
  if (offset < 0) {
    // For negative offsets, check if p->pos is large enough
    size_t abs_offset = (size_t)(-offset);
    if (p->pos < abs_offset) {
      // Would underflow - return NULL
      return NULL;
    }
    size_t idx = p->pos - abs_offset;
    if (idx >= p->tokens->count)
      return NULL;
    return &p->tokens->tokens[idx];
  }

  // Positive offset - safe to add
  size_t idx = p->pos + (size_t)offset;
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
    parser_set_error(p, "Unexpected end of input");
    return NULL;
  }

  Token *tok = &p->tokens->tokens[p->pos];
  if (tok->type != expected) {
    char msg[128];
    snprintf(msg, sizeof(msg), "Expected token type %s, got %s",
             token_type_name(expected), token_type_name(tok->type));
    parser_set_error(p, msg);
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
static ASTNode *parse_delete(Parser *p, int indent);
static ASTNode *parse_try(Parser *p, int indent);
static ASTNode *parse_raise(Parser *p, int indent);

// Helper functions for parse_try
static bool try_parse_catch_block(Parser *p, int indent, ASTNode *try_node,
                                  size_t *catch_capacity);
static bool try_parse_finally_block(Parser *p, int indent, ASTNode *try_node);

// Helper functions for parse_if
static bool if_parse_else_if(Parser *p, int indent, ASTNode *if_node);
static bool if_parse_else(Parser *p, int indent, ASTNode *if_node);

// Helper functions for parse_for
static bool for_parse_range_iteration(Parser *p, ASTNode **iterable,
                                      ASTNode **end, ASTNode **step);
static void for_cleanup_resources(ASTNode *iterable, ASTNode *end,
                                  ASTNode *step, ASTNode **block,
                                  size_t block_size);
static ASTNode *parse_list_literal(Parser *p);
static ASTNode *parse_range_literal(Parser *p);
static ASTNode *parse_map_literal(Parser *p);

// Helper functions for parse_map_literal
static ASTNode *map_parse_key(Parser *p);
static bool map_grow_entries(ASTNode ***keys, ASTNode ***values,
                             size_t *capacity);
static void map_cleanup_entries(ASTNode **keys, ASTNode **values,
                                size_t entry_count);

// Helper functions for parse_list_literal
static bool list_grow_elements(ASTNode ***elements, size_t *capacity);
static void list_cleanup_elements(ASTNode **elements, size_t element_count);

// Helper functions for parse_assignment
static ASTNode *assignment_parse_index(Parser *p, int indent, Token *name);
static ASTNode *assignment_parse_regular(Parser *p, int indent, Token *name,
                                         bool is_mutable);

// Helper functions for parse_function
static bool function_parse_parameters(Parser *p, char ***params,
                                      size_t *param_count,
                                      size_t *param_capacity);
static void function_cleanup_parameters(char **params, size_t param_count);

// Helper functions for parse_call
static bool call_parse_arguments(Parser *p, ASTNode ***args, size_t *arg_count,
                                 size_t *arg_capacity);
static void call_cleanup_arguments(ASTNode **args, size_t arg_count);
static ASTNode *parse_primary(Parser *p);
static ASTNode *parse_fstring(Parser *p);

// Helper functions for parse_fstring
static ASTNode *fstring_create_string_part(const char *content, size_t start,
                                           size_t end);
static ASTNode *fstring_parse_expression(const char *content, size_t expr_start,
                                         size_t expr_end);
static size_t fstring_find_next_brace(const char *content, size_t content_len,
                                      size_t start);
static size_t fstring_find_matching_brace(const char *content,
                                          size_t content_len, size_t start);
static void fstring_cleanup_parts(ASTNode **parts, size_t part_count);
static bool fstring_grow_parts_array(ASTNode ***parts, size_t *capacity);
static bool fstring_add_part(ASTNode ***parts, size_t *part_count,
                             size_t *capacity, ASTNode *part);

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
    fprintf(stderr, "Failed to allocate AST node (type=%d)\n", type);
    return NULL;
  }
  return node;
}

// Helper function for parse_fstring: create a string literal AST node from
// content
static ASTNode *fstring_create_string_part(const char *content, size_t start,
                                           size_t end) {
  size_t str_len = end - start;
  ASTNode *str_node = ast_node_new_checked(AST_STRING);
  if (!str_node)
    return NULL;

  str_node->as.string.value = malloc(str_len + 1);
  if (!str_node->as.string.value) {
    free(str_node);
    return NULL;
  }
  memcpy(str_node->as.string.value, content + start, str_len);
  str_node->as.string.value[str_len] = '\0';
  str_node->as.string.length = str_len;
  return str_node;
}

// Helper function for parse_fstring: parse expression from f-string content
static ASTNode *fstring_parse_expression(const char *content, size_t expr_start,
                                         size_t expr_end) {
  // Extract expression string
  size_t expr_len = expr_end - expr_start;
  char *expr_str = malloc(expr_len + 1);
  if (!expr_str)
    return NULL;

  memcpy(expr_str, content + expr_start, expr_len);
  expr_str[expr_len] = '\0';

  // Tokenize and parse the expression
  TokenArray *expr_tokens = tokenize(expr_str, NULL);
  free(expr_str);
  if (!expr_tokens)
    return NULL;

  // Create a temporary parser for the expression
  Parser expr_parser = {expr_tokens, 0, 0, NULL};

  // Skip INDENT token if present (tokenizer adds it for each line)
  if (expr_parser.pos < expr_tokens->count &&
      expr_tokens->tokens[expr_parser.pos].type == TOK_INDENT) {
    expr_parser.pos++;
  }

  ASTNode *expr_node = parse_expression(&expr_parser);
  token_array_free(expr_tokens);
  return expr_node;
}

/**
 * @brief Find the next opening brace in f-string content
 *
 * Skips escaped characters. Returns content_len if no brace is found.
 *
 * @param content F-string content
 * @param content_len Length of content
 * @param start Starting position to search from
 * @return Position of opening brace, or content_len if not found
 */
static size_t fstring_find_next_brace(const char *content, size_t content_len,
                                      size_t start) {
  size_t i = start;
  while (i < content_len) {
    if (content[i] == '\\' && i + 1 < content_len) {
      i += 2; // Skip escaped character
    } else if (content[i] == '{') {
      return i;
    } else {
      i++;
    }
  }
  return content_len;
}

/**
 * @brief Find the matching closing brace for an opening brace
 *
 * Handles nested braces by tracking depth. Returns content_len if no matching
 * brace is found.
 *
 * @param content F-string content
 * @param content_len Length of content
 * @param start Position after the opening brace
 * @return Position of matching closing brace, or content_len if not found
 */
static size_t fstring_find_matching_brace(const char *content,
                                          size_t content_len, size_t start) {
  size_t i = start;
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
        return i;
      }
      i++;
    } else {
      i++;
    }
  }
  return content_len; // No matching brace found
}

/**
 * @brief Cleanup allocated parts array
 *
 * Frees all AST nodes in the parts array and the array itself.
 *
 * @param parts Array of AST nodes
 * @param part_count Number of parts in the array
 */
static void fstring_cleanup_parts(ASTNode **parts, size_t part_count) {
  if (!parts)
    return;
  for (size_t i = 0; i < part_count; i++) {
    ast_node_free(parts[i]);
  }
  free(parts);
}

/**
 * @brief Grow the parts array if needed
 *
 * Doubles the capacity of the parts array. On failure, returns false.
 *
 * @param parts Pointer to parts array pointer
 * @param capacity Pointer to current capacity
 * @return true on success, false on allocation failure
 */
static bool fstring_grow_parts_array(ASTNode ***parts, size_t *capacity) {
  // Use a dummy count that's >= capacity to force growth
  return grow_array((void **)parts, *capacity, capacity, sizeof(ASTNode *));
}

/**
 * @brief Add a part to the parts array, growing if necessary
 *
 * Adds a part to the array, automatically growing the array if needed.
 * On failure, frees the part and returns false.
 *
 * @param parts Pointer to parts array pointer
 * @param part_count Pointer to current part count
 * @param capacity Pointer to current capacity
 * @param part Part to add (will be freed on failure)
 * @return true on success, false on allocation failure
 */
static bool fstring_add_part(ASTNode ***parts, size_t *part_count,
                             size_t *capacity, ASTNode *part) {
  if (*part_count >= *capacity) {
    if (!fstring_grow_parts_array(parts, capacity)) {
      ast_node_free(part);
      return false;
    }
  }
  (*parts)[(*part_count)++] = part;
  return true;
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
    if (!node)
      return NULL;

    // Use strtod() with proper error checking instead of atof()
    errno = 0; // Clear errno before conversion
    char *endptr;
    double value = strtod(tok->text, &endptr);

    // Check for conversion errors
    if (errno == ERANGE) {
      // Overflow or underflow occurred
      if (value == HUGE_VAL || value == -HUGE_VAL) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Number overflow: %s", tok->text);
        parser_set_error(p, msg);
        free(node);
        return NULL;
      }
      // Underflow (value is 0.0 or very small) - acceptable, continue
    }

    // Check if the entire string was consumed
    // endptr should point to the null terminator if conversion succeeded
    if (endptr == tok->text || *endptr != '\0') {
      char msg[256];
      snprintf(msg, sizeof(msg), "Invalid number format: %s", tok->text);
      parser_set_error(p, msg);
      free(node);
      return NULL;
    }

    node->as.number = value;
    return node;
  }

  if (tok->type == TOK_TRUE) {
    consume_any(p);
    ASTNode *node = ast_node_new_checked(AST_BOOL);
    if (!node)
      return NULL;
    node->as.boolean = true;
    return node;
  }

  if (tok->type == TOK_FALSE) {
    consume_any(p);
    ASTNode *node = ast_node_new_checked(AST_BOOL);
    if (!node)
      return NULL;
    node->as.boolean = false;
    return node;
  }

  if (tok->type == TOK_NULL || tok->type == TOK_UNDEFINED) {
    consume_any(p);
    ASTNode *node = ast_node_new_checked(AST_NULL);
    if (!node)
      return NULL;
    return node;
  }

  if (tok->type == TOK_STRING) {
    consume_any(p);
    // Tokenizer already strips quotes, so tok->text contains only the content
    ASTNode *node = ast_node_new_checked(AST_STRING);
    if (!node)
      return NULL;
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
    if (!node)
      return NULL;
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

  parser_set_error(p, "Unexpected token in value position");
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
 * @brief Grow the list elements array
 *
 * Doubles the capacity of the elements array.
 *
 * @param elements Pointer to elements array pointer
 * @param capacity Pointer to current capacity
 * @return true on success, false on allocation failure
 */
static bool list_grow_elements(ASTNode ***elements, size_t *capacity) {
  // Use a dummy count that's >= capacity to force growth
  return grow_array((void **)elements, *capacity, capacity, sizeof(ASTNode *));
}

/**
 * @brief Cleanup list elements array
 *
 * Frees all element AST nodes and the array itself.
 *
 * @param elements Elements array
 * @param element_count Number of elements
 */
static void list_cleanup_elements(ASTNode **elements, size_t element_count) {
  if (!elements)
    return;
  for (size_t i = 0; i < element_count; i++) {
    if (elements[i])
      ast_node_free(elements[i]);
  }
  free(elements);
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
  size_t element_capacity = INITIAL_ARRAY_CAPACITY;

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
    if (!node) {
      list_cleanup_elements(elements, element_count);
      return NULL;
    }
    node->as.list.elements = elements;
    node->as.list.element_count = 0;
    return node;
  }

  // Parse first element
  ASTNode *first = parse_expression(p);
  if (!first) {
    list_cleanup_elements(elements, element_count);
    return NULL;
  }
  elements[element_count++] = first;

  // Parse remaining elements (comma-separated)
  while (peek(p, 0) && peek(p, 0)->type == TOK_COMMA) {
    consume_any(p); // consume comma

    if (element_count == element_capacity) {
      if (!list_grow_elements(&elements, &element_capacity)) {
        fprintf(stderr, "Failed to grow list elements array\n");
        list_cleanup_elements(elements, element_count);
        return NULL;
      }
    }

    ASTNode *elem = parse_expression(p);
    if (!elem) {
      list_cleanup_elements(elements, element_count);
      return NULL;
    }
    elements[element_count++] = elem;
  }

  ASTNode *node = ast_node_new_checked(AST_LIST);
  if (!node) {
    list_cleanup_elements(elements, element_count);
    return NULL;
  }
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
  if (!node) {
    ast_node_free(start);
    ast_node_free(end);
    if (step)
      ast_node_free(step);
    return NULL;
  }
  node->as.range.start = start;
  node->as.range.end = end;
  node->as.range.step = step; // NULL means step=1
  return node;
}

/**
 * @brief Parse a map key (identifier or expression)
 *
 * If the key is an identifier (TOK_NAME), converts it to a string literal.
 * Otherwise, parses it as an expression.
 *
 * @param p Parser state
 * @return AST node for the key, or NULL on error
 */
static ASTNode *map_parse_key(Parser *p) {
  Token *key_tok = peek(p, 0);
  if (key_tok && key_tok->type == TOK_NAME) {
    // Convert identifier to string literal
    consume_any(p); // consume identifier
    ASTNode *key = ast_node_new_checked(AST_STRING);
    if (!key)
      return NULL;
    key->as.string.value = strdup(key_tok->text);
    if (!key->as.string.value) {
      free(key);
      return NULL;
    }
    key->as.string.length = key_tok->length;
    return key;
  } else {
    // Parse as expression (for number, string, bool, null keys)
    return parse_expression(p);
  }
}

/**
 * @brief Grow the map entries arrays
 *
 * Doubles the capacity of both keys and values arrays.
 *
 * @param keys Pointer to keys array pointer
 * @param values Pointer to values array pointer
 * @param capacity Pointer to current capacity
 * @return true on success, false on allocation failure
 */
static bool map_grow_entries(ASTNode ***keys, ASTNode ***values,
                             size_t *capacity) {
  // Grow both arrays - if second fails, first is already grown, so we can't
  // easily rollback. This is acceptable since we check both before using.
  bool keys_ok =
      grow_array((void **)keys, *capacity, capacity, sizeof(ASTNode *));
  bool values_ok =
      grow_array((void **)values, *capacity, capacity, sizeof(ASTNode *));
  if (!keys_ok || !values_ok) {
    // If one succeeded and the other failed, we have a problem
    // In practice, this is very rare and indicates severe memory pressure
    return false;
  }
  return true;
}

/**
 * @brief Cleanup map entries arrays
 *
 * Frees all key and value AST nodes and the arrays themselves.
 *
 * @param keys Keys array
 * @param values Values array
 * @param entry_count Number of entries
 */
static void map_cleanup_entries(ASTNode **keys, ASTNode **values,
                                size_t entry_count) {
  if (!keys || !values)
    return;
  for (size_t i = 0; i < entry_count; i++) {
    if (keys[i])
      ast_node_free(keys[i]);
    if (values[i])
      ast_node_free(values[i]);
  }
  free(keys);
  free(values);
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
  size_t entry_capacity = INITIAL_ARRAY_CAPACITY;

  keys = malloc(sizeof(ASTNode *) * entry_capacity);
  values = malloc(sizeof(ASTNode *) * entry_capacity);
  if (!keys || !values) {
    map_cleanup_entries(keys, values, entry_count);
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
    if (!node) {
      map_cleanup_entries(keys, values, entry_count);
      return NULL;
    }
    node->as.map.keys = keys;
    node->as.map.values = values;
    node->as.map.entry_count = 0;
    return node;
  }

  // Parse first entry
  ASTNode *key = map_parse_key(p);
  if (!key) {
    map_cleanup_entries(keys, values, entry_count);
    return NULL;
  }

  // Expect colon
  if (!consume(p, TOK_COLON)) {
    ast_node_free(key);
    map_cleanup_entries(keys, values, entry_count);
    return NULL;
  }

  ASTNode *value = parse_expression(p);
  if (!value) {
    ast_node_free(key);
    map_cleanup_entries(keys, values, entry_count);
    return NULL;
  }

  keys[entry_count] = key;
  values[entry_count] = value;
  entry_count++;

  // Parse remaining entries (comma-separated)
  while (peek(p, 0) && peek(p, 0)->type == TOK_COMMA) {
    consume_any(p); // consume comma

    if (entry_count == entry_capacity) {
      if (!map_grow_entries(&keys, &values, &entry_capacity)) {
        map_cleanup_entries(keys, values, entry_count);
        return NULL;
      }
    }

    key = map_parse_key(p);
    if (!key) {
      map_cleanup_entries(keys, values, entry_count);
      return NULL;
    }

    // Expect colon
    if (!consume(p, TOK_COLON)) {
      ast_node_free(key);
      map_cleanup_entries(keys, values, entry_count);
      return NULL;
    }

    value = parse_expression(p);
    if (!value) {
      ast_node_free(key);
      map_cleanup_entries(keys, values, entry_count);
      return NULL;
    }

    keys[entry_count] = key;
    values[entry_count] = value;
    entry_count++;
  }

  ASTNode *node = ast_node_new_checked(AST_MAP);
  if (!node) {
    map_cleanup_entries(keys, values, entry_count);
    return NULL;
  }
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
  size_t part_capacity = INITIAL_ARRAY_CAPACITY;
  size_t part_count = 0;
  ASTNode **parts = malloc(sizeof(ASTNode *) * part_capacity);
  if (!parts) {
    fprintf(stderr, "Failed to allocate memory for f-string parts\n");
    return NULL;
  }

  size_t i = 0;
  while (i < content_len) {
    size_t start = i;
    size_t brace_start = fstring_find_next_brace(content, content_len, i);

    // Add string literal part (from start to brace_start)
    if (brace_start > start) {
      ASTNode *str_node =
          fstring_create_string_part(content, start, brace_start);
      if (!str_node ||
          !fstring_add_part(&parts, &part_count, &part_capacity, str_node)) {
        fstring_cleanup_parts(parts, part_count);
        return NULL;
      }
    }

    // If we found a {, parse the expression inside
    if (brace_start < content_len) {
      size_t expr_start = brace_start + 1;
      size_t brace_end =
          fstring_find_matching_brace(content, content_len, expr_start);

      if (brace_end >= content_len) {
        fprintf(stderr, "Unmatched { in f-string\n");
        fstring_cleanup_parts(parts, part_count);
        return NULL;
      }

      // Parse expression from f-string content
      ASTNode *expr_node =
          fstring_parse_expression(content, expr_start, brace_end);
      if (!expr_node ||
          !fstring_add_part(&parts, &part_count, &part_capacity, expr_node)) {
        fstring_cleanup_parts(parts, part_count);
        return NULL;
      }

      i = brace_end + 1; // Skip }
    } else {
      i = content_len; // No more braces, we're done
    }
  }

  // If no parts, add empty string
  if (part_count == 0) {
    ASTNode *empty_str = ast_node_new_checked(AST_STRING);
    if (!empty_str) {
      free(parts);
      return NULL;
    }
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
  if (!node) {
    fstring_cleanup_parts(parts, part_count);
    return NULL;
  }
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
      if (!index_node) {
        ast_node_free(expr);
        ast_node_free(index);
        return NULL;
      }
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
      if (!slice_node) {
        ast_node_free(expr);
        ast_node_free(start);
        if (end)
          ast_node_free(end);
        return NULL;
      }
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
 * Recursively parses expressions respecting operator precedence and
 * associativity. Handles unary operators (NOT) and binary operators
 * (arithmetic, comparisons, logical).
 *
 * @param p Parser state
 * @param min_prec Minimum precedence to parse (stops when encountering lower
 * precedence)
 * @return AST node for the expression, or NULL on error
 */
static ASTNode *parse_expression_prec(Parser *p, int min_prec) {
  // Check recursion depth
  if (!check_recursion_depth(p)) {
    return NULL;
  }

  // Handle unary operators (NOT and negation)
  ASTNode *left = NULL;
  Token *tok = peek(p, 0);
  if (tok && tok->type == TOK_NOT) {
    consume_any(p); // consume NOT
    ASTNode *operand =
        parse_expression_prec(p, 10); // High precedence to bind tightly
    if (!operand) {
      decrement_recursion_depth(p);
      return NULL;
    }
    ASTNode *node = ast_node_new_checked(AST_BINOP);
    if (!node) {
      ast_node_free(operand);
      decrement_recursion_depth(p);
      return NULL;
    }
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
      ASTNode *operand =
          parse_expression_prec(p, 10); // High precedence to bind tightly
      if (!operand) {
        decrement_recursion_depth(p);
        return NULL;
      }
      ASTNode *node = ast_node_new_checked(AST_BINOP);
      if (!node) {
        ast_node_free(operand);
        decrement_recursion_depth(p);
        return NULL;
      }
      node->as.binop.left = operand;
      node->as.binop.op = BINOP_NEG; // Use a new unary operator
      node->as.binop.right = NULL;   // NULL indicates unary operation
      left = node;
    } else {
      // Binary subtraction - parse primary and continue
      left = parse_primary(p);
      if (!left) {
        decrement_recursion_depth(p);
        return NULL;
      }
    }
  } else {
    // Parse primary expression (values, list literals, postfix operations)
    left = parse_primary(p);
    if (!left) {
      decrement_recursion_depth(p);
      return NULL;
    }
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
      decrement_recursion_depth(p);
      return NULL;
    }

    // Build AST node
    ASTNode *node = ast_node_new_checked(AST_BINOP);
    if (!node) {
      ast_node_free(left);
      ast_node_free(right);
      decrement_recursion_depth(p);
      return NULL;
    }
    node->as.binop.left = left;
    node->as.binop.op = op;
    node->as.binop.right = right;
    left = node;
  }

  decrement_recursion_depth(p);
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
 * Conditions are full expressions (can include comparisons and logical
 * operators).
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
 * @brief Parse an index assignment
 *
 * Parses: "let var at index to value"
 *
 * @param p Parser state
 * @param indent Indent level
 * @param name Variable name token
 * @return AST node for index assignment, or NULL on error
 */
static ASTNode *assignment_parse_index(Parser *p, int indent, Token *name) {
  consume(p, TOK_AT);

  ASTNode *index = parse_expression(p);
  if (!index) {
    return NULL;
  }

  if (!consume(p, TOK_TO)) {
    ast_node_free(index);
    return NULL;
  }

  ASTNode *value = parse_expression(p);
  if (!value) {
    ast_node_free(index);
    return NULL;
  }

  if (!consume(p, TOK_NEWLINE)) {
    ast_node_free(index);
    ast_node_free(value);
    return NULL;
  }

  ASTNode *node = ast_node_new_checked(AST_ASSIGN_INDEX);
  if (!node) {
    ast_node_free(index);
    ast_node_free(value);
    return NULL;
  }
  node->indent = indent;

  // Create a variable AST node for the target
  ASTNode *target = ast_node_new_checked(AST_VAR);
  if (!target) {
    ast_node_free(index);
    ast_node_free(value);
    free(node);
    return NULL;
  }
  target->as.var_name = strdup(name->text);
  if (!target->as.var_name) {
    ast_node_free(index);
    ast_node_free(value);
    free(target);
    free(node);
    return NULL;
  }
  node->as.assign_index.target = target;
  node->as.assign_index.index = index;
  node->as.assign_index.value = value;

  return node;
}

/**
 * @brief Parse a regular assignment
 *
 * Parses: "set/let var to value [as type]"
 *
 * @param p Parser state
 * @param indent Indent level
 * @param name Variable name token
 * @param is_mutable Whether this is a mutable (let) assignment
 * @return AST node for assignment, or NULL on error
 */
static ASTNode *assignment_parse_regular(Parser *p, int indent, Token *name,
                                         bool is_mutable) {
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
  if (!node) {
    ast_node_free(value);
    free(type_name);
    return NULL;
  }
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

  // Check if this is an index assignment: let var at index to value
  Token *at_token = peek(p, 0);
  if (at_token && at_token->type == TOK_AT) {
    return assignment_parse_index(p, indent, name);
  }

  // Regular assignment: set/let var to value
  return assignment_parse_regular(p, indent, name, is_mutable);
}

// Parse delete statement: delete var at key
static ASTNode *parse_delete(Parser *p, int indent) {
  consume(p, TOK_DELETE);

  Token *name = consume(p, TOK_NAME);
  if (!name)
    return NULL;

  if (!consume(p, TOK_AT))
    return NULL;

  ASTNode *key = parse_expression(p);
  if (!key)
    return NULL;

  if (!consume(p, TOK_NEWLINE)) {
    ast_node_free(key);
    return NULL;
  }

  ASTNode *node = ast_node_new_checked(AST_DELETE);
  if (!node) {
    ast_node_free(key);
    return NULL;
  }
  node->indent = indent;
  // Create a variable AST node for the target
  ASTNode *target = ast_node_new_checked(AST_VAR);
  if (!target) {
    ast_node_free(key);
    free(node);
    return NULL;
  }
  target->as.var_name = strdup(name->text);
  if (!target->as.var_name) {
    ast_node_free(key);
    free(target);
    free(node);
    return NULL;
  }
  node->as.delete_stmt.target = target;
  node->as.delete_stmt.key = key;

  return node;
}

// Parse raise statement: raise ErrorType "message" or raise "message"
static ASTNode *parse_raise(Parser *p, int indent) {
  consume(p, TOK_RAISE);

  char *error_type = NULL;
  ASTNode *message = NULL;

  // Check what comes next - if it's a name, it could be error type or a
  // variable
  Token *next = peek(p, 0);
  if (next && next->type == TOK_NAME) {
    // Check if this is followed by a string or expression
    Token *after = peek(p, 1);
    if (after && (after->type == TOK_STRING || after->type == TOK_FSTRING)) {
      // Syntax: raise ErrorType "message"
      consume(p, TOK_NAME);
      error_type = strdup(next->text);
      if (!error_type) {
        return NULL;
      }
      message = parse_expression(p);
      if (!message) {
        free(error_type);
        return NULL;
      }
    } else {
      // Syntax: raise expression (error_type is NULL)
      message = parse_expression(p);
      if (!message) {
        return NULL;
      }
    }
  } else if (next && (next->type == TOK_STRING || next->type == TOK_FSTRING)) {
    // Syntax: raise "message" (no type specified)
    message = parse_expression(p);
    if (!message) {
      return NULL;
    }
  } else {
    // Error: expected error type or message
    return NULL;
  }

  if (!consume(p, TOK_NEWLINE)) {
    free(error_type);
    ast_node_free(message);
    return NULL;
  }

  ASTNode *node = ast_node_new_checked(AST_RAISE);
  if (!node) {
    free(error_type);
    ast_node_free(message);
    return NULL;
  }
  node->indent = indent;
  node->as.raise_stmt.error_type = error_type;
  node->as.raise_stmt.message = message;

  return node;
}

/**
 * @brief Parse a catch block in a try statement
 *
 * Parses: catch [ErrorType] [as var]:
 * Returns false on error (caller should cleanup try_node).
 *
 * @param p Parser state
 * @param indent Expected indent level
 * @param try_node The try statement node
 * @param catch_capacity Pointer to current catch blocks capacity (will be
 * updated)
 * @return true on success, false on error
 */
static bool try_parse_catch_block(Parser *p, int indent, ASTNode *try_node,
                                  size_t *catch_capacity) {
  consume_any(p); // consume INDENT
  consume(p, TOK_CATCH);

  char *error_type = NULL;
  char *catch_var = NULL;

  // Check if next token is a name (could be error type or variable)
  Token *name_tok = peek(p, 0);
  if (name_tok && name_tok->type == TOK_NAME) {
    consume_any(p); // consume name

    // Check if next is "as" - if so, this was an error type
    Token *after_name = peek(p, 0);
    if (after_name && after_name->type == TOK_AS) {
      // Syntax: catch ErrorType as var:
      error_type = strdup(name_tok->text);
      if (!error_type) {
        return false;
      }
      consume(p, TOK_AS);

      // Parse catch variable name
      Token *var_tok = consume(p, TOK_NAME);
      if (!var_tok) {
        free(error_type);
        return false;
      }
      catch_var = strdup(var_tok->text);
      if (!catch_var) {
        free(error_type);
        return false;
      }
    } else {
      // Syntax: catch var: (catch all errors)
      catch_var = strdup(name_tok->text);
      if (!catch_var) {
        return false;
      }
    }
  }

  if (!consume(p, TOK_COLON)) {
    free(error_type);
    free(catch_var);
    return false;
  }

  if (!consume(p, TOK_NEWLINE)) {
    free(error_type);
    free(catch_var);
    return false;
  }

  size_t catch_block_size = 0;
  ASTNode **catch_block = parse_block(p, indent, &catch_block_size);
  if (!catch_block) {
    free(error_type);
    free(catch_var);
    return false;
  }

  // Grow catch blocks array if needed
  if (!grow_array((void **)&try_node->as.try_stmt.catch_blocks,
                  try_node->as.try_stmt.catch_block_count, catch_capacity,
                  sizeof(try_node->as.try_stmt.catch_blocks[0]))) {
    free(error_type);
    free(catch_var);
    for (size_t i = 0; i < catch_block_size; i++) {
      ast_node_free(catch_block[i]);
    }
    free(catch_block);
    return false;
  }

  // Add catch block to array
  size_t idx = try_node->as.try_stmt.catch_block_count++;
  try_node->as.try_stmt.catch_blocks[idx].error_type = error_type;
  try_node->as.try_stmt.catch_blocks[idx].catch_var = catch_var;
  try_node->as.try_stmt.catch_blocks[idx].catch_block = catch_block;
  try_node->as.try_stmt.catch_blocks[idx].catch_block_size = catch_block_size;

  return true;
}

/**
 * @brief Parse a finally block in a try statement
 *
 * Parses: finally:
 * Returns false on error (caller should cleanup try_node).
 *
 * @param p Parser state
 * @param indent Expected indent level
 * @param try_node The try statement node
 * @return true on success, false on error
 */
static bool try_parse_finally_block(Parser *p, int indent, ASTNode *try_node) {
  consume_any(p); // consume INDENT
  consume(p, TOK_FINALLY);

  if (!consume(p, TOK_COLON)) {
    return false;
  }

  if (!consume(p, TOK_NEWLINE)) {
    return false;
  }

  size_t finally_block_size = 0;
  ASTNode **finally_block = parse_block(p, indent, &finally_block_size);
  if (!finally_block) {
    return false;
  }

  try_node->as.try_stmt.finally_block = finally_block;
  try_node->as.try_stmt.finally_block_size = finally_block_size;

  return true;
}

// Parse try/catch/finally statement
static ASTNode *parse_try(Parser *p, int indent) {
  consume(p, TOK_TRY);

  if (!consume(p, TOK_COLON)) {
    return NULL;
  }

  if (!consume(p, TOK_NEWLINE)) {
    return NULL;
  }

  // Parse try block
  size_t try_block_size = 0;
  ASTNode **try_block = parse_block(p, indent, &try_block_size);
  if (!try_block) {
    return NULL;
  }

  ASTNode *node = ast_node_new_checked(AST_TRY);
  if (!node) {
    // Cleanup try_block
    for (size_t i = 0; i < try_block_size; i++) {
      ast_node_free(try_block[i]);
    }
    free(try_block);
    return NULL;
  }
  node->indent = indent;
  node->as.try_stmt.try_block = try_block;
  node->as.try_stmt.try_block_size = try_block_size;
  node->as.try_stmt.catch_blocks = NULL;
  node->as.try_stmt.catch_block_count = 0;
  node->as.try_stmt.finally_block = NULL;
  node->as.try_stmt.finally_block_size = 0;

  size_t catch_capacity = INITIAL_ARRAY_CAPACITY;
  node->as.try_stmt.catch_blocks =
      malloc(sizeof(node->as.try_stmt.catch_blocks[0]) * catch_capacity);
  if (!node->as.try_stmt.catch_blocks) {
    ast_node_free(node);
    return NULL;
  }

  // Parse optional catch and finally blocks
  while (p->pos < p->tokens->count) {
    Token *tok = peek(p, 0);
    if (!tok || tok->type != TOK_INDENT)
      break;

    int next_indent = tok->indent_level;
    if (next_indent != indent)
      break;

    Token *next_tok = peek(p, 1);
    if (!next_tok)
      break;

    if (next_tok->type == TOK_CATCH) {
      if (!try_parse_catch_block(p, indent, node, &catch_capacity)) {
        ast_node_free(node);
        return NULL;
      }
    } else if (next_tok->type == TOK_FINALLY) {
      if (!try_parse_finally_block(p, indent, node)) {
        ast_node_free(node);
        return NULL;
      }
      // Finally is typically last, but we'll continue to check for more blocks
    } else {
      // Not catch or finally, stop parsing
      break;
    }
  }

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
  if (!node) {
    ast_node_free(value);
    return NULL;
  }
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
  // Check recursion depth
  if (!check_recursion_depth(p)) {
    return NULL;
  }

  if (block_size)
    *block_size = 0;

  size_t capacity =
      INITIAL_ARRAY_CAPACITY * 2; // Larger initial capacity for blocks
  size_t count = 0;
  ASTNode **block = malloc(sizeof(ASTNode *) * capacity);
  if (!block) {
    fprintf(stderr, "Parser failed to allocate block statements\n");
    decrement_recursion_depth(p);
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
    } else if (tok->type == TOK_DELETE) {
      stmt = parse_delete(p, next_indent);
    } else if (tok->type == TOK_TRY) {
      stmt = parse_try(p, next_indent);
    } else if (tok->type == TOK_RAISE) {
      stmt = parse_raise(p, next_indent);
    }

    if (!stmt) {
      // Parsing failed; free previously parsed statements.
      for (size_t i = 0; i < count; i++) {
        ast_node_free(block[i]);
      }
      free(block);
      if (block_size)
        *block_size = 0;
      decrement_recursion_depth(p);
      return NULL;
    }

    if (!grow_array((void **)&block, count, &capacity, sizeof(ASTNode *))) {
      fprintf(stderr, "Parser failed to grow block statements\n");
      for (size_t i = 0; i < count; i++) {
        ast_node_free(block[i]);
      }
      free(block);
      if (block_size)
        *block_size = 0;
      decrement_recursion_depth(p);
      return NULL;
    }
    block[count++] = stmt;
  }

  if (block_size)
    *block_size = count;
  decrement_recursion_depth(p);
  return block;
}

/**
 * @brief Parse an else-if block in an if statement
 *
 * Parses: else if condition:
 * Returns false on error (caller should cleanup if_node).
 *
 * @param p Parser state
 * @param indent Expected indent level
 * @param if_node The if statement node
 * @return true on success, false on error
 */
static bool if_parse_else_if(Parser *p, int indent, ASTNode *if_node) {
  consume_any(p); // consume INDENT
  consume(p, TOK_ELSE);
  consume(p, TOK_IF);

  ASTNode *else_if_condition = parse_condition(p);
  if (!else_if_condition) {
    return false;
  }

  if (!consume(p, TOK_COLON)) {
    ast_node_free(else_if_condition);
    return false;
  }

  if (!consume(p, TOK_NEWLINE)) {
    ast_node_free(else_if_condition);
    return false;
  }

  size_t else_if_block_size = 0;
  ASTNode **else_if_block = parse_block(p, indent, &else_if_block_size);
  if (!else_if_block) {
    ast_node_free(else_if_condition);
    return false;
  }

  // Grow arrays
  size_t new_count = if_node->as.if_stmt.else_if_count + 1;
  ASTNode **new_conditions = realloc(if_node->as.if_stmt.else_if_conditions,
                                     sizeof(ASTNode *) * new_count);
  ASTNode ***new_blocks = realloc(if_node->as.if_stmt.else_if_blocks,
                                  sizeof(ASTNode **) * new_count);
  size_t *new_block_sizes = realloc(if_node->as.if_stmt.else_if_block_sizes,
                                    sizeof(size_t) * new_count);

  if (!new_conditions || !new_blocks || !new_block_sizes) {
    ast_node_free(else_if_condition);
    for (size_t i = 0; i < else_if_block_size; i++) {
      ast_node_free(else_if_block[i]);
    }
    free(else_if_block);
    return false;
  }

  if_node->as.if_stmt.else_if_conditions = new_conditions;
  if_node->as.if_stmt.else_if_blocks = new_blocks;
  if_node->as.if_stmt.else_if_block_sizes = new_block_sizes;
  if_node->as.if_stmt.else_if_conditions[new_count - 1] = else_if_condition;
  if_node->as.if_stmt.else_if_blocks[new_count - 1] = else_if_block;
  if_node->as.if_stmt.else_if_block_sizes[new_count - 1] = else_if_block_size;
  if_node->as.if_stmt.else_if_count = new_count;

  return true;
}

/**
 * @brief Parse an else block in an if statement
 *
 * Parses: else:
 * Returns false on error (caller should cleanup if_node).
 *
 * @param p Parser state
 * @param indent Expected indent level
 * @param if_node The if statement node
 * @return true on success, false on error
 */
static bool if_parse_else(Parser *p, int indent, ASTNode *if_node) {
  consume_any(p); // consume INDENT
  consume(p, TOK_ELSE);

  if (!consume(p, TOK_COLON)) {
    return false;
  }

  if (!consume(p, TOK_NEWLINE)) {
    return false;
  }

  size_t else_block_size = 0;
  ASTNode **else_block = parse_block(p, indent, &else_block_size);
  if (!else_block) {
    return false;
  }

  if_node->as.if_stmt.else_block = else_block;
  if_node->as.if_stmt.else_block_size = else_block_size;

  return true;
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
  if (!node) {
    ast_node_free(condition);
    // Cleanup block
    for (size_t i = 0; i < block_size; i++) {
      ast_node_free(block[i]);
    }
    free(block);
    return NULL;
  }
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
        if (!if_parse_else_if(p, indent, node)) {
          ast_node_free(node);
          return NULL;
        }
      } else {
        // It's just else
        if (!if_parse_else(p, indent, node)) {
          ast_node_free(node);
          return NULL;
        }
        break; // else is always last
      }
    } else {
      break; // Not an else/else-if, we're done
    }
  }

  return node;
}

/**
 * @brief Parse range iteration in a for statement
 *
 * Parses: range start to end [by step]
 * Sets iterable to start, end to end, and step to step (or NULL).
 * Returns false on error.
 *
 * @param p Parser state
 * @param iterable Output parameter for start/iterable expression
 * @param end Output parameter for end expression
 * @param step Output parameter for step expression (NULL if not provided)
 * @return true on success, false on error
 */
static bool for_parse_range_iteration(Parser *p, ASTNode **iterable,
                                      ASTNode **end, ASTNode **step) {
  consume_any(p); // consume TOK_RANGE

  ASTNode *start = parse_expression(p);
  if (!start)
    return false;

  if (!consume(p, TOK_TO)) {
    ast_node_free(start);
    return false;
  }

  *end = parse_expression(p);
  if (!*end) {
    ast_node_free(start);
    return false;
  }
  *iterable = start; // For range, iterable is the start value

  // Check for optional "by step" clause
  Token *after_end = peek(p, 0);
  if (after_end && after_end->type == TOK_BY) {
    consume_any(p); // consume TOK_BY
    *step = parse_expression(p);
    if (!*step) {
      ast_node_free(start);
      ast_node_free(*end);
      return false;
    }
  } else {
    *step = NULL;
  }

  return true;
}

/**
 * @brief Cleanup resources allocated during for statement parsing
 *
 * Frees iterable, end, step expressions and the block array.
 *
 * @param iterable Iterable expression (may be NULL)
 * @param end End expression (may be NULL)
 * @param step Step expression (may be NULL)
 * @param block Block array (may be NULL)
 * @param block_size Number of statements in block
 */
static void for_cleanup_resources(ASTNode *iterable, ASTNode *end,
                                  ASTNode *step, ASTNode **block,
                                  size_t block_size) {
  if (iterable)
    ast_node_free(iterable);
  if (end)
    ast_node_free(end);
  if (step)
    ast_node_free(step);
  if (block) {
    for (size_t i = 0; i < block_size; i++) {
      ast_node_free(block[i]);
    }
    free(block);
  }
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
    if (!for_parse_range_iteration(p, &iterable, &end, &step)) {
      return NULL;
    }
  } else {
    // List iteration: for item in list_expr
    is_range = false;
    iterable = parse_expression(p);
    if (!iterable)
      return NULL;
  }

  if (!consume(p, TOK_COLON)) {
    for_cleanup_resources(iterable, end, step, NULL, 0);
    return NULL;
  }

  if (!consume(p, TOK_NEWLINE)) {
    for_cleanup_resources(iterable, end, step, NULL, 0);
    return NULL;
  }

  size_t block_size = 0;
  ASTNode **block = parse_block(p, indent, &block_size);
  if (!block) {
    for_cleanup_resources(iterable, end, step, NULL, 0);
    return NULL;
  }

  ASTNode *node = ast_node_new_checked(AST_FOR);
  if (!node) {
    for_cleanup_resources(iterable, end, step, block, block_size);
    return NULL;
  }
  node->indent = indent;
  node->as.for_stmt.var = strdup(var->text);
  if (!node->as.for_stmt.var) {
    for_cleanup_resources(iterable, end, step, block, block_size);
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
  if (!node) {
    ast_node_free(condition);
    // Cleanup block
    for (size_t i = 0; i < block_size; i++) {
      ast_node_free(block[i]);
    }
    free(block);
    return NULL;
  }
  node->indent = indent;
  node->as.while_stmt.condition = condition;
  node->as.while_stmt.block = block;
  node->as.while_stmt.block_size = block_size;

  return node;
}

/**
 * @brief Parse function parameters
 *
 * Parses: "with param1, param2, param3"
 * Returns false on error. On success, params array is allocated and populated.
 *
 * @param p Parser state
 * @param params Output parameter for parameters array
 * @param param_count Output parameter for number of parameters
 * @param param_capacity Output parameter for capacity of params array
 * @return true on success, false on error
 */
static bool function_parse_parameters(Parser *p, char ***params,
                                      size_t *param_count,
                                      size_t *param_capacity) {
  Token *tok = peek(p, 0);
  if (!tok || tok->type != TOK_WITH) {
    *params = NULL;
    *param_count = 0;
    *param_capacity = 0;
    return true; // No parameters is valid
  }

  consume_any(p);

  *param_capacity = INITIAL_ARRAY_CAPACITY;
  *param_count = 0;
  *params = malloc(sizeof(char *) * *param_capacity);
  if (!*params) {
    fprintf(stderr, "parse_function: failed to allocate params array\n");
    return false;
  }

  Token *param = consume(p, TOK_NAME);
  if (!param) {
    free(*params);
    *params = NULL;
    return false;
  }
  char *param_name = strdup(param->text);
  if (!param_name) {
    free(*params);
    *params = NULL;
    return false;
  }
  (*params)[(*param_count)++] = param_name;

  while (peek(p, 0) && peek(p, 0)->type == TOK_COMMA) {
    consume_any(p);
    param = consume(p, TOK_NAME);
    if (!param) {
      function_cleanup_parameters(*params, *param_count);
      *params = NULL;
      return false;
    }

    if (!grow_array((void **)params, *param_count, param_capacity,
                    sizeof(char *))) {
      fprintf(stderr, "parse_function: failed to grow params array\n");
      function_cleanup_parameters(*params, *param_count);
      *params = NULL;
      return false;
    }
    char *param_name_loop = strdup(param->text);
    if (!param_name_loop) {
      function_cleanup_parameters(*params, *param_count);
      *params = NULL;
      return false;
    }
    (*params)[(*param_count)++] = param_name_loop;
  }

  return true;
}

/**
 * @brief Cleanup function parameters array
 *
 * Frees all parameter strings and the array itself.
 *
 * @param params Parameters array
 * @param param_count Number of parameters
 */
static void function_cleanup_parameters(char **params, size_t param_count) {
  if (!params)
    return;
  for (size_t i = 0; i < param_count; i++) {
    free(params[i]);
  }
  free(params);
}

// Parse function definition
static ASTNode *parse_function(Parser *p, int indent) {
  consume(p, TOK_FUNCTION);

  Token *name = consume(p, TOK_NAME);
  if (!name)
    return NULL;

  // Parse parameters
  size_t param_capacity = 0;
  size_t param_count = 0;
  char **params = NULL;
  if (!function_parse_parameters(p, &params, &param_count, &param_capacity)) {
    return NULL;
  }

  if (!consume(p, TOK_COLON)) {
    function_cleanup_parameters(params, param_count);
    return NULL;
  }

  if (!consume(p, TOK_NEWLINE)) {
    function_cleanup_parameters(params, param_count);
    return NULL;
  }

  size_t block_size = 0;
  ASTNode **block = parse_block(p, indent, &block_size);
  if (!block) {
    function_cleanup_parameters(params, param_count);
    return NULL;
  }

  ASTNode *node = ast_node_new_checked(AST_FUNCTION);
  if (!node) {
    function_cleanup_parameters(params, param_count);
    // Free block and its statements
    if (block) {
      for (size_t i = 0; i < block_size; i++) {
        ast_node_free(block[i]);
      }
      free(block);
    }
    return NULL;
  }
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

/**
 * @brief Parse function call arguments
 *
 * Parses: "with arg1, arg2, arg3"
 * Returns false on error. On success, args array is allocated and populated.
 *
 * @param p Parser state
 * @param args Output parameter for arguments array
 * @param arg_count Output parameter for number of arguments
 * @param arg_capacity Output parameter for capacity of args array
 * @return true on success, false on error
 */
static bool call_parse_arguments(Parser *p, ASTNode ***args, size_t *arg_count,
                                 size_t *arg_capacity) {
  Token *tok = peek(p, 0);
  if (!tok || tok->type != TOK_WITH) {
    *args = NULL;
    *arg_count = 0;
    *arg_capacity = 0;
    return true; // No arguments is valid
  }

  consume_any(p);

  *arg_capacity = INITIAL_ARRAY_CAPACITY;
  *arg_count = 0;
  *args = malloc(sizeof(ASTNode *) * *arg_capacity);
  if (!*args) {
    fprintf(stderr, "parse_call: failed to allocate argument array\n");
    return false;
  }

  ASTNode *arg = parse_expression(p);
  if (!arg) {
    free(*args);
    *args = NULL;
    return false;
  }
  (*args)[(*arg_count)++] = arg;

  while (peek(p, 0) && peek(p, 0)->type == TOK_COMMA) {
    consume_any(p);
    arg = parse_expression(p);
    if (!arg) {
      call_cleanup_arguments(*args, *arg_count);
      *args = NULL;
      return false;
    }

    if (!grow_array((void **)args, *arg_count, arg_capacity,
                    sizeof(ASTNode *))) {
      fprintf(stderr, "parse_call: failed to grow argument array\n");
      call_cleanup_arguments(*args, *arg_count);
      *args = NULL;
      return false;
    }
    (*args)[(*arg_count)++] = arg;
  }

  return true;
}

/**
 * @brief Cleanup function call arguments array
 *
 * Frees all argument AST nodes and the array itself.
 *
 * @param args Arguments array
 * @param arg_count Number of arguments
 */
static void call_cleanup_arguments(ASTNode **args, size_t arg_count) {
  if (!args)
    return;
  for (size_t i = 0; i < arg_count; i++) {
    ast_node_free(args[i]);
  }
  free(args);
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
  size_t arg_capacity = 0;
  size_t arg_count = 0;
  ASTNode **args = NULL;
  if (!call_parse_arguments(p, &args, &arg_count, &arg_capacity)) {
    return NULL;
  }

  // Only require newline if it's a statement (indent >= 0)
  if (indent >= 0) {
    if (!consume(p, TOK_NEWLINE)) {
      call_cleanup_arguments(args, arg_count);
      return NULL;
    }
  }

  ASTNode *node = ast_node_new_checked(AST_CALL);
  if (!node) {
    call_cleanup_arguments(args, arg_count);
    return NULL;
  }
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
  if (!node) {
    ast_node_free(value);
    return NULL;
  }
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
    size_t capacity = INITIAL_ARRAY_CAPACITY;
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

      if (!grow_array((void **)&imported_names, imported_count, &capacity,
                      sizeof(char *))) {
        // Cleanup
        for (size_t i = 0; i < imported_count; i++) {
          free(imported_names[i]);
        }
        free(module_name);
        free(imported_names);
        return NULL;
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
  if (!node) {
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
  if (!node)
    return NULL;
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
  if (!node)
    return NULL;
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
  case TOK_DELETE:
    return parse_delete(p, indent);
  case TOK_TRY:
    return parse_try(p, indent);
  case TOK_RAISE:
    return parse_raise(p, indent);
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
AST *parse(TokenArray *tokens, ParseError **out_err) {
  // Initialize error output to NULL
  if (out_err)
    *out_err = NULL;

  Parser p = {tokens, 0, 0, out_err};

  AST *ast = malloc(sizeof(AST));
  if (!ast)
    return NULL;

  ast->capacity = INITIAL_ARRAY_CAPACITY * 4; // Larger initial capacity for AST
  ast->count = 0;
  ast->statements = malloc(sizeof(ASTNode *) * ast->capacity);
  if (!ast->statements) {
    free(ast);
    return NULL;
  }

  bool has_errors = false;

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
      if (!grow_array((void **)&ast->statements, ast->count, &ast->capacity,
                      sizeof(ASTNode *))) {
        // Realloc failed - free the statement and report error
        ast_node_free(stmt);
        parser_set_error(&p, "Failed to grow AST statements array");
        // Free existing AST and return NULL
        for (size_t i = 0; i < ast->count; i++) {
          ast_node_free(ast->statements[i]);
        }
        free(ast->statements);
        free(ast);
        return NULL;
      }
      ast->statements[ast->count++] = stmt;
    } else {
      // Parse error occurred - track it
      has_errors = true;

      // If error output is provided and not already set, set it with details
      // about the first parse error encountered
      if (out_err && *out_err == NULL) {
        Token *error_tok = peek(&p, 0);
        if (error_tok) {
          char msg[256];
          snprintf(msg, sizeof(msg),
                   "Parse error: failed to parse statement starting with token "
                   "type %s",
                   token_type_name(error_tok->type));
          parser_set_error(&p, msg);
        } else {
          parser_set_error(&p, "Parse error: failed to parse statement "
                               "(unexpected end of input)");
        }
      }

      // Skip to next line on error (recovery mode - continue parsing)
      // This allows partial ASTs to be returned, but callers can check
      // out_err to know if the AST is incomplete due to parse errors
      while (tok && tok->type != TOK_NEWLINE && tok->type != TOK_EOF) {
        consume_any(&p);
        tok = peek(&p, 0);
      }
    }
  }

  // If errors occurred, the error has been set in out_err (if provided)
  // The AST is returned but may be partial - callers should check out_err
  // to determine if the AST contains all statements or if some failed to parse
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
  case AST_ASSIGN_INDEX:
    ast_node_free(node->as.assign_index.target);
    ast_node_free(node->as.assign_index.index);
    ast_node_free(node->as.assign_index.value);
    break;
  case AST_DELETE:
    ast_node_free(node->as.delete_stmt.target);
    ast_node_free(node->as.delete_stmt.key);
    break;
  case AST_TRY:
    // Free try block
    for (size_t i = 0; i < node->as.try_stmt.try_block_size; i++) {
      ast_node_free(node->as.try_stmt.try_block[i]);
    }
    free(node->as.try_stmt.try_block);
    // Free catch blocks
    if (node->as.try_stmt.catch_blocks) {
      for (size_t i = 0; i < node->as.try_stmt.catch_block_count; i++) {
        free(node->as.try_stmt.catch_blocks[i].error_type);
        free(node->as.try_stmt.catch_blocks[i].catch_var);
        for (size_t j = 0;
             j < node->as.try_stmt.catch_blocks[i].catch_block_size; j++) {
          ast_node_free(node->as.try_stmt.catch_blocks[i].catch_block[j]);
        }
        free(node->as.try_stmt.catch_blocks[i].catch_block);
      }
      free(node->as.try_stmt.catch_blocks);
    }
    // Free finally block
    if (node->as.try_stmt.finally_block) {
      for (size_t i = 0; i < node->as.try_stmt.finally_block_size; i++) {
        ast_node_free(node->as.try_stmt.finally_block[i]);
      }
      free(node->as.try_stmt.finally_block);
    }
    break;
  case AST_RAISE:
    free(node->as.raise_stmt.error_type);
    ast_node_free(node->as.raise_stmt.message);
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
