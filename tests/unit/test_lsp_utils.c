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
