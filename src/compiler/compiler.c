#include "compiler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  Bytecode *bytecode;
} Compiler;

// Helper to emit byte
static void emit_byte(Compiler *c, uint8_t byte) {
  if (c->bytecode->count >= c->bytecode->capacity) {
    c->bytecode->capacity *= 2;
    c->bytecode->code = realloc(c->bytecode->code, c->bytecode->capacity);
  }
  c->bytecode->code[c->bytecode->count++] = byte;
}

// Helper to emit two bytes
static void emit_bytes(Compiler *c, uint8_t byte1, uint8_t byte2) {
  emit_byte(c, byte1);
  emit_byte(c, byte2);
}

// Helper to add constant to pool
static size_t add_constant(Compiler *c, KronosValue *value) {
  if (c->bytecode->const_count >= c->bytecode->const_capacity) {
    c->bytecode->const_capacity *= 2;
    c->bytecode->constants =
        realloc(c->bytecode->constants,
                sizeof(KronosValue *) * c->bytecode->const_capacity);
  }
  c->bytecode->constants[c->bytecode->const_count] = value;
  return c->bytecode->const_count++;
}

// Helper to emit constant
static void emit_constant(Compiler *c, KronosValue *value) {
  size_t idx = add_constant(c, value);
  emit_bytes(c, OP_LOAD_CONST, (uint8_t)idx);
}

// Forward declaration
static void compile_node(Compiler *c, ASTNode *node);

// Compile expression
static void compile_expression(Compiler *c, ASTNode *node) {
  if (!node)
    return;

  switch (node->type) {
  case AST_NUMBER: {
    KronosValue *val = value_new_number(node->as.number);
    emit_constant(c, val);
    break;
  }

  case AST_STRING: {
    KronosValue *val =
        value_new_string(node->as.string.value, node->as.string.length);
    emit_constant(c, val);
    break;
  }

  case AST_BOOL: {
    KronosValue *val = value_new_bool(node->as.boolean);
    emit_constant(c, val);
    break;
  }

  case AST_NULL: {
    KronosValue *val = value_new_nil();
    emit_constant(c, val);
    break;
  }

  case AST_VAR: {
    KronosValue *name =
        value_new_string(node->as.var_name, strlen(node->as.var_name));
    size_t idx = add_constant(c, name);
    emit_bytes(c, OP_LOAD_VAR, (uint8_t)idx);
    break;
  }

  case AST_BINOP: {
    // Compile left and right operands
    compile_expression(c, node->as.binop.left);
    compile_expression(c, node->as.binop.right);

    // Emit operator
    switch (node->as.binop.op) {
    case BINOP_ADD:
      emit_byte(c, OP_ADD);
      break;
    case BINOP_SUB:
      emit_byte(c, OP_SUB);
      break;
    case BINOP_MUL:
      emit_byte(c, OP_MUL);
      break;
    case BINOP_DIV:
      emit_byte(c, OP_DIV);
      break;
    default:
      break;
    }
    break;
  }

  case AST_CONDITION: {
    // Compile left and right operands
    compile_expression(c, node->as.condition.left);
    compile_expression(c, node->as.condition.right);

    // Emit comparison operator
    switch (node->as.condition.op) {
    case BINOP_EQ:
      emit_byte(c, OP_EQ);
      break;
    case BINOP_NEQ:
      emit_byte(c, OP_NEQ);
      break;
    case BINOP_GT:
      emit_byte(c, OP_GT);
      break;
    case BINOP_LT:
      emit_byte(c, OP_LT);
      break;
    case BINOP_GTE:
      emit_byte(c, OP_GTE);
      break;
    case BINOP_LTE:
      emit_byte(c, OP_LTE);
      break;
    default:
      break;
    }
    break;
  }

  default:
    fprintf(stderr, "Unknown expression node type: %d\n", node->type);
    break;
  }
}

