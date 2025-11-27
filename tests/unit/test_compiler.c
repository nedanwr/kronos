#include "../framework/test_framework.h"
#include "../../src/frontend/tokenizer.h"
#include "../../src/frontend/parser.h"
#include "../../src/compiler/compiler.h"

static AST *parse_string(const char *source) {
    TokenizeError *tok_err = NULL;
    TokenArray *tokens = tokenize(source, &tok_err);
    if (tok_err != NULL || tokens == NULL) {
        if (tok_err) tokenize_error_free(tok_err);
        return NULL;
    }
    
    AST *ast = parse(tokens);
    token_array_free(tokens);
    return ast;
}

TEST(compile_number) {
    AST *ast = parse_string("print 42");
    ASSERT_PTR_NOT_NULL(ast);
    
    const char *err = NULL;
    Bytecode *bytecode = compile(ast, &err);
    ASSERT_PTR_NULL(err);
    ASSERT_PTR_NOT_NULL(bytecode);
    ASSERT_TRUE(bytecode->count > 0);
    ASSERT_TRUE(bytecode->const_count > 0);
    
    bytecode_free(bytecode);
    ast_free(ast);
}

TEST(compile_assignment) {
    AST *ast = parse_string("set x to 10");
    ASSERT_PTR_NOT_NULL(ast);
    
    const char *err = NULL;
    Bytecode *bytecode = compile(ast, &err);
    ASSERT_PTR_NULL(err);
    ASSERT_PTR_NOT_NULL(bytecode);
    
    bytecode_free(bytecode);
    ast_free(ast);
}

TEST(compile_binary_operation) {
    AST *ast = parse_string("10 plus 20");
    ASSERT_PTR_NOT_NULL(ast);
    
    const char *err = NULL;
    Bytecode *bytecode = compile(ast, &err);
    ASSERT_PTR_NULL(err);
    ASSERT_PTR_NOT_NULL(bytecode);
    
    bytecode_free(bytecode);
    ast_free(ast);
}

TEST(compile_print) {
    AST *ast = parse_string("print 42");
    ASSERT_PTR_NOT_NULL(ast);
    
    const char *err = NULL;
    Bytecode *bytecode = compile(ast, &err);
    ASSERT_PTR_NULL(err);
    ASSERT_PTR_NOT_NULL(bytecode);
    
    // Should have PRINT opcode
    bool has_print = false;
    for (size_t i = 0; i < bytecode->count; i++) {
        if (bytecode->code[i] == OP_PRINT) {
            has_print = true;
            break;
        }
    }
    ASSERT_TRUE(has_print);
    
    bytecode_free(bytecode);
    ast_free(ast);
}

TEST(compile_if_statement) {
    AST *ast = parse_string("if true:\n    print 1");
    ASSERT_PTR_NOT_NULL(ast);
    
    const char *err = NULL;
    Bytecode *bytecode = compile(ast, &err);
    ASSERT_PTR_NULL(err);
    ASSERT_PTR_NOT_NULL(bytecode);
    
    // Should have JUMP_IF_FALSE opcode
    bool has_jump = false;
    for (size_t i = 0; i < bytecode->count; i++) {
        if (bytecode->code[i] == OP_JUMP_IF_FALSE) {
            has_jump = true;
            break;
        }
    }
    ASSERT_TRUE(has_jump);
    
    bytecode_free(bytecode);
    ast_free(ast);
}

TEST(compile_function_definition) {
    AST *ast = parse_string("function add with x, y:\n    return x plus y");
    ASSERT_PTR_NOT_NULL(ast);
    
    const char *err = NULL;
    Bytecode *bytecode = compile(ast, &err);
    ASSERT_PTR_NULL(err);
    ASSERT_PTR_NOT_NULL(bytecode);
    
    // Should have DEFINE_FUNC opcode
    bool has_define = false;
    for (size_t i = 0; i < bytecode->count; i++) {
        if (bytecode->code[i] == OP_DEFINE_FUNC) {
            has_define = true;
            break;
        }
    }
    ASSERT_TRUE(has_define);
    
    bytecode_free(bytecode);
    ast_free(ast);
}

TEST(compile_list_literal) {
    AST *ast = parse_string("set mylist to list 1, 2, 3");
    ASSERT_PTR_NOT_NULL(ast);
    
    const char *err = NULL;
    Bytecode *bytecode = compile(ast, &err);
    ASSERT_PTR_NULL(err);
    ASSERT_PTR_NOT_NULL(bytecode);
    
    // Should have LIST_NEW opcode
    bool has_list_new = false;
    for (size_t i = 0; i < bytecode->count; i++) {
        if (bytecode->code[i] == OP_LIST_NEW) {
            has_list_new = true;
            break;
        }
    }
    ASSERT_TRUE(has_list_new);
    
    bytecode_free(bytecode);
    ast_free(ast);
}

