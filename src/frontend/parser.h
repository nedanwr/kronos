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
  AST_MAP,
  AST_INDEX,
  AST_SLICE,
  AST_ASSIGN_INDEX, // List/map index assignment: let var at index to value
  AST_DELETE,       // Map key deletion: delete var at key
  AST_TRY,          // Try/catch/finally exception handling
  AST_RAISE,        // Raise exception: raise ErrorType "message"
} ASTNodeType;

typedef struct ASTNode ASTNode;

// Binary operator
typedef enum {
  BINOP_ADD,
  BINOP_SUB,
  BINOP_MUL,
  BINOP_DIV,
  BINOP_MOD,
  BINOP_EQ,
  BINOP_NEQ,
  BINOP_GT,
  BINOP_LT,
  BINOP_GTE,
  BINOP_LTE,
  BINOP_AND,
  BINOP_OR,
  BINOP_NOT,
  BINOP_NEG,
} BinOp;

struct ASTNode {
  ASTNodeType type;
  int indent;
  size_t line;   // 1-based line number where this node starts (0 if unknown)
  size_t column; // 1-based column number where this node starts (0 if unknown)
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

    // Try/catch/finally exception handling
    struct {
      ASTNode **try_block; // Try block (required)
      size_t try_block_size;
      // Catch blocks (can have multiple catch blocks for different error types)
      struct {
        char *error_type;      // Error type to catch (NULL means catch all)
        char *catch_var;       // Catch variable name (NULL if no variable)
        ASTNode **catch_block; // Catch block statements
        size_t catch_block_size;
      } *catch_blocks;
      size_t catch_block_count;
      ASTNode **finally_block; // Finally block (NULL if no finally)
      size_t finally_block_size;
    } try_stmt;

    // Raise exception: raise ErrorType "message" or raise "message"
    struct {
      char
          *error_type; // Error type name (e.g., "ValueError"), NULL for generic
      ASTNode *message; // Error message expression
    } raise_stmt;

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

    // Import: import module_name [from "file.kr"] or from module_name import
    // func1, func2
    struct {
      char *module_name; // Module name (for namespace)
      char *file_path;   // File path if importing from file (NULL for built-in)
      char **imported_names; // Function names to import (NULL for full import)
      size_t imported_count; // Number of imported names
      bool is_from_import;   // true for "from X import Y", false for "import X
                             // from Y"
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

    // Map literal: map key: value, key2: value2
    struct {
      ASTNode **keys;
      ASTNode **values;
      size_t entry_count;
    } map;

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

    // Index assignment: let var at index to value
    struct {
      ASTNode *target; // Variable or expression (list/map)
      ASTNode *index;  // Index/key expression
      ASTNode *value;  // Value to assign
    } assign_index;

    // Delete: delete var at key
    struct {
      ASTNode *target; // Variable (map)
      ASTNode *key;    // Key expression
    } delete_stmt;
  } as;
};

typedef struct {
  ASTNode **statements;
  size_t count;
  size_t capacity;
} AST;

// Error information for parsing failures
typedef struct {
  char *message; // Error message (heap-allocated, owned by ParseError)
  size_t line;   // 1-based line number where error occurred (0 if unknown)
  size_t column; // 1-based column number where error occurred (0 if unknown)
} ParseError;

// Parse tokens into AST
// @param tokens Token array to parse (must not be NULL)
// @param out_err Optional pointer to receive error details on failure.
//                If non-NULL and an error occurs, *out_err is set to a
//                heap-allocated ParseError (caller must free with
//                parse_error_free()). On success, *out_err is set to NULL.
// @return AST* on success, NULL on error (allocation failure or parse error).
//         On error, if out_err is non-NULL, *out_err contains error details.
AST *parse(TokenArray *tokens, ParseError **out_err);

// Free a ParseError structure
// @param err ParseError to free (may be NULL, in which case this is a no-op)
void parse_error_free(ParseError *err);
void ast_free(AST *ast);
void ast_node_free(ASTNode *node);

// Debug
void ast_print(AST *ast);

#endif // KRONOS_PARSER_H