// Compile statement
static void compile_statement(Compiler *c, ASTNode *node) {
  if (!node)
    return;

  switch (node->type) {
  case AST_ASSIGN: {
    // Compile value expression
    compile_expression(c, node->as.assign.value);

    // Store in variable
    KronosValue *name =
        value_new_string(node->as.assign.name, strlen(node->as.assign.name));
    size_t idx = add_constant(c, name);
    emit_bytes(c, OP_STORE_VAR, (uint8_t)idx);

    // Emit mutability flag (1 byte: 1 for mutable, 0 for immutable)
    emit_byte(c, node->as.assign.is_mutable ? 1 : 0);

    // Emit type name if specified
    if (node->as.assign.type_name) {
      KronosValue *type_val = value_new_string(node->as.assign.type_name,
                                                strlen(node->as.assign.type_name));
      size_t type_idx = add_constant(c, type_val);
      emit_byte(c, (uint8_t)type_idx);
    } else {
      emit_byte(c, 255); // 255 means no type specified
    }
    break;
  }

  case AST_PRINT: {
    // Compile value expression
    compile_expression(c, node->as.print.value);

    // Emit print instruction
    emit_byte(c, OP_PRINT);
    break;
  }

  case AST_IF: {
    // Compile condition
    compile_expression(c, node->as.if_stmt.condition);

    // Emit jump if false (placeholder for jump offset)
    emit_byte(c, OP_JUMP_IF_FALSE);
    size_t jump_offset_pos = c->bytecode->count;
    emit_byte(c, 0); // Placeholder

    // Compile block
    for (size_t i = 0; i < node->as.if_stmt.block_size; i++) {
      compile_statement(c, node->as.if_stmt.block[i]);
    }

    // Patch jump offset
    size_t jump_target = c->bytecode->count;
    c->bytecode->code[jump_offset_pos] =
        (uint8_t)(jump_target - jump_offset_pos - 1);
    break;
  }

  case AST_FOR: {
    // Initialize loop variable
    compile_expression(c, node->as.for_stmt.start);
    KronosValue *var_name =
        value_new_string(node->as.for_stmt.var, strlen(node->as.for_stmt.var));
    size_t var_idx = add_constant(c, var_name);
    emit_bytes(c, OP_STORE_VAR, (uint8_t)var_idx);

    // Loop start
    size_t loop_start = c->bytecode->count;

    // Load loop variable and end value
    emit_bytes(c, OP_LOAD_VAR, (uint8_t)var_idx);
    compile_expression(c, node->as.for_stmt.end);

    // Check if var <= end
    emit_byte(c, OP_LTE);

    // Jump if false (exit loop)
    emit_byte(c, OP_JUMP_IF_FALSE);
    size_t exit_jump_pos = c->bytecode->count;
    emit_byte(c, 0); // Placeholder

    // Compile loop body
    for (size_t i = 0; i < node->as.for_stmt.block_size; i++) {
      compile_statement(c, node->as.for_stmt.block[i]);
    }

    // Increment loop variable
    emit_bytes(c, OP_LOAD_VAR, (uint8_t)var_idx);
    KronosValue *one = value_new_number(1);
    emit_constant(c, one);
    emit_byte(c, OP_ADD);
    emit_bytes(c, OP_STORE_VAR, (uint8_t)var_idx);

    // Jump back to loop start
    size_t offset = c->bytecode->count - loop_start + 2;
    emit_bytes(c, OP_JUMP, (uint8_t)(-offset));

    // Patch exit jump
    size_t exit_target = c->bytecode->count;
    c->bytecode->code[exit_jump_pos] =
        (uint8_t)(exit_target - exit_jump_pos - 1);
    break;
  }

  case AST_WHILE: {
    // Loop start
    size_t loop_start = c->bytecode->count;

    // Compile condition
    compile_expression(c, node->as.while_stmt.condition);

    // Jump if false (exit loop)
    emit_byte(c, OP_JUMP_IF_FALSE);
    size_t exit_jump_pos = c->bytecode->count;
    emit_byte(c, 0); // Placeholder

    // Compile loop body
    for (size_t i = 0; i < node->as.while_stmt.block_size; i++) {
      compile_statement(c, node->as.while_stmt.block[i]);
    }

    // Jump back to loop start
    size_t offset = c->bytecode->count - loop_start + 2;
    emit_bytes(c, OP_JUMP, (uint8_t)(-offset));

    // Patch exit jump
    size_t exit_target = c->bytecode->count;
    c->bytecode->code[exit_jump_pos] =
        (uint8_t)(exit_target - exit_jump_pos - 1);
    break;
  }

  case AST_FUNCTION: {
    // Store function name
    KronosValue *func_name = value_new_string(node->as.function.name,
                                              strlen(node->as.function.name));
    size_t name_idx = add_constant(c, func_name);

    // Store parameter count
    emit_bytes(c, OP_DEFINE_FUNC, (uint8_t)name_idx);
    emit_byte(c, (uint8_t)node->as.function.param_count);

    // Store parameter names as constants
    for (size_t i = 0; i < node->as.function.param_count; i++) {
      KronosValue *param_name = value_new_string(
          node->as.function.params[i], strlen(node->as.function.params[i]));
      size_t param_idx = add_constant(c, param_name);
      emit_byte(c, (uint8_t)param_idx);
    }

    // Store function body start position
    size_t body_start = c->bytecode->count + 2; // +2 for jump instruction
    emit_byte(c, (uint8_t)(body_start >> 8));   // High byte
    emit_byte(c, (uint8_t)(body_start & 0xFF)); // Low byte

    // Emit jump over function body
    emit_byte(c, OP_JUMP);
    size_t skip_body_pos = c->bytecode->count;
    emit_byte(c, 0); // Placeholder

    // Compile function body
    size_t func_body_start = c->bytecode->count;
    for (size_t i = 0; i < node->as.function.block_size; i++) {
      compile_statement(c, node->as.function.block[i]);
    }

    // Implicit return nil if no explicit return
    KronosValue *nil_val = value_new_nil();
    emit_constant(c, nil_val);
    emit_byte(c, OP_RETURN_VAL);

    // Patch jump over body
    size_t func_end = c->bytecode->count;
    c->bytecode->code[skip_body_pos] = (uint8_t)(func_end - skip_body_pos - 1);
    break;
  }

  case AST_CALL: {
    // Push arguments onto stack
    for (size_t i = 0; i < node->as.call.arg_count; i++) {
      compile_expression(c, node->as.call.args[i]);
    }

    // Push function name
    KronosValue *func_name =
        value_new_string(node->as.call.name, strlen(node->as.call.name));
    size_t name_idx = add_constant(c, func_name);

    // Call function
    emit_bytes(c, OP_CALL_FUNC, (uint8_t)name_idx);
    emit_byte(c, (uint8_t)node->as.call.arg_count);

    // Pop return value (function calls as statements don't use the return
    // value)
    emit_byte(c, OP_POP);
    break;
  }

  case AST_RETURN: {
    // Compile return value
    compile_expression(c, node->as.return_stmt.value);
    emit_byte(c, OP_RETURN_VAL);
    break;
  }

  default:
    fprintf(stderr, "Unknown statement node type: %d\n", node->type);
    break;
  }
}

