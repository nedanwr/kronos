#include "../../src/frontend/parser.h"
#include "../../src/frontend/tokenizer.h"
#include "../framework/test_framework.h"

TEST(parse_number_in_print) {
  TokenizeError *tok_err = NULL;
  TokenArray *tokens = tokenize("print 42", &tok_err);
  ASSERT_PTR_NULL(tok_err);
  ASSERT_PTR_NOT_NULL(tokens);

  AST *ast = parse(tokens, NULL);
  ASSERT_PTR_NOT_NULL(ast);
  ASSERT_INT_EQ(ast->count, 1);
  ASSERT_INT_EQ(ast->statements[0]->type, AST_PRINT);
  ASSERT_INT_EQ(ast->statements[0]->as.print.value->type, AST_NUMBER);
  ASSERT_DOUBLE_EQ(ast->statements[0]->as.print.value->as.number, 42.0);

  ast_free(ast);
  token_array_free(tokens);
}

TEST(parse_string_in_print) {
  TokenizeError *tok_err = NULL;
  TokenArray *tokens = tokenize("print \"hello\"", &tok_err);
  ASSERT_PTR_NULL(tok_err);
  ASSERT_PTR_NOT_NULL(tokens);

  AST *ast = parse(tokens, NULL);
  ASSERT_PTR_NOT_NULL(ast);
  ASSERT_INT_EQ(ast->count, 1);
  ASSERT_INT_EQ(ast->statements[0]->type, AST_PRINT);
  ASSERT_INT_EQ(ast->statements[0]->as.print.value->type, AST_STRING);
  ASSERT_STR_EQ(ast->statements[0]->as.print.value->as.string.value, "hello");

  ast_free(ast);
  token_array_free(tokens);
}

TEST(parse_assignment) {
  TokenizeError *tok_err = NULL;
  TokenArray *tokens = tokenize("set x to 10", &tok_err);
  ASSERT_PTR_NULL(tok_err);
  ASSERT_PTR_NOT_NULL(tokens);

  AST *ast = parse(tokens, NULL);
  ASSERT_PTR_NOT_NULL(ast);
  ASSERT_INT_EQ(ast->count, 1);
  ASSERT_INT_EQ(ast->statements[0]->type, AST_ASSIGN);
  ASSERT_STR_EQ(ast->statements[0]->as.assign.name, "x");
  ASSERT_FALSE(ast->statements[0]->as.assign.is_mutable);
  ASSERT_INT_EQ(ast->statements[0]->as.assign.value->type, AST_NUMBER);

  ast_free(ast);
  token_array_free(tokens);
}

TEST(parse_mutable_assignment) {
  TokenizeError *tok_err = NULL;
  TokenArray *tokens = tokenize("let y to 20", &tok_err);
  ASSERT_PTR_NULL(tok_err);
  ASSERT_PTR_NOT_NULL(tokens);

  AST *ast = parse(tokens, NULL);
  ASSERT_PTR_NOT_NULL(ast);
  ASSERT_INT_EQ(ast->count, 1);
  ASSERT_INT_EQ(ast->statements[0]->type, AST_ASSIGN);
  ASSERT_STR_EQ(ast->statements[0]->as.assign.name, "y");
  ASSERT_TRUE(ast->statements[0]->as.assign.is_mutable);

  ast_free(ast);
  token_array_free(tokens);
}

TEST(parse_typed_assignment) {
  TokenizeError *tok_err = NULL;
  TokenArray *tokens = tokenize("set x to 10 as number", &tok_err);
  ASSERT_PTR_NULL(tok_err);
  ASSERT_PTR_NOT_NULL(tokens);

  AST *ast = parse(tokens, NULL);
  ASSERT_PTR_NOT_NULL(ast);
  ASSERT_INT_EQ(ast->count, 1);
  ASSERT_INT_EQ(ast->statements[0]->type, AST_ASSIGN);
  ASSERT_STR_EQ(ast->statements[0]->as.assign.type_name, "number");

  ast_free(ast);
  token_array_free(tokens);
}

