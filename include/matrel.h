/*-------------------------------------------------------------------------
 *
 * matrel.h
 *	  Interface for modifying continuous view materialization tables
 *
 * Copyright (c) 2023, Tantor Labs, Inc.
 * Copyright (c) 2018, PipelineDB, Inc.
 *
 *-------------------------------------------------------------------------
 */
#ifndef CQMATVIEW_H
#define CQMATVIEW_H

#include "postgres.h"

#include "nodes/execnodes.h"
#include "postgres_ext.h"

typedef struct MatRelPartitionKey
{
	Datum value;
	Oid   typid;
} MatRelPartitionKey;

extern bool matrels_writable;
extern bool log_partitions;

#define CQ_OSREL_SUFFIX "_osrel"
#define CQ_MATREL_SUFFIX "_mrel"
#define CQ_SEQREL_SUFFIX "_seq"
#define CQ_DEFREL_SUFFIX "_def"
#define CQ_MATREL_PKEY "$pk"
#define MatRelWritable() (matrels_writable)

extern ResultRelInfo *CQMatRelOpen(Relation matrel);
extern void CQOSRelClose(ResultRelInfo *rinfo);
extern ResultRelInfo *CQOSRelOpen(Relation osrel);
extern void CQMatRelClose(ResultRelInfo *rinfo);
extern void ExecInsertCQMatRelIndexTuples(ResultRelInfo *indstate,
										  TupleTableSlot *slot, ItemPointer tid,
										  EState *estate);
extern pg_nodiscard bool ExecCQMatRelUpdate(ResultRelInfo *ri,
											TupleTableSlot *slot,
											EState *estate);
extern pg_nodiscard bool ExecCQMatRelInsert(ResultRelInfo *ri,
											TupleTableSlot *slot,
											EState *estate);
extern void DefineMatRelPartition(RangeVar *matrel,
								  MatRelPartitionKey *lower_bound,
								  Interval *duration);

extern char *CVNameToOSRelName(char *cv_name);
extern char *CVNameToMatRelName(char *cv_name);
extern char *CVNameToDefRelName(char *cv_name);
extern char *CVNameToSeqRelName(char *cv_name);

#endif
