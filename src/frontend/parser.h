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
  AST_FUNCTION,
  AST_CALL,
  AST_RETURN,
  AST_NUMBER,
  AST_STRING,
  AST_BOOL,
  AST_NULL,
  AST_VAR,
  AST_BINOP,
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
    } if_stmt;

    struct {
      char *var;
      ASTNode *start;
      ASTNode *end;
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
