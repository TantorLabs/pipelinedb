/*-------------------------------------------------------------------------
 *
 * copy.h
 *	  COPY for streams
 *
 * Copyright (c) 2023, Tantor Labs, Inc.
 * Portions Copyright (c) 2018, PipelineDB, Inc.
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */
#ifndef PIPELINE_COPY_H
#define PIPELINE_COPY_H

#include "commands/copy.h"
#include "nodes/execnodes.h"
#include "nodes/parsenodes.h"
#include "parser/parse_node.h"
#include "tcop/dest.h"

extern copy_data_source_cb stream_copy_hook;

extern void DoStreamCopy(ParseState *state, const CopyStmt *stmt,
	   int stmt_location, int stmt_len,
	   uint64 *processed);

extern uint64 CopyStreamFrom(CopyFromState cstate);
extern bool NextCopyStreamFrom(CopyFromState cstate, ExprContext *econtext,
			 Datum *values, bool *nulls, Oid *tupleOid);
extern void ProcessStreamCopyOptions(ParseState *pstate, CopyFromState cstate, bool is_from, List *options);
extern Expr *expression_planner(Expr *expr);

#endif
