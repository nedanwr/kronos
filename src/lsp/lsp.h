/**
 * @file lsp.h
 * @brief Shared types, structs, and forward declarations for LSP server
 */

#ifndef LSP_H
#define LSP_H

#include "../frontend/parser.h"
#include <stdbool.h>
#include <stddef.h>

// Maximum recursion depth for AST traversal to prevent stack overflow
#define MAX_AST_DEPTH 1000

// Buffer size constants
#define LSP_STACK_PATTERN_SIZE 128      // Stack buffer for small JSON key patterns
#define LSP_PATTERN_BUFFER_SIZE 256     // Pattern matching and URI buffers
#define LSP_ERROR_MSG_SIZE 512          // Error message buffers
#define LSP_INITIAL_BUFFER_SIZE 8192    // Initial diagnostics and response buffers
#define LSP_HOVER_BUFFER_SIZE 4096      // Hover text and stack buffers
#define LSP_REFERENCES_BUFFER_SIZE 16384 // References and rename operation buffers
#define LSP_LARGE_BUFFER_SIZE 32768     // Large result buffers (formatting, etc.)

/**
 * Symbol types in the symbol table
 */
typedef enum {
  SYMBOL_VARIABLE,  /**< Variable declaration (set/let) */
  SYMBOL_FUNCTION,  /**< Function definition */
  SYMBOL_PARAMETER, /**< Function parameter */
} SymbolType;

/**
 * Symbol information structure
 *
 * Represents a symbol (variable, function, or parameter) in the source code.
 * Symbols are stored in a linked list for the document's symbol table.
 */
typedef struct Symbol {
  char *name;      /**< Symbol name */
  SymbolType type; /**< Type of symbol */
  size_t line;     /**< 1-based line number where symbol is defined */
  size_t column;   /**< 1-based column number where symbol is defined */
  char *type_name; /**< Optional type annotation (e.g., "number", "string") */
  bool is_mutable; /**< For variables: true for 'let', false for 'set' */
  size_t param_count;  /**< For functions: number of parameters */
  bool written;        /**< Track if variable has been assigned to */
  bool read;           /**< Track if variable has been read from */
  struct Symbol *next; /**< Next symbol in linked list */
} Symbol;

/**
 * Imported module information
 */
typedef struct ImportedModule {
  char *name;      /**< Module name (e.g., "utils") */
  char *file_path; /**< File path if importing from file (NULL for built-in) */
  Symbol *exports; /**< List of exported symbols (functions, variables) from
                      this module */
  struct ImportedModule *next; /**< Next imported module */
} ImportedModule;

/**
 * Document state structure
 *
 * Maintains the current state of an open document, including its text,
 * parsed AST, and symbol table.
 */
typedef struct {
  char *uri;       /**< Document URI (file path) */
  char *text;      /**< Full document text */
  Symbol *symbols; /**< Linked list of symbols in the document */
  AST *ast;        /**< Parsed Abstract Syntax Tree */
  ImportedModule *imported_modules; /**< Linked list of imported modules */
} DocumentState;

/**
 * Type inference for expressions
 */
typedef enum {
  TYPE_UNKNOWN,
  TYPE_NUMBER,
  TYPE_STRING,
  TYPE_LIST,
  TYPE_MAP,
  TYPE_RANGE,
  TYPE_BOOL,
  TYPE_NULL
} ExprType;

// Forward declarations
// Global document state - NOTE: Currently only supports single document
// TODO: Implement multi-document support using hash table keyed by URI
// to comply with LSP specification requirements
extern DocumentState *g_doc;

// Message handling (lsp_messages.c)
bool read_lsp_message(char **out_body, size_t *out_length);
void json_escape(const char *input, char *output, size_t output_size);
void json_escape_markdown(const char *input, char *output, size_t output_size);
void send_response(const char *id, const char *result);
void send_notification(const char *method, const char *params);
const char *skip_ws(const char *s);
const char *find_value_start(const char *json, const char *key);
char *json_get_string_value(const char *json, const char *key);
char *json_get_unquoted_value(const char *json, const char *key);
char *json_get_id_value(const char *json);
char *json_get_nested_value(const char *json, const char *path);