TEST(parse_binary_operation) {
  TokenizeError *tok_err = NULL;
  TokenArray *tokens = tokenize("set result to 10 plus 20", &tok_err);
  ASSERT_PTR_NULL(tok_err);
  ASSERT_PTR_NOT_NULL(tokens);

  AST *ast = parse(tokens, NULL);
  ASSERT_PTR_NOT_NULL(ast);
  ASSERT_INT_EQ(ast->count, 1);
  ASSERT_INT_EQ(ast->statements[0]->type, AST_ASSIGN);
  ASSERT_INT_EQ(ast->statements[0]->as.assign.value->type, AST_BINOP);
  ASSERT_INT_EQ(ast->statements[0]->as.assign.value->as.binop.op, BINOP_ADD);

  ast_free(ast);
  token_array_free(tokens);
}

TEST(parse_print_statement) {
  TokenizeError *tok_err = NULL;
  TokenArray *tokens = tokenize("print 42", &tok_err);
  ASSERT_PTR_NULL(tok_err);
  ASSERT_PTR_NOT_NULL(tokens);

  AST *ast = parse(tokens, NULL);
  ASSERT_PTR_NOT_NULL(ast);
  ASSERT_INT_EQ(ast->count, 1);
  ASSERT_INT_EQ(ast->statements[0]->type, AST_PRINT);
  ASSERT_INT_EQ(ast->statements[0]->as.print.value->type, AST_NUMBER);

  ast_free(ast);
  token_array_free(tokens);
}

TEST(parse_if_statement) {
  TokenizeError *tok_err = NULL;
  TokenArray *tokens = tokenize("if true:\n    print 1", &tok_err);
  ASSERT_PTR_NULL(tok_err);
  ASSERT_PTR_NOT_NULL(tokens);

  AST *ast = parse(tokens, NULL);
  ASSERT_PTR_NOT_NULL(ast);
  ASSERT_INT_EQ(ast->count, 1);
  ASSERT_INT_EQ(ast->statements[0]->type, AST_IF);
  ASSERT_INT_EQ(ast->statements[0]->as.if_stmt.condition->type, AST_BOOL);
  ASSERT_INT_EQ(ast->statements[0]->as.if_stmt.block_size, 1);

  ast_free(ast);
  token_array_free(tokens);
}

TEST(parse_variable_reference) {
  TokenizeError *tok_err = NULL;
  TokenArray *tokens = tokenize("print x", &tok_err);
  ASSERT_PTR_NULL(tok_err);
  ASSERT_PTR_NOT_NULL(tokens);

  AST *ast = parse(tokens, NULL);
  ASSERT_PTR_NOT_NULL(ast);
  ASSERT_INT_EQ(ast->count, 1);
  ASSERT_INT_EQ(ast->statements[0]->type, AST_PRINT);
  ASSERT_INT_EQ(ast->statements[0]->as.print.value->type, AST_VAR);
  ASSERT_STR_EQ(ast->statements[0]->as.print.value->as.var_name, "x");

  ast_free(ast);
  token_array_free(tokens);
}

TEST(parse_list_literal) {
  TokenizeError *tok_err = NULL;
  TokenArray *tokens = tokenize("set mylist to list 1, 2, 3", &tok_err);
  ASSERT_PTR_NULL(tok_err);
  ASSERT_PTR_NOT_NULL(tokens);

  AST *ast = parse(tokens, NULL);
  ASSERT_PTR_NOT_NULL(ast);
  ASSERT_INT_EQ(ast->count, 1);
  ASSERT_INT_EQ(ast->statements[0]->type, AST_ASSIGN);
  ASSERT_INT_EQ(ast->statements[0]->as.assign.value->type, AST_LIST);
  ASSERT_INT_EQ(ast->statements[0]->as.assign.value->as.list.element_count, 3);

  ast_free(ast);
  token_array_free(tokens);
}

