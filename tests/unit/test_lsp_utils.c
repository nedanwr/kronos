#include "../../src/lsp/lsp.h"
#include "../framework/test_framework.h"

// lsp_utils.c references this global from lsp.h.
DocumentState *g_doc = NULL;

TEST(process_symbols_function_with_null_params_array) {
  ASTNode function_node = {0};
  function_node.type = AST_FUNCTION;
  function_node.as.function.name = "demo";
  function_node.as.function.params = NULL;
  function_node.as.function.param_count = 2;
  function_node.as.function.required_param_count = 2;
  function_node.as.function.has_variadic = false;

  ASTNode *statements[] = {&function_node};

  Symbol *symbols = NULL;
  Symbol **tail = &symbols;

  process_statements_for_symbols(statements, 1, &tail, &symbols);

  ASSERT_PTR_NOT_NULL(symbols);
  ASSERT_INT_EQ(symbols->type, SYMBOL_FUNCTION);
  ASSERT_STR_EQ(symbols->name, "demo");
  ASSERT_PTR_NULL(symbols->next);

  free_symbols(symbols);
}

TEST(process_symbols_function_skips_null_param_entries) {
  char *params[] = {NULL, "value"};

  ASTNode function_node = {0};
  function_node.type = AST_FUNCTION;
  function_node.as.function.name = "demo";
  function_node.as.function.params = params;
  function_node.as.function.param_count = 2;
  function_node.as.function.required_param_count = 2;
  function_node.as.function.has_variadic = false;

  ASTNode *statements[] = {&function_node};

  Symbol *symbols = NULL;
  Symbol **tail = &symbols;

  process_statements_for_symbols(statements, 1, &tail, &symbols);

  ASSERT_PTR_NOT_NULL(symbols);
  ASSERT_INT_EQ(symbols->type, SYMBOL_FUNCTION);
  ASSERT_PTR_NOT_NULL(symbols->param_names);
  ASSERT_PTR_NULL(symbols->param_names[0]);
  ASSERT_STR_EQ(symbols->param_names[1], "value");

  Symbol *param_symbol = symbols->next;
  ASSERT_PTR_NOT_NULL(param_symbol);
  ASSERT_INT_EQ(param_symbol->type, SYMBOL_PARAMETER);
  ASSERT_STR_EQ(param_symbol->name, "value");
  ASSERT_PTR_NULL(param_symbol->next);

  free_symbols(symbols);
}

TEST(is_loop_variable_respects_scope_boundaries) {
  ASTNode global_assign = {0};
  global_assign.type = AST_ASSIGN;
  global_assign.line = 1;
  global_assign.column = 1;
  global_assign.as.assign.name = "i";

  ASTNode loop_node = {0};
  loop_node.type = AST_FOR;
  loop_node.line = 3;
  loop_node.column = 3;
  loop_node.as.for_stmt.var = "i";

  ASTNode *function_block[] = {&loop_node};
  ASTNode function_node = {0};
  function_node.type = AST_FUNCTION;
  function_node.line = 2;
  function_node.column = 1;
  function_node.as.function.name = "worker";
  function_node.as.function.block = function_block;
  function_node.as.function.block_size = 1;

  ASTNode *statements[] = {&global_assign, &function_node};
  AST ast = {statements, 2, 2};

  Symbol global_i = {0};
  global_i.name = "i";
  global_i.type = SYMBOL_VARIABLE;
  global_i.line = 1;
  global_i.column = 1;

  Symbol loop_i = {0};
  loop_i.name = "i";
  loop_i.type = SYMBOL_VARIABLE;
  loop_i.line = 3;
  loop_i.column = 3;

  ASSERT_FALSE(is_loop_variable(&global_i, &ast));
  ASSERT_TRUE(is_loop_variable(&loop_i, &ast));
}
