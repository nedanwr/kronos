#ifndef KRONOS_COMPILER_H
#define KRONOS_COMPILER_H

#include "../core/runtime.h"
#include "../frontend/parser.h"

// Bytecode instructions
typedef enum {
  OP_LOAD_CONST,    // Load constant from pool
  OP_LOAD_VAR,      // Load variable
  OP_STORE_VAR,     // Store variable
  OP_PRINT,         // Print top of stack
  OP_ADD,           // Binary add
  OP_SUB,           // Binary subtract
  OP_MUL,           // Binary multiply
  OP_DIV,           // Binary divide
  OP_EQ,            // Equal comparison
  OP_NEQ,           // Not equal comparison
  OP_GT,            // Greater than
  OP_LT,            // Less than
  OP_GTE,           // Greater than or equal
  OP_LTE,           // Less than or equal
  OP_AND,           // Logical AND (short-circuit)
  OP_OR,            // Logical OR (short-circuit)
  OP_NOT,           // Logical NOT (unary)
  OP_JUMP,          // Unconditional jump
  OP_JUMP_IF_FALSE, // Jump if top of stack is false
  OP_DEFINE_FUNC,   // Define function
  OP_CALL_FUNC,     // Call function
  OP_RETURN_VAL,    // Return from function with value
  OP_POP,           // Pop value from stack
  OP_HALT,          // End program
} OpCode;

// Bytecode representation
typedef struct {
  uint8_t *code;
  size_t count;
  size_t capacity;

  // Constant pool
  KronosValue **constants;
  size_t const_count;
  size_t const_capacity;
} Bytecode;

/**
 * @brief Compile an abstract syntax tree (AST) into executable bytecode.
 *
 * Traverses the AST and generates a flat sequence of bytecode instructions
 * with an associated constant pool. The bytecode can then be executed by
 * the Kronos VM.
 *
 * Error handling:
 * - Returns NULL for all failure modes (invalid AST, allocation failures,
 *   or internal compiler errors such as buffer growth failures).
 * - Never calls exit() or terminates the process; all errors are returned
 *   to the caller for graceful handling.
 * - When @p out_err is non-NULL, it is set to a static, human-readable
 *   error string on failure; the pointer remains valid for the lifetime
 *   of the process and must not be freed. On success, *out_err is set to NULL.
 *
 * @param ast Abstract syntax tree to compile (may be NULL).
 * @param out_err Optional location to store an error message on failure
 * (owned by the compiler; caller must not free). Ignored when NULL.
 * @return Pointer to newly allocated Bytecode on success, NULL on error.
 * @note Caller must call bytecode_free() on the returned bytecode when done.
 * @note The AST is not modified and remains owned by the caller.
 * @note Thread-safety: NOT thread-safe. Do not compile concurrently.
 */
Bytecode *compile(AST *ast, const char **out_err);

/**
 * @brief Free a Bytecode structure and all associated resources.
 *
 * Releases the code buffer, releases all values in the constant pool,
 * and frees the Bytecode structure itself.
 *
 * @param bytecode Bytecode to free (may be NULL, in which case this is a
 * no-op).
 * @note After calling, the bytecode pointer is invalid.
 * @note Thread-safety: NOT thread-safe. Do not free concurrently.
 */
void bytecode_free(Bytecode *bytecode);

/**
 * @brief Print a human-readable representation of bytecode for debugging.
 *
 * Disassembles the bytecode instruction-by-instruction and prints to stdout.
 * Shows opcode names, operands, and references to the constant pool.
 *
 * @param bytecode Bytecode to print (may be NULL; prints "Bytecode: NULL" if
 * so).
 * @note This is a debugging/development tool, not for production use.
 * @note Output format is subject to change.
 */
void bytecode_print(Bytecode *bytecode);

#endif // KRONOS_COMPILER_H
