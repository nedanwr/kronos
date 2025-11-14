#ifndef KRONOS_COMPILER_H
#define KRONOS_COMPILER_H

#include "../frontend/parser.h"
#include "../core/runtime.h"

// Bytecode instructions
typedef enum {
    OP_LOAD_CONST,      // Load constant from pool
    OP_LOAD_VAR,        // Load variable
    OP_STORE_VAR,       // Store variable
    OP_PRINT,           // Print top of stack
    OP_ADD,             // Binary add
    OP_SUB,             // Binary subtract
    OP_MUL,             // Binary multiply
    OP_DIV,             // Binary divide
    OP_EQ,              // Equal comparison
    OP_NEQ,             // Not equal comparison
    OP_GT,              // Greater than
    OP_LT,              // Less than
    OP_GTE,             // Greater than or equal
    OP_LTE,             // Less than or equal
    OP_JUMP,            // Unconditional jump
    OP_JUMP_IF_FALSE,   // Jump if top of stack is false
    OP_CALL,            // Call function
    OP_RETURN,          // Return from function
    OP_POP,             // Pop value from stack
    OP_HALT,            // End program
} OpCode;

// Bytecode representation
typedef struct {
    uint8_t* code;
    size_t count;
    size_t capacity;
    
    // Constant pool
    KronosValue** constants;
    size_t const_count;
    size_t const_capacity;
} Bytecode;

// Compile AST to bytecode
Bytecode* compile(AST* ast);
void bytecode_free(Bytecode* bytecode);

// Debug
void bytecode_print(Bytecode* bytecode);

#endif // KRONOS_COMPILER_H