// Utilities (lsp_utils.c)
void free_symbols(Symbol *sym);
void get_node_position(ASTNode *node, size_t *line, size_t *col);
void free_imported_modules(ImportedModule *modules);
bool is_module_imported(const char *module_name);
Symbol *load_module_exports(const char *file_path);
char *get_module_hover_info(ImportedModule *mod);
void free_document_state(DocumentState *doc);
void process_statements_for_symbols(ASTNode **statements, size_t count,
                                     Symbol ***tail, Symbol **head);
void build_symbol_table(DocumentState *doc, AST *ast, const char *text);
Symbol *find_symbol(const char *const name);
char *get_word_at_position(const char *source, size_t line, size_t character);
bool find_nth_occurrence(const char *text, const char *varname, size_t n,
                         size_t *line, size_t *col);
void find_node_position(ASTNode *node, const char *text, const char *pattern,
                        size_t *line, size_t *col);
bool get_constant_number(ASTNode *node, double *value);
int get_builtin_arg_count(const char *func_name);
void find_call_position(const char *text, const char *func_name,
                        size_t *line, size_t *col);
bool find_call_argument_position(const char *text, const char *func_name,
                                 ASTNode *arg_node, size_t *line,
                                 size_t *col, size_t *length);
bool find_assignment_value_position(const char *text, const char *varname,
                                    size_t occurrence, ASTNode *value_node,
                                    size_t *line, size_t *col, size_t *length);
ASTNode *find_variable_assignment(AST *ast, const char *var_name);
bool is_loop_variable(Symbol *sym, AST *ast);
const char *get_module_description(const char *module_name);
void count_references_in_node(ASTNode *node, void *ctx);
size_t count_symbol_references(const char *symbol_name, AST *ast);
bool grow_diagnostics_buffer(char **diagnostics, size_t *capacity,
                             size_t pos, size_t needed);
bool safe_strtoul(const char *str, size_t *out_value);

// Diagnostics (lsp_diagnostics.c)
ExprType infer_type_with_ast(ASTNode *node, Symbol *symbols, AST *ast);
void check_function_calls(AST *ast, const char *text, Symbol *symbols,
                          char **diagnostics, size_t *pos,
                          size_t *remaining, bool *has_diagnostics,
                          size_t *capacity);
void check_expression(ASTNode *node, const char *text, Symbol *symbols,
                      AST *ast, char **diagnostics, size_t *pos,
                      size_t *remaining, bool *has_diagnostics,
                      void *seen_vars, size_t seen_count,
                      size_t *capacity);
void check_undefined_variables(AST *ast, const char *text,
                                Symbol *symbols, char **diagnostics,
                                size_t *pos, size_t *remaining,
                                bool *has_diagnostics, size_t *capacity);
void check_unused_symbols(Symbol *symbols, const char *text, AST *ast,
                          char **diagnostics, size_t *pos,
                          size_t *remaining, bool *has_diagnostics,
                          size_t *capacity);
void check_diagnostics(const char *uri, const char *text);

// Handlers (lsp_handlers.c)
void handle_initialize(const char *id);
void handle_shutdown(const char *id);
void handle_did_open(const char *uri, const char *text);
void handle_did_change(const char *uri, const char *text);
void handle_code_action(const char *id, const char *body);
void handle_formatting(const char *id, const char *body);
void handle_document_symbols(const char *id);
void handle_workspace_symbol(const char *id, const char *body);
void handle_code_lens(const char *id, const char *body);
void handle_semantic_tokens(const char *id);

// Completion (lsp_completion.c)
void handle_completion(const char *id, const char *body);

// Hover (lsp_hover.c)
void handle_hover(const char *id, const char *body);

// Definition (lsp_definition.c)
void handle_definition(const char *id, const char *body);
void handle_references(const char *id, const char *body);
void handle_prepare_rename(const char *id, const char *body);
void handle_rename(const char *id, const char *body);

#endif // LSP_H
