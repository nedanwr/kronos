#define _POSIX_C_SOURCE 200809L
#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  TokenArray *tokens;
  size_t pos;
} Parser;

// Helper functions
static Token *peek(Parser *p, int offset) {
  size_t idx = p->pos + offset;
  if (idx >= p->tokens->count)
    return NULL;
  return &p->tokens->tokens[idx];
}

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

static Token *consume_any(Parser *p) {
  if (p->pos >= p->tokens->count)
    return NULL;
  return &p->tokens->tokens[p->pos++];
}

// Forward declarations
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

// Parse value (number, string, variable, boolean, null)
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

  if (tok->type == TOK_NULL) {
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

  fprintf(stderr, "Unexpected token in value position\n");
  return NULL;
}

// Get operator precedence (higher number = tighter binding)
static int get_precedence(TokenType type) {
  switch (type) {
  case TOK_PLUS:
  case TOK_MINUS:
    return 1; // Lower precedence
  case TOK_TIMES:
  case TOK_DIVIDED:
    return 2; // Higher precedence
  default:
    return 0; // Not a binary operator
  }
}

// Parse expression with precedence-climbing (Pratt parser)
static ASTNode *parse_expression_prec(Parser *p, int min_prec) {
  // Parse left operand
  ASTNode *left = parse_value(p);
  if (!left)
    return NULL;

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

    switch (tok->type) {
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

// Parse expression (with binary operators and precedence)
static ASTNode *parse_expression(Parser *p) {
  return parse_expression_prec(p, 1); // Start with minimum precedence
}

// Parse condition (for if/while)
static ASTNode *parse_condition(Parser *p) {
  ASTNode *left = parse_expression(p);
  if (!left)
    return NULL;

  if (!consume(p, TOK_IS)) {
    ast_node_free(left);
    return NULL;
  }

  bool negated = false;
  Token *tok = peek(p, 0);
  if (tok && tok->type == TOK_NOT) {
    consume_any(p);
    negated = true;
  }

  tok = peek(p, 0);
  if (!tok) {
    ast_node_free(left);
    return NULL;
  }

  BinOp op;
  if (tok->type == TOK_EQUAL) {
    consume_any(p);
    op = negated ? BINOP_NEQ : BINOP_EQ;
  } else if (tok->type == TOK_GREATER) {
    consume_any(p);
    if (!consume(p, TOK_THAN)) {
      ast_node_free(left);
      return NULL;
    }
    op = negated ? BINOP_LTE : BINOP_GT;
  } else if (tok->type == TOK_LESS) {
    consume_any(p);
    if (!consume(p, TOK_THAN)) {
      ast_node_free(left);
      return NULL;
    }
    op = negated ? BINOP_GTE : BINOP_LT;
  } else {
    fprintf(stderr, "Unknown comparison operator\n");
    ast_node_free(left);
    return NULL;
  }

  ASTNode *right = parse_expression(p);
  if (!right) {
    ast_node_free(left);
    return NULL;
  }

  ASTNode *node = ast_node_new_checked(AST_BINOP);
  node->as.binop.left = left;
  node->as.binop.op = op;
  node->as.binop.right = right;

  return node;
}

// Parse assignment
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

// Parse block (indented statements)
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
  if (!consume(p, TOK_RANGE))
    return NULL;

  ASTNode *start = parse_expression(p);
  if (!start)
    return NULL;

  if (!consume(p, TOK_TO)) {
    ast_node_free(start);
    return NULL;
  }

  ASTNode *end = parse_expression(p);
  if (!end) {
    ast_node_free(start);
    return NULL;
  }

  if (!consume(p, TOK_COLON)) {
    ast_node_free(start);
    ast_node_free(end);
    return NULL;
  }

  if (!consume(p, TOK_NEWLINE)) {
    ast_node_free(start);
    ast_node_free(end);
    return NULL;
  }

  size_t block_size = 0;
  ASTNode **block = parse_block(p, indent, &block_size);
  if (!block) {
    ast_node_free(start);
    ast_node_free(end);
    return NULL;
  }

  ASTNode *node = ast_node_new_checked(AST_FOR);
  node->indent = indent;
  node->as.for_stmt.var = strdup(var->text);
  if (!node->as.for_stmt.var) {
    ast_node_free(start);
    ast_node_free(end);
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
  node->as.for_stmt.start = start;
  node->as.for_stmt.end = end;
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

  if (!consume(p, TOK_NEWLINE)) {
    for (size_t i = 0; i < arg_count; i++)
      ast_node_free(args[i]);
    free(args);
    return NULL;
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
  default:
    return NULL;
  }
}

// Parse tokens into AST
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

// Free AST node
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
    break;
  case AST_FOR:
    free(node->as.for_stmt.var);
    ast_node_free(node->as.for_stmt.start);
    ast_node_free(node->as.for_stmt.end);
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
  default:
    break;
  }

  free(node);
}

// Free AST
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