TEST(parse_all_arithmetic_operators) {
  TokenizeError *tok_err = NULL;

  // Test SUB
  TokenArray *tokens = tokenize("set result to 10 minus 5", &tok_err);
  ASSERT_PTR_NULL(tok_err);
  AST *ast = parse(tokens, NULL);
  ASSERT_PTR_NOT_NULL(ast);
  ASSERT_INT_EQ(ast->statements[0]->as.assign.value->as.binop.op, BINOP_SUB);
  ast_free(ast);
  token_array_free(tokens);

  // Test MUL
  tokens = tokenize("set result to 10 times 5", &tok_err);
  ASSERT_PTR_NULL(tok_err);
  ast = parse(tokens, NULL);
  ASSERT_PTR_NOT_NULL(ast);
  ASSERT_INT_EQ(ast->statements[0]->as.assign.value->as.binop.op, BINOP_MUL);
  ast_free(ast);
  token_array_free(tokens);

  // Test DIV
  tokens = tokenize("set result to 10 divided by 5", &tok_err);
  ASSERT_PTR_NULL(tok_err);
  ast = parse(tokens, NULL);
  ASSERT_PTR_NOT_NULL(ast);
  ASSERT_INT_EQ(ast->statements[0]->as.assign.value->as.binop.op, BINOP_DIV);
  ast_free(ast);
  token_array_free(tokens);
}

TEST(parse_all_comparison_operators) {
  TokenizeError *tok_err = NULL;

  // Test GT
  TokenArray *tokens = tokenize("set result to 10 is greater than 5", &tok_err);
  ASSERT_PTR_NULL(tok_err);
  AST *ast = parse(tokens, NULL);
  ASSERT_PTR_NOT_NULL(ast);
  ASSERT_INT_EQ(ast->statements[0]->as.assign.value->as.binop.op, BINOP_GT);
  ast_free(ast);
  token_array_free(tokens);

  // Test LT
  tokens = tokenize("set result to 5 is less than 10", &tok_err);
  ASSERT_PTR_NULL(tok_err);
  ast = parse(tokens, NULL);
  ASSERT_PTR_NOT_NULL(ast);
  ASSERT_INT_EQ(ast->statements[0]->as.assign.value->as.binop.op, BINOP_LT);
  ast_free(ast);
  token_array_free(tokens);

  // Test GTE
  tokens = tokenize("set result to 10 is greater than or equal 5", &tok_err);
  ASSERT_PTR_NULL(tok_err);
  ast = parse(tokens, NULL);
  ASSERT_PTR_NOT_NULL(ast);
  ASSERT_INT_EQ(ast->statements[0]->as.assign.value->as.binop.op, BINOP_GTE);
  ast_free(ast);
  token_array_free(tokens);

  // Test LTE
  tokens = tokenize("set result to 5 is less than or equal 10", &tok_err);
  ASSERT_PTR_NULL(tok_err);
  ast = parse(tokens, NULL);
  ASSERT_PTR_NOT_NULL(ast);
  ASSERT_INT_EQ(ast->statements[0]->as.assign.value->as.binop.op, BINOP_LTE);
  ast_free(ast);
  token_array_free(tokens);

  // Test NEQ
  tokens = tokenize("set result to 10 is not equal 5", &tok_err);
  ASSERT_PTR_NULL(tok_err);
  ast = parse(tokens, NULL);
  ASSERT_PTR_NOT_NULL(ast);
  ASSERT_INT_EQ(ast->statements[0]->as.assign.value->as.binop.op, BINOP_NEQ);
  ast_free(ast);
  token_array_free(tokens);
}

