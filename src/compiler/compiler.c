#include "compiler.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  Bytecode *bytecode;
  const char *error_message;
} Compiler;

static inline bool compiler_has_error(const Compiler *c) {
  return c && c->error_message;
}

static void compiler_set_error(Compiler *c, const char *message) {
  if (!c || c->error_message)
    return;
  c->error_message = message;
}

// Helper to emit byte
static void emit_byte(Compiler *c, uint8_t byte) {
  if (!c || compiler_has_error(c))
    return;

  if (c->bytecode->count >= c->bytecode->capacity) {
    // Determine new capacity (minimum 256 if starting from 0)
    size_t new_capacity;
    if (c->bytecode->capacity == 0) {
      new_capacity = 256; // Sane minimum initial capacity for bytecode
    } else {
      // Check for overflow before doubling capacity
      if (c->bytecode->capacity > SIZE_MAX / 2) {
        compiler_set_error(c, "Bytecode capacity overflow");
        return;
      }
      new_capacity = c->bytecode->capacity * 2;
    }

    // Calculate byte size safely (overflow already checked above)
    size_t new_size = new_capacity * sizeof(uint8_t);

    // Attempt reallocation using temporary pointer
    uint8_t *new_code = realloc(c->bytecode->code, new_size);

    if (!new_code) {
      compiler_set_error(c, "Failed to allocate memory for bytecode");
      return;
    }

    // Only update after successful reallocation
    c->bytecode->code = new_code;
    c->bytecode->capacity = new_capacity;
  }
  if (compiler_has_error(c))
    return;

  c->bytecode->code[c->bytecode->count++] = byte;
}

// Helper to emit two bytes
static void emit_bytes(Compiler *c, uint8_t byte1, uint8_t byte2) {
  emit_byte(c, byte1);
  emit_byte(c, byte2);
}

// Helper to emit 16-bit value (big-endian)
static void emit_uint16(Compiler *c, uint16_t value) {
  emit_byte(c, (uint8_t)((value >> 8) & 0xFF));
  emit_byte(c, (uint8_t)(value & 0xFF));
}

// Helper to add constant to pool
static size_t add_constant(Compiler *c, KronosValue *value) {
  if (!c || compiler_has_error(c))
    return SIZE_MAX;

  if (c->bytecode->const_count >= c->bytecode->const_capacity) {
    // Determine new capacity (minimum 8 if starting from 0)
    size_t new_capacity;
    if (c->bytecode->const_capacity == 0) {
      new_capacity = 8; // Sane minimum initial capacity
    } else {
      // Check for overflow before doubling capacity
      if (c->bytecode->const_capacity > SIZE_MAX / 2 / sizeof(KronosValue *)) {
        compiler_set_error(c, "Constant pool capacity overflow");
        return SIZE_MAX;
      }
      new_capacity = c->bytecode->const_capacity * 2;
    }

    // Calculate byte size safely (overflow already checked above)
    size_t new_size = new_capacity * sizeof(KronosValue *);

    // Attempt reallocation using temporary pointer
    KronosValue **new_constants = realloc(c->bytecode->constants, new_size);

    if (!new_constants) {
      compiler_set_error(c, "Failed to allocate memory for constant pool");
      return SIZE_MAX;
    }

    // Only update after successful reallocation
    c->bytecode->constants = new_constants;
    c->bytecode->const_capacity = new_capacity;
  }
  if (compiler_has_error(c))
    return SIZE_MAX;

  c->bytecode->constants[c->bytecode->const_count] = value;
  return c->bytecode->const_count++;
}

// Helper to emit constant
static void emit_constant(Compiler *c, KronosValue *value) {
  if (!c) {
    if (value)
      value_release(value);
    return;
  }

  if (compiler_has_error(c)) {
    if (value)
      value_release(value);
    return;
  }

  size_t idx = add_constant(c, value);

  if (idx == SIZE_MAX) {
    if (value)
      value_release(value);
    return;
  }

  if (idx > UINT16_MAX) {
    compiler_set_error(c, "Too many constants (limit 65535)");
    return;
  }
  emit_byte(c, OP_LOAD_CONST);
  emit_uint16(c, (uint16_t)idx);
}