// Compile AST to bytecode
Bytecode *compile(AST *ast) {
  if (!ast)
    return NULL;

  Compiler c;
  c.bytecode = malloc(sizeof(Bytecode));
  if (!c.bytecode)
    return NULL;

  // Initialize bytecode
  c.bytecode->capacity = 256;
  c.bytecode->count = 0;
  c.bytecode->code = malloc(c.bytecode->capacity);

  c.bytecode->const_capacity = 32;
  c.bytecode->const_count = 0;
  c.bytecode->constants =
      malloc(sizeof(KronosValue *) * c.bytecode->const_capacity);

  // Compile all statements
  for (size_t i = 0; i < ast->count; i++) {
    compile_statement(&c, ast->statements[i]);
  }

  // Emit halt instruction
  emit_byte(&c, OP_HALT);

  return c.bytecode;
}

// Free bytecode
void bytecode_free(Bytecode *bytecode) {
  if (!bytecode)
    return;

  // Free constants
  for (size_t i = 0; i < bytecode->const_count; i++) {
    value_release(bytecode->constants[i]);
  }
  free(bytecode->constants);

  free(bytecode->code);
  free(bytecode);
}

// Debug print bytecode
void bytecode_print(Bytecode *bytecode) {
  if (!bytecode)
    return;

  printf("=== Bytecode ===\n");
  printf("Constants (%zu):\n", bytecode->const_count);
  for (size_t i = 0; i < bytecode->const_count; i++) {
    printf("  [%zu] ", i);
    value_print(bytecode->constants[i]);
    printf("\n");
  }

  printf("\nInstructions (%zu bytes):\n", bytecode->count);
  size_t offset = 0;
  while (offset < bytecode->count) {
    printf("%04zu  ", offset);
    uint8_t instruction = bytecode->code[offset];

    switch (instruction) {
    case OP_LOAD_CONST:
      printf("LOAD_CONST %d\n", bytecode->code[offset + 1]);
      offset += 2;
      break;
    case OP_LOAD_VAR:
      printf("LOAD_VAR %d\n", bytecode->code[offset + 1]);
      offset += 2;
      break;
    case OP_STORE_VAR:
      printf("STORE_VAR %d\n", bytecode->code[offset + 1]);
      offset += 2;
      break;
    case OP_PRINT:
      printf("PRINT\n");
      offset++;
      break;
    case OP_ADD:
      printf("ADD\n");
      offset++;
      break;
    case OP_SUB:
      printf("SUB\n");
      offset++;
      break;
    case OP_MUL:
      printf("MUL\n");
      offset++;
      break;
    case OP_DIV:
      printf("DIV\n");
      offset++;
      break;
    case OP_EQ:
      printf("EQ\n");
      offset++;
      break;
    case OP_NEQ:
      printf("NEQ\n");
      offset++;
      break;
    case OP_GT:
      printf("GT\n");
      offset++;
      break;
    case OP_LT:
      printf("LT\n");
      offset++;
      break;
    case OP_GTE:
      printf("GTE\n");
      offset++;
      break;
    case OP_LTE:
      printf("LTE\n");
      offset++;
      break;
    case OP_JUMP:
      printf("JUMP %d\n", (int8_t)bytecode->code[offset + 1]);
      offset += 2;
      break;
    case OP_JUMP_IF_FALSE:
      printf("JUMP_IF_FALSE %d\n", bytecode->code[offset + 1]);
      offset += 2;
      break;
    case OP_DEFINE_FUNC: {
      printf("DEFINE_FUNC %d (param_count=%d)\n", bytecode->code[offset + 1],
             bytecode->code[offset + 2]);
      size_t param_count = bytecode->code[offset + 2];
      offset +=
          3 + param_count + 2; // Skip name, param_count, params, body_start
      break;
    }
    case OP_CALL_FUNC:
      printf("CALL_FUNC %d (arg_count=%d)\n", bytecode->code[offset + 1],
             bytecode->code[offset + 2]);
      offset += 3;
      break;
    case OP_RETURN_VAL:
      printf("RETURN_VAL\n");
      offset++;
      break;
    case OP_HALT:
      printf("HALT\n");
      offset++;
      break;
    default:
      printf("UNKNOWN (%d)\n", instruction);
      offset++;
      break;
    }
  }
}