TEST(parse_logical_operators) {
  TokenizeError *tok_err = NULL;

  // Test AND
  TokenArray *tokens = tokenize("set result to true and false", &tok_err);
  ASSERT_PTR_NULL(tok_err);
  AST *ast = parse(tokens, NULL);
  ASSERT_PTR_NOT_NULL(ast);
  ASSERT_INT_EQ(ast->statements[0]->as.assign.value->as.binop.op, BINOP_AND);
  ast_free(ast);
  token_array_free(tokens);

  // Test OR
  tokens = tokenize("set result to true or false", &tok_err);
  ASSERT_PTR_NULL(tok_err);
  ast = parse(tokens, NULL);
  ASSERT_PTR_NOT_NULL(ast);
  ASSERT_INT_EQ(ast->statements[0]->as.assign.value->as.binop.op, BINOP_OR);
  ast_free(ast);
  token_array_free(tokens);

  // Test NOT
  tokens = tokenize("set result to not true", &tok_err);
  ASSERT_PTR_NULL(tok_err);
  ast = parse(tokens, NULL);
  ASSERT_PTR_NOT_NULL(ast);
  ASSERT_INT_EQ(ast->statements[0]->as.assign.value->as.binop.op, BINOP_NOT);
  ast_free(ast);
  token_array_free(tokens);
}

TEST(parse_for_loop) {
  TokenizeError *tok_err = NULL;
  TokenArray *tokens =
      tokenize("for i in range 1 to 10:\n    print i", &tok_err);
  ASSERT_PTR_NULL(tok_err);
  ASSERT_PTR_NOT_NULL(tokens);

  AST *ast = parse(tokens, NULL);
  ASSERT_PTR_NOT_NULL(ast);
  ASSERT_INT_EQ(ast->count, 1);
  ASSERT_INT_EQ(ast->statements[0]->type, AST_FOR);
  ASSERT_STR_EQ(ast->statements[0]->as.for_stmt.var, "i");
  ASSERT_TRUE(ast->statements[0]->as.for_stmt.is_range);

  ast_free(ast);
  token_array_free(tokens);
}

TEST(parse_while_loop) {
  TokenizeError *tok_err = NULL;
  TokenArray *tokens = tokenize("while true:\n    print 1", &tok_err);
  ASSERT_PTR_NULL(tok_err);
  ASSERT_PTR_NOT_NULL(tokens);

  AST *ast = parse(tokens, NULL);
  ASSERT_PTR_NOT_NULL(ast);
  ASSERT_INT_EQ(ast->count, 1);
  ASSERT_INT_EQ(ast->statements[0]->type, AST_WHILE);
  ASSERT_INT_EQ(ast->statements[0]->as.while_stmt.condition->type, AST_BOOL);

  ast_free(ast);
  token_array_free(tokens);
}

TEST(parse_list_indexing) {
  TokenizeError *tok_err = NULL;
  TokenArray *tokens = tokenize("set item to mylist at 0", &tok_err);
  ASSERT_PTR_NULL(tok_err);
  ASSERT_PTR_NOT_NULL(tokens);

  AST *ast = parse(tokens, NULL);
  ASSERT_PTR_NOT_NULL(ast);
  ASSERT_INT_EQ(ast->count, 1);
  ASSERT_INT_EQ(ast->statements[0]->as.assign.value->type, AST_INDEX);

  ast_free(ast);
  token_array_free(tokens);
}

TEST(parse_list_slicing) {
  TokenizeError *tok_err = NULL;
  TokenArray *tokens = tokenize("set slice to mylist from 1 to 3", &tok_err);
  ASSERT_PTR_NULL(tok_err);
  ASSERT_PTR_NOT_NULL(tokens);

  AST *ast = parse(tokens, NULL);
  ASSERT_PTR_NOT_NULL(ast);
  ASSERT_INT_EQ(ast->count, 1);
  ASSERT_INT_EQ(ast->statements[0]->as.assign.value->type, AST_SLICE);

  ast_free(ast);
  token_array_free(tokens);
}