// Compile expression
static void compile_expression(Compiler *c, ASTNode *node) {
  if (!node || compiler_has_error(c))
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
    if (idx == SIZE_MAX) {
      value_release(name);
      return;
    }
    if (idx > UINT16_MAX) {
      compiler_set_error(c, "Too many constants (limit 65535)");
      return;
    }
    emit_byte(c, OP_LOAD_VAR);
    emit_uint16(c, (uint16_t)idx);
    break;
  }

  case AST_BINOP: {
    // Compile left and right operands
    compile_expression(c, node->as.binop.left);
    if (compiler_has_error(c))
      return;
    compile_expression(c, node->as.binop.right);
    if (compiler_has_error(c))
      return;

    // Emit operator (arithmetic or comparison)
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
    default: {
      // Report error for unsupported/unknown binary operator
      static char error_buf[128];
      snprintf(error_buf, sizeof(error_buf),
               "Unsupported binary operator (enum value: %d)",
               node->as.binop.op);
      compiler_set_error(c, error_buf);
      return; // Return early to avoid stack imbalance
    }
    }
    break;
  }

  default:
    compiler_set_error(c, "Unknown expression node type");
    break;
  }
}

// Compile statement
static void compile_statement(Compiler *c, ASTNode *node) {
  if (!node || compiler_has_error(c))
    return;

  switch (node->type) {
  case AST_ASSIGN: {
    // Compile value expression
    compile_expression(c, node->as.assign.value);
    if (compiler_has_error(c))
      return;

    // Store in variable
    KronosValue *name =
        value_new_string(node->as.assign.name, strlen(node->as.assign.name));
    size_t idx = add_constant(c, name);
    if (idx == SIZE_MAX) {
      value_release(name);
      return;
    }
    if (idx > UINT16_MAX) {
      compiler_set_error(c, "Too many constants (limit 65535)");
      return;
    }
    emit_byte(c, OP_STORE_VAR);
    emit_uint16(c, (uint16_t)idx);
    if (compiler_has_error(c))
      return;

    // Emit mutability flag (1 byte: 1 for mutable, 0 for immutable)
    emit_byte(c, node->as.assign.is_mutable ? 1 : 0);
    if (compiler_has_error(c))
      return;

    // Emit type name if specified
    if (node->as.assign.type_name) {
      emit_byte(c, 1);
      KronosValue *type_val = value_new_string(
          node->as.assign.type_name, strlen(node->as.assign.type_name));
      size_t type_idx = add_constant(c, type_val);
      if (type_idx == SIZE_MAX) {
        value_release(type_val);
        return;
      }
      if (type_idx > UINT16_MAX) {
        compiler_set_error(c, "Too many constants (limit 65535)");
        return;
      }
      emit_uint16(c, (uint16_t)type_idx);
    } else {
      emit_byte(c, 0); // no type specified
    }
    if (compiler_has_error(c))
      return;
    break;
  }

  case AST_PRINT: {
    // Compile value expression
    compile_expression(c, node->as.print.value);
    if (compiler_has_error(c))
      return;

    // Emit print instruction
    emit_byte(c, OP_PRINT);
    if (compiler_has_error(c))
      return;
    break;
  }

  case AST_IF: {
    // Compile condition
    compile_expression(c, node->as.if_stmt.condition);
    if (compiler_has_error(c))
      return;

    // Emit jump if false (placeholder for jump offset)
    emit_byte(c, OP_JUMP_IF_FALSE);
    size_t jump_offset_pos = c->bytecode->count;
    emit_byte(c, 0); // Placeholder
    if (compiler_has_error(c))
      return;

    // Compile block
    for (size_t i = 0; i < node->as.if_stmt.block_size; i++) {
      compile_statement(c, node->as.if_stmt.block[i]);
      if (compiler_has_error(c))
        return;
    }

    // Patch jump offset
    if (compiler_has_error(c))
      return;
    size_t jump_target = c->bytecode->count;
    c->bytecode->code[jump_offset_pos] =
        (uint8_t)(jump_target - jump_offset_pos - 1);
    break;
  }

  case AST_FOR: {
    // Initialize loop variable
    compile_expression(c, node->as.for_stmt.start);
    if (compiler_has_error(c))
      return;
    KronosValue *var_name =
        value_new_string(node->as.for_stmt.var, strlen(node->as.for_stmt.var));
    size_t var_idx = add_constant(c, var_name);
    if (var_idx == SIZE_MAX) {
      value_release(var_name);
      return;
    }
    if (var_idx > UINT16_MAX) {
      compiler_set_error(c, "Too many constants (limit 65535)");
      return;
    }
    emit_byte(c, OP_STORE_VAR);
    emit_uint16(c, (uint16_t)var_idx);
    emit_byte(c, 1); // for loop variables default mutable
    emit_byte(c, 0); // no type annotation
    if (compiler_has_error(c))
      return;

    // Loop start
    size_t loop_start = c->bytecode->count;

    // Load loop variable and end value
    emit_byte(c, OP_LOAD_VAR);
    emit_uint16(c, (uint16_t)var_idx);
    if (compiler_has_error(c))
      return;
    compile_expression(c, node->as.for_stmt.end);
    if (compiler_has_error(c))
      return;

    // Check if var <= end
    emit_byte(c, OP_LTE);
    if (compiler_has_error(c))
      return;

    // Jump if false (exit loop)
    emit_byte(c, OP_JUMP_IF_FALSE);
    size_t exit_jump_pos = c->bytecode->count;
    emit_byte(c, 0); // Placeholder
    if (compiler_has_error(c))
      return;

    // Compile loop body
    for (size_t i = 0; i < node->as.for_stmt.block_size; i++) {
      compile_statement(c, node->as.for_stmt.block[i]);
      if (compiler_has_error(c))
        return;
    }

    // Increment loop variable
    emit_byte(c, OP_LOAD_VAR);
    emit_uint16(c, (uint16_t)var_idx);
    if (compiler_has_error(c))
      return;
    KronosValue *one = value_new_number(1);
    emit_constant(c, one);
    if (compiler_has_error(c))
      return;
    emit_byte(c, OP_ADD);
    emit_byte(c, OP_STORE_VAR);
    emit_uint16(c, (uint16_t)var_idx);
    emit_byte(c, 1);
    emit_byte(c, 0);
    if (compiler_has_error(c))
      return;

    // Jump back to loop start
    size_t offset = c->bytecode->count - loop_start + 2;
    emit_bytes(c, OP_JUMP, (uint8_t)(-offset));
    if (compiler_has_error(c))
      return;

    // Patch exit jump
    if (compiler_has_error(c))
      return;
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
    if (compiler_has_error(c))
      return;

    // Jump if false (exit loop)
    emit_byte(c, OP_JUMP_IF_FALSE);
    size_t exit_jump_pos = c->bytecode->count;
    emit_byte(c, 0); // Placeholder
    if (compiler_has_error(c))
      return;

    // Compile loop body
    for (size_t i = 0; i < node->as.while_stmt.block_size; i++) {
      compile_statement(c, node->as.while_stmt.block[i]);
      if (compiler_has_error(c))
        return;
    }

    // Jump back to loop start
    size_t offset = c->bytecode->count - loop_start + 2;
    emit_bytes(c, OP_JUMP, (uint8_t)(-offset));
    if (compiler_has_error(c))
      return;

    // Patch exit jump
    if (compiler_has_error(c))
      return;
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
    if (name_idx == SIZE_MAX) {
      value_release(func_name);
      return;
    }

    // Store parameter count
    if (name_idx > UINT16_MAX) {
      compiler_set_error(c, "Too many constants (limit 65535)");
      return;
    }
    emit_byte(c, OP_DEFINE_FUNC);
    emit_uint16(c, (uint16_t)name_idx);
    if (compiler_has_error(c))
      return;
    emit_byte(c, (uint8_t)node->as.function.param_count);
    if (compiler_has_error(c))
      return;

    // Store parameter names as constants
    for (size_t i = 0; i < node->as.function.param_count; i++) {
      KronosValue *param_name = value_new_string(
          node->as.function.params[i], strlen(node->as.function.params[i]));
      size_t param_idx = add_constant(c, param_name);
      if (param_idx == SIZE_MAX) {
        value_release(param_name);
        return;
      }
      if (param_idx > UINT16_MAX) {
        compiler_set_error(c, "Too many constants (limit 65535)");
        return;
      }
      emit_uint16(c, (uint16_t)param_idx);
      if (compiler_has_error(c))
        return;
    }

    // Store function body start position
    size_t body_start = c->bytecode->count + 2; // +2 for jump instruction
    emit_byte(c, (uint8_t)(body_start >> 8));   // High byte
    emit_byte(c, (uint8_t)(body_start & 0xFF)); // Low byte
    if (compiler_has_error(c))
      return;

    // Emit jump over function body
    emit_byte(c, OP_JUMP);
    size_t skip_body_pos = c->bytecode->count;
    emit_byte(c, 0); // Placeholder
    if (compiler_has_error(c))
      return;

    // Compile function body
    size_t func_body_start = c->bytecode->count;
    for (size_t i = 0; i < node->as.function.block_size; i++) {
      compile_statement(c, node->as.function.block[i]);
      if (compiler_has_error(c))
        return;
    }

    // Implicit return nil if no explicit return
    KronosValue *nil_val = value_new_nil();
    emit_constant(c, nil_val);
    if (compiler_has_error(c))
      return;
    emit_byte(c, OP_RETURN_VAL);
    if (compiler_has_error(c))
      return;

    // Patch jump over body
    if (compiler_has_error(c))
      return;
    size_t func_end = c->bytecode->count;
    c->bytecode->code[skip_body_pos] = (uint8_t)(func_end - skip_body_pos - 1);
    break;
  }

  case AST_CALL: {
    // Push arguments onto stack
    for (size_t i = 0; i < node->as.call.arg_count; i++) {
      compile_expression(c, node->as.call.args[i]);
      if (compiler_has_error(c))
        return;
    }

    // Push function name
    KronosValue *func_name =
        value_new_string(node->as.call.name, strlen(node->as.call.name));
    size_t name_idx = add_constant(c, func_name);
    if (name_idx == SIZE_MAX) {
      value_release(func_name);
      return;
    }

    // Call function
    if (name_idx > UINT16_MAX) {
      compiler_set_error(c, "Too many constants (limit 65535)");
      return;
    }
    emit_byte(c, OP_CALL_FUNC);
    emit_uint16(c, (uint16_t)name_idx);
    if (compiler_has_error(c))
      return;
    emit_byte(c, (uint8_t)node->as.call.arg_count);
    if (compiler_has_error(c))
      return;

    // For built-in functions, print the result instead of discarding it
    // For user-defined functions, pop the return value (function calls as
    // statements don't use the return value)
    const char *func_name_str = node->as.call.name;
    if (strcmp(func_name_str, "add") == 0 || strcmp(func_name_str, "subtract") == 0 ||
        strcmp(func_name_str, "multiply") == 0 || strcmp(func_name_str, "divide") == 0) {
      emit_byte(c, OP_PRINT);
    } else {
      emit_byte(c, OP_POP);
    }
    if (compiler_has_error(c))
      return;
    break;
  }

  case AST_RETURN: {
    // Compile return value
    compile_expression(c, node->as.return_stmt.value);
    if (compiler_has_error(c))
      return;
    emit_byte(c, OP_RETURN_VAL);
    if (compiler_has_error(c))
      return;
    break;
  }

  default:
    compiler_set_error(c, "Unknown statement node type");
    break;
  }
}

