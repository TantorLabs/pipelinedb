/*-------------------------------------------------------------------------
 *
 * mutator.h
 *	  Interface for mutating parse trees
 *
 * Copyright (c) 2023, Tantor Labs, Inc.
 * Copyright (c) 2018, PipelineDB, Inc.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PIPELINE_MUTATOR_H
#define PIPELINE_MUTATOR_H

#include "nodes/parsenodes.h"

typedef Node *(*RawExprMutator) (Node *node, void *context);

extern Node * raw_expression_tree_mutator(Node *node, RawExprMutator walker,
									   void *context);

#endif