TEST(parse_function_call) {
  TokenizeError *tok_err = NULL;
  TokenArray *tokens = tokenize("call add with 10, 20", &tok_err);
  ASSERT_PTR_NULL(tok_err);
  ASSERT_PTR_NOT_NULL(tokens);

  AST *ast = parse(tokens, NULL);
  ASSERT_PTR_NOT_NULL(ast);
  ASSERT_INT_EQ(ast->count, 1);
  ASSERT_INT_EQ(ast->statements[0]->type, AST_CALL);
  ASSERT_STR_EQ(ast->statements[0]->as.call.name, "add");
  ASSERT_INT_EQ(ast->statements[0]->as.call.arg_count, 2);

  ast_free(ast);
  token_array_free(tokens);
}

TEST(parse_return_statement) {
  TokenizeError *tok_err = NULL;
  TokenArray *tokens = tokenize("return 42", &tok_err);
  ASSERT_PTR_NULL(tok_err);
  ASSERT_PTR_NOT_NULL(tokens);

  AST *ast = parse(tokens, NULL);
  ASSERT_PTR_NOT_NULL(ast);
  ASSERT_INT_EQ(ast->count, 1);
  ASSERT_INT_EQ(ast->statements[0]->type, AST_RETURN);
  ASSERT_INT_EQ(ast->statements[0]->as.return_stmt.values[0]->type, AST_NUMBER);

  ast_free(ast);
  token_array_free(tokens);
}

TEST(parse_break_statement) {
  TokenizeError *tok_err = NULL;
  TokenArray *tokens = tokenize("break", &tok_err);
  ASSERT_PTR_NULL(tok_err);
  ASSERT_PTR_NOT_NULL(tokens);

  AST *ast = parse(tokens, NULL);
  ASSERT_PTR_NOT_NULL(ast);
  ASSERT_INT_EQ(ast->count, 1);
  ASSERT_INT_EQ(ast->statements[0]->type, AST_BREAK);

  ast_free(ast);
  token_array_free(tokens);
}

TEST(parse_continue_statement) {
  TokenizeError *tok_err = NULL;
  TokenArray *tokens = tokenize("continue", &tok_err);
  ASSERT_PTR_NULL(tok_err);
  ASSERT_PTR_NOT_NULL(tokens);

  AST *ast = parse(tokens, NULL);
  ASSERT_PTR_NOT_NULL(ast);
  ASSERT_INT_EQ(ast->count, 1);
  ASSERT_INT_EQ(ast->statements[0]->type, AST_CONTINUE);

  ast_free(ast);
  token_array_free(tokens);
}

TEST(parse_import_statement) {
  TokenizeError *tok_err = NULL;
  TokenArray *tokens = tokenize("import math", &tok_err);
  ASSERT_PTR_NULL(tok_err);
  ASSERT_PTR_NOT_NULL(tokens);

  AST *ast = parse(tokens, NULL);
  ASSERT_PTR_NOT_NULL(ast);
  ASSERT_INT_EQ(ast->count, 1);
  ASSERT_INT_EQ(ast->statements[0]->type, AST_IMPORT);
  ASSERT_STR_EQ(ast->statements[0]->as.import.module_name, "math");

  ast_free(ast);
  token_array_free(tokens);
}

TEST(parse_fstring) {
  TokenizeError *tok_err = NULL;
  TokenArray *tokens = tokenize("print f\"Hello {name}\"", &tok_err);
  ASSERT_PTR_NULL(tok_err);
  ASSERT_PTR_NOT_NULL(tokens);

  AST *ast = parse(tokens, NULL);
  ASSERT_PTR_NOT_NULL(ast);
  ASSERT_INT_EQ(ast->count, 1);
  ASSERT_INT_EQ(ast->statements[0]->type, AST_PRINT);
  ASSERT_INT_EQ(ast->statements[0]->as.print.value->type, AST_FSTRING);

  ast_free(ast);
  token_array_free(tokens);
}
