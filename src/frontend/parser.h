#ifndef KRONOS_PARSER_H
#define KRONOS_PARSER_H

#include "../core/runtime.h"
#include "tokenizer.h"

// AST node types
typedef enum {
  AST_ASSIGN,
  AST_PRINT,
  AST_IF,
  AST_FOR,
  AST_WHILE,
  AST_BREAK,
  AST_CONTINUE,
  AST_FUNCTION,
  AST_CALL,
  AST_RETURN,
  AST_IMPORT,
  AST_NUMBER,
  AST_STRING,
  AST_FSTRING,
  AST_BOOL,
  AST_NULL,
  AST_VAR,
  AST_BINOP,
  AST_LIST,
  AST_RANGE,
  AST_INDEX,
  AST_SLICE,
} ASTNodeType;

typedef struct ASTNode ASTNode;

// Binary operator
typedef enum {
  BINOP_ADD,
  BINOP_SUB,
  BINOP_MUL,
  BINOP_DIV,
  BINOP_EQ,
  BINOP_NEQ,
  BINOP_GT,
  BINOP_LT,
  BINOP_GTE,
  BINOP_LTE,
  BINOP_AND,
  BINOP_OR,
  BINOP_NOT,
} BinOp;

struct ASTNode {
  ASTNodeType type;
  int indent;
  union {
    // Literals
    double number;
    struct {
      char *value;
      size_t length;
    } string;
    struct {
      // F-string parts: alternating string literals and expressions
      // parts[0] is always a string (may be empty)
      // parts[1] is an expression (if present)
      // parts[2] is a string (if present)
      // etc.
      ASTNode **parts;
      size_t part_count;
    } fstring;
    bool boolean;
    char *var_name;

    // Assignment: set/let name to value [as type]
    struct {
      char *name;
      ASTNode *value;
      bool is_mutable; // true for 'let', false for 'set'
      char *type_name; // Optional type annotation (NULL if not specified)
    } assign;

    // Print statement
    struct {
      ASTNode *value;
    } print;

    // Binary operation (arithmetic and comparison)
    struct {
      ASTNode *left;
      BinOp op;
      ASTNode *right;
    } binop;

    // Control flow
    struct {
      ASTNode *condition;
      ASTNode **block;
      size_t block_size;
      // Else-if chain: list of (condition, block) pairs
      ASTNode **else_if_conditions;
      ASTNode ***else_if_blocks;
      size_t *else_if_block_sizes;
      size_t else_if_count;
      // Else block (optional)
      ASTNode **else_block;
      size_t else_block_size;
    } if_stmt;

    struct {
      char *var;
      ASTNode *iterable; // For range: contains range expression, for list: list
                         // expression
      bool is_range;     // true for range iteration, false for list iteration
      ASTNode *end;      // Only used for range (end value), NULL for list
      ASTNode *step;     // Only used for range (step value), NULL means step=1
      ASTNode **block;
      size_t block_size;
    } for_stmt;

    struct {
      ASTNode *condition;
      ASTNode **block;
      size_t block_size;
    } while_stmt;

    // Functions
    struct {
      char *name;
      char **params;
      size_t param_count;
      ASTNode **block;
      size_t block_size;
    } function;

    struct {
      char *name;
      ASTNode **args;
      size_t arg_count;
    } call;

    struct {
      ASTNode *value;
    } return_stmt;

    // Import: import module_name
    struct {
      char *module_name;
    } import;

    // List literal: list 1, 2, 3
    struct {
      ASTNode **elements;
      size_t element_count;
    } list;

    // Range literal: range 1 to 10 [by 2]
    struct {
      ASTNode *start;
      ASTNode *end;
      ASTNode *step; // NULL means step=1
    } range;

    // Indexing: list at 0
    struct {
      ASTNode *list_expr;
      ASTNode *index;
    } index;

    // Slicing: list from 1 to 3 or list from 2 to end
    struct {
      ASTNode *list_expr;
      ASTNode *start; // NULL means from beginning
      ASTNode *end;   // NULL means to end
    } slice;
  } as;
};

typedef struct {
  ASTNode **statements;
  size_t count;
  size_t capacity;
} AST;

// Parse tokens into AST
AST *parse(TokenArray *tokens);
void ast_free(AST *ast);
void ast_node_free(ASTNode *node);

// Debug
void ast_print(AST *ast);

#endif // KRONOS_PARSER_H
