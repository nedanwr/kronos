#include "../framework/test_framework.h"
#include "../../src/frontend/tokenizer.h"
#include "../../src/frontend/parser.h"
#include "../../src/compiler/compiler.h"
#include <stdio.h>
#include <string.h>

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

TEST(compile_loop_large_offset_break) {
    // Regression test for UAF bug in patch_pending_jumps.
    // Create a loop with a large body that triggers offset >255,
    // then break/continue to hit pop_loop safely.
    // The test verifies that patch_pending_jumps handles early returns
    // correctly without leaving dangling pointers.
    
    // Build a loop with many statements to create a large body
    // Each statement generates several bytes, so ~100 statements should
    // create enough bytecode to trigger offset >255
    const char *loop_start = "while true:\n";
    const char *loop_end = "    break\n";
    
    // Build large loop body with many assignments
    char large_loop[8192] = {0};
    strcat(large_loop, loop_start);
    // Add many statements to create large bytecode
    for (int i = 0; i < 150; i++) {
        char stmt[64];
        snprintf(stmt, sizeof(stmt), "    set x%d to %d\n", i, i);
        strcat(large_loop, stmt);
    }
    strcat(large_loop, loop_end);
    
    AST *ast = parse_string(large_loop);
    ASSERT_PTR_NOT_NULL(ast);
    
    const char *err = NULL;
    Bytecode *bytecode = compile(ast, &err);
    // Should either compile successfully (if offset fits) or error with "break jump offset too large"
    // Either way, should not crash with UAF
    if (err) {
        // Expected: offset too large error
        ASSERT_TRUE(strstr(err, "offset too large") != NULL || 
                   strstr(err, "break jump") != NULL);
    }
    
    if (bytecode) {
        bytecode_free(bytecode);
    }
    ast_free(ast);
}

TEST(compile_loop_large_offset_continue) {
    // Similar test for continue statement
    const char *loop_start = "while true:\n";
    const char *loop_end = "    continue\n";
    
    char large_loop[8192] = {0};
    strcat(large_loop, loop_start);
    for (int i = 0; i < 150; i++) {
        char stmt[64];
        snprintf(stmt, sizeof(stmt), "    set x%d to %d\n", i, i);
        strcat(large_loop, stmt);
    }
    strcat(large_loop, loop_end);
    
    AST *ast = parse_string(large_loop);
    ASSERT_PTR_NOT_NULL(ast);
    
    const char *err = NULL;
    Bytecode *bytecode = compile(ast, &err);
    // Should either compile successfully or error safely without UAF
    if (err) {
        ASSERT_TRUE(strstr(err, "offset too large") != NULL || 
                   strstr(err, "continue jump") != NULL);
    }
    
    if (bytecode) {
        bytecode_free(bytecode);
    }
    ast_free(ast);
}

