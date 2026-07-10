#include "optimizer.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static KronosValue *fold_binary(BinOp op, KronosValue *left,
                                KronosValue *right) {
  if (!left || !right) {
    return NULL;
  }

  if (op == BINOP_EQ || op == BINOP_NEQ) {
    bool equal = value_equals(left, right);
    return value_new_bool(op == BINOP_EQ ? equal : !equal);
  }

  if (op == BINOP_AND || op == BINOP_OR) {
    bool lhs = value_is_truthy(left);
    bool rhs = value_is_truthy(right);
    return value_new_bool(op == BINOP_AND ? lhs && rhs : lhs || rhs);
  }

  if (left->type == VAL_STRING && right->type == VAL_STRING &&
      op == BINOP_ADD) {
    if (right->as.string.length == SIZE_MAX ||
        left->as.string.length > SIZE_MAX - right->as.string.length - 1) {
      return NULL;
    }
    size_t length = left->as.string.length + right->as.string.length;
    char *joined = malloc(length + 1);
    if (!joined) {
      return NULL;
    }
    memcpy(joined, left->as.string.data, left->as.string.length);
    memcpy(joined + left->as.string.length, right->as.string.data,
           right->as.string.length);
    joined[length] = '\0';
    KronosValue *result = value_new_string(joined, length);
    free(joined);
    return result;
  }

  if (left->type != VAL_NUMBER || right->type != VAL_NUMBER) {
    return NULL;
  }

  double a = left->as.number;
  double b = right->as.number;
  switch (op) {
  case BINOP_ADD:
    return value_new_number(a + b);
  case BINOP_SUB:
    return value_new_number(a - b);
  case BINOP_MUL:
    return value_new_number(a * b);
  case BINOP_DIV:
    return b == 0.0 ? NULL : value_new_number(a / b);
  case BINOP_MOD:
    return b == 0.0 ? NULL : value_new_number(fmod(a, b));
  case BINOP_GT:
    return value_new_bool(a > b);
  case BINOP_LT:
    return value_new_bool(a < b);
  case BINOP_GTE:
    return value_new_bool(a >= b);
  case BINOP_LTE:
    return value_new_bool(a <= b);
  default:
    return NULL;
  }
}

KronosValue *optimizer_fold_constant(const ASTNode *node) {
  if (!node) {
    return NULL;
  }
  switch (node->type) {
  case AST_NUMBER:
    return value_new_number(node->as.number);
  case AST_STRING:
    return value_new_string(node->as.string.value, node->as.string.length);
  case AST_BOOL:
    return value_new_bool(node->as.boolean);
  case AST_NULL:
    return value_new_nil();
  case AST_BINOP:
    break;
  default:
    return NULL;
  }

  KronosValue *left = optimizer_fold_constant(node->as.binop.left);
  if (!left) {
    return NULL;
  }

  KronosValue *result = NULL;
  if (node->as.binop.op == BINOP_NEG && left->type == VAL_NUMBER) {
    result = value_new_number(-left->as.number);
  } else if (node->as.binop.op == BINOP_NOT) {
    result = value_new_bool(!value_is_truthy(left));
  } else if (node->as.binop.right) {
    KronosValue *right = optimizer_fold_constant(node->as.binop.right);
    if (right) {
      result = fold_binary(node->as.binop.op, left, right);
      value_release(right);
    }
  }
  value_release(left);
  return result;
}

bool optimizer_block_terminates(ASTNode *const *statements, size_t count) {
  size_t reachable = optimizer_reachable_count(statements, count);
  return reachable > 0 &&
         optimizer_statement_terminates(statements[reachable - 1]);
}

bool optimizer_statement_terminates(const ASTNode *node) {
  if (!node) {
    return false;
  }
  if (node->type == AST_RETURN || node->type == AST_RAISE ||
      node->type == AST_BREAK || node->type == AST_CONTINUE) {
    return true;
  }
  if (node->type != AST_IF || node->as.if_stmt.else_block_size == 0 ||
      !optimizer_block_terminates(node->as.if_stmt.block,
                                  node->as.if_stmt.block_size) ||
      !optimizer_block_terminates(node->as.if_stmt.else_block,
                                  node->as.if_stmt.else_block_size)) {
    return false;
  }
  for (size_t i = 0; i < node->as.if_stmt.else_if_count; i++) {
    if (!optimizer_block_terminates(
            node->as.if_stmt.else_if_blocks[i],
            node->as.if_stmt.else_if_block_sizes[i])) {
      return false;
    }
  }
  return true;
}

size_t optimizer_reachable_count(ASTNode *const *statements, size_t count) {
  if (!statements) {
    return 0;
  }
  for (size_t i = 0; i < count; i++) {
    if (optimizer_statement_terminates(statements[i])) {
      return i + 1;
    }
  }
  return count;
}
