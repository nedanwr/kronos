#ifndef KRONOS_OPTIMIZER_H
#define KRONOS_OPTIMIZER_H

#include "../frontend/parser.h"

/*
 * Evaluate a side-effect-free literal expression. The returned value is owned
 * by the caller. Expressions that could raise at runtime are not folded.
 */
KronosValue *optimizer_fold_constant(const ASTNode *node);

/* Return the reachable prefix length of a statement block. */
size_t optimizer_reachable_count(ASTNode *const *statements, size_t count);

/* True when control cannot continue past this statement in the same block. */
bool optimizer_statement_terminates(const ASTNode *node);

/* True when the reachable portion of a block always terminates control flow. */
bool optimizer_block_terminates(ASTNode *const *statements, size_t count);

#endif