// Compile AST to bytecode
Bytecode *compile(AST *ast, const char **out_err) {
  if (out_err)
    *out_err = NULL;

  if (!ast) {
    if (out_err)
      *out_err = "Invalid AST (NULL)";
    return NULL;
  }

  Compiler c;
  c.error_message = NULL;
  c.bytecode = malloc(sizeof(Bytecode));
  if (!c.bytecode) {
    if (out_err)
      *out_err = "Failed to allocate bytecode structure";
    return NULL;
  }

  // Initialize bytecode
  c.bytecode->capacity = 256;
  c.bytecode->count = 0;
  c.bytecode->code = malloc(c.bytecode->capacity);
  if (!c.bytecode->code) {
    free(c.bytecode);
    if (out_err)
      *out_err = "Failed to allocate bytecode buffer";
    return NULL;
  }

  c.bytecode->const_capacity = 32;
  c.bytecode->const_count = 0;
  c.bytecode->constants =
      malloc(sizeof(KronosValue *) * c.bytecode->const_capacity);
  if (!c.bytecode->constants) {
    free(c.bytecode->code);
    free(c.bytecode);
    if (out_err)
      *out_err = "Failed to allocate constant pool";
    return NULL;
  }

  // Compile all statements
  for (size_t i = 0; i < ast->count && !compiler_has_error(&c); i++) {
    compile_statement(&c, ast->statements[i]);
  }

  // Emit halt instruction if no errors occurred
  if (!compiler_has_error(&c)) {
    emit_byte(&c, OP_HALT);
  }

  if (compiler_has_error(&c)) {
    if (out_err)
      *out_err = c.error_message ? c.error_message : "Compilation failed";
    bytecode_free(c.bytecode);
    return NULL;
  }

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
  if (!bytecode) {
    printf("Bytecode: NULL\n");
    return;
  }

  printf("=== Bytecode ===\n");
  printf("Constants (%zu):\n", bytecode->const_count);
  for (size_t i = 0; i < bytecode->const_count; i++) {
    printf("  [%zu] ", i);
    value_fprint(stdout, bytecode->constants[i]);
    printf("\n");
  }

  printf("\nInstructions (%zu bytes):\n", bytecode->count);
  size_t offset = 0;
  while (offset < bytecode->count) {
    printf("%04zu  ", offset);
    uint8_t instruction = bytecode->code[offset];

    switch (instruction) {
    case OP_LOAD_CONST: {
      uint16_t idx = (uint16_t)(bytecode->code[offset + 1] << 8 |
                                bytecode->code[offset + 2]);
      printf("LOAD_CONST %u\n", idx);
      offset += 3;
      break;
    }
    case OP_LOAD_VAR: {
      uint16_t idx = (uint16_t)(bytecode->code[offset + 1] << 8 |
                                bytecode->code[offset + 2]);
      printf("LOAD_VAR %u\n", idx);
      offset += 3;
      break;
    }
    case OP_STORE_VAR: {
      uint16_t idx = (uint16_t)(bytecode->code[offset + 1] << 8 |
                                bytecode->code[offset + 2]);
      uint8_t is_mutable = bytecode->code[offset + 3];
      uint8_t has_type = bytecode->code[offset + 4];
      printf("STORE_VAR name=%u mutable=%u", idx, is_mutable);
      offset += 5;
      if (has_type) {
        uint16_t type_idx = (uint16_t)(bytecode->code[offset] << 8 |
                                       bytecode->code[offset + 1]);
        printf(" type=%u", type_idx);
        offset += 2;
      }
      printf("\n");
      break;
    }
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
      uint16_t name_idx = (uint16_t)(bytecode->code[offset + 1] << 8 |
                                     bytecode->code[offset + 2]);
      uint8_t param_count = bytecode->code[offset + 3];
      printf("DEFINE_FUNC %u (param_count=%u)\n", name_idx, param_count);
      size_t skip = 4 + (size_t)param_count * 2 + 2 + 2;
      offset += skip;
      break;
    }
    case OP_CALL_FUNC: {
      uint16_t name_idx = (uint16_t)(bytecode->code[offset + 1] << 8 |
                                     bytecode->code[offset + 2]);
      uint8_t arg_count = bytecode->code[offset + 3];
      printf("CALL_FUNC %u (arg_count=%u)\n", name_idx, arg_count);
      offset += 4;
      break;
    }
    case OP_RETURN_VAL:
      printf("RETURN_VAL\n");
      offset++;
      break;
    case OP_POP:
      printf("POP\n");
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
