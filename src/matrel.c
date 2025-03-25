/*-------------------------------------------------------------------------
 *
 * matrel.c
 *	  Support for interacting with CV materialization tables and their indexes
 *
 * Copyright (c) 2018, PipelineDB, Inc.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/reloptions.h"
#include "access/htup_details.h"
#include "access/xact.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/toasting.h"
#include "commands/tablecmds.h"
#include "executor/executor.h"
#include "matrel.h"
#include "miscutils.h"
#include "pipeline_query.h"
#include "nodes/execnodes.h"
#include "parser/parse_utilcmd.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/palloc.h"
#include "utils/rel.h"
#include "utils/syscache.h"

bool matrels_writable;
bool log_partitions;

/*
 * print_timestamp
 *
 * Convert a timestamp / timestamptz value to the character
 * representation
 */
static char *
print_timestamp(MatRelPartitionKey *ts)
{
	Datum (*out_func)(PG_FUNCTION_ARGS);

	Assert(ts->typid == TIMESTAMPOID || ts->typid == TIMESTAMPTZOID);

	out_func = (ts->typid == TIMESTAMPOID) ? timestamp_out :
		timestamptz_out;

	return (char *) DirectFunctionCall1(out_func, ts->value);
}

/*
 * format_timestamp
 *
 * Format a timestamp / timestamptz value for partition name
 */
static char *
format_timestamp(MatRelPartitionKey *ts)
{
	static char * const fmt = "YYYYMMDDHH24MISS";
	Datum (*to_char_func)(PG_FUNCTION_ARGS);

	Assert(ts->typid == TIMESTAMPOID || ts->typid == TIMESTAMPTZOID);

	to_char_func = (ts->typid == TIMESTAMPOID) ? timestamp_to_char :
		timestamptz_to_char;

	return TextDatumGetCString(DirectFunctionCall2(to_char_func, ts->value,
								(Datum) CStringGetTextDatum(fmt)));
}

/*
 * get_partition_name
 *
 * Generate a partition name given its lower and upper bounds
 */
static char *
get_partition_name(Oid matrelid, MatRelPartitionKey *lower_bound,
				   MatRelPartitionKey *upper_bound)
{
	StringInfoData buf;

	Assert(upper_bound->typid == lower_bound->typid);

	initStringInfo(&buf);
	appendStringInfo(&buf, "mrel_%u_", matrelid);

	appendStringInfo(&buf, "%s_", format_timestamp(lower_bound));
	appendStringInfo(&buf, "%s", format_timestamp(upper_bound));

	return buf.data;
}

/*
 * get_partition_upper_bound
 *
 * Given a partition lower bound, compute the corresponding upper bound
 */
static MatRelPartitionKey
get_partition_upper_bound(MatRelPartitionKey *lower_bound, Interval *i)
{
	MatRelPartitionKey result;

	Assert(lower_bound->typid == TIMESTAMPOID ||
		   lower_bound->typid == TIMESTAMPTZOID);

	result.value =
		DirectFunctionCall2((lower_bound->typid == TIMESTAMPOID) ?
							timestamp_pl_interval : timestamptz_pl_interval,
							lower_bound->value, (Datum) i);
	result.typid = lower_bound->typid;

	return result;
}

/*
 * DefineMatRelPartition
 *
 * Given a matrel and a partition lower bound, create a partition for [lower_bound, upper_bound)
 */
void
DefineMatRelPartition(RangeVar *matrel, MatRelPartitionKey *lower_bound,
					  Interval *duration)
{
	CreateStmt *cstmt;
	PartitionBoundSpec *spec;
	A_Const *startc;
	A_Const *endc;
	char *starts;
	char *ends;
	Oid matrelid = RangeVarGetRelid(matrel, NoLock, false);
	MatRelPartitionKey upper_bound_val;
	MatRelPartitionKey *upper_bound = &upper_bound_val;
	Datum toast_options;
	ObjectAddress address;
	static char *validnsps[] = HEAP_RELOPT_NAMESPACES;
	List *matrel_options = NIL;

	*upper_bound = get_partition_upper_bound(lower_bound, duration);

	starts = print_timestamp(lower_bound);
	ends = print_timestamp(upper_bound);

	startc = makeNode(A_Const);
	startc->val.sval.type = T_String;
	startc->val.sval.sval = starts;

	endc = makeNode(A_Const);
	endc->val.sval.type = T_String;
	endc->val.sval.sval = ends;

	spec = makeNode(PartitionBoundSpec);
	spec->strategy = PARTITION_STRATEGY_RANGE;

	spec->lowerdatums = list_make1(startc);
	spec->upperdatums = list_make1(endc);

	cstmt = makeNode(CreateStmt);
	cstmt->inhRelations = list_make1(matrel);

	cstmt->relation = copyObject(matrel);

	/*
	 * We name the partition using the following format:
	 *
	 * mrel_<mrelid>_<start timestamp>_<end timestamp>
	 *
	 * Where each timestamp is formatted as YYYYMMDDHHmmSS
	 *
	 * This keeps the partition names independent of the order in which they're created, and also
	 * keeps them ordered sanely when a partitioned matrel is described with \d+.
	 */
	cstmt->relation->relname = get_partition_name(matrelid, lower_bound, upper_bound);

	cstmt->partbound = spec;

	matrel_options = add_fillfactor(matrel_options, continuous_view_fillfactor);

	cstmt->options = matrel_options;

	transformCreateStmt(cstmt, "CREATE TABLE");

	address = DefineRelation(cstmt, RELKIND_RELATION, InvalidOid, NULL, "combiner");
	CommandCounterIncrement();

	toast_options = transformRelOptions((Datum) 0, cstmt->options, "toast",
		validnsps, true, false);

	(void) heap_reloptions(RELKIND_TOASTVALUE, toast_options, true);
	AlterTableCreateToastTable(address.objectId, toast_options, AccessExclusiveLock);

	if (log_partitions)
		elog(LOG, "created partition %s", cstmt->relation->relname);
}

/*
 * CQOSRelOpen
 *
 * Open an output stream
 */
ResultRelInfo *
CQOSRelOpen(Relation osrel)
{
	ResultRelInfo *resultRelInfo;

	resultRelInfo = makeNode(ResultRelInfo);
	resultRelInfo->ri_RangeTableIndex = 1; /* dummy */
	resultRelInfo->ri_RelationDesc = osrel;
	resultRelInfo->ri_TrigDesc = NULL;

	return resultRelInfo;
}

/*
 * CQCSRelClose
 */
void
CQOSRelClose(ResultRelInfo *rinfo)
{
	pfree(rinfo);
}

/*
 * CQMatViewOpen
 *
 * Open any indexes associated with the given materialization table
 */
ResultRelInfo *
CQMatRelOpen(Relation matrel)
{
	ResultRelInfo *resultRelInfo;

	resultRelInfo = makeNode(ResultRelInfo);
	resultRelInfo->ri_RangeTableIndex = 1; /* dummy */
	resultRelInfo->ri_RelationDesc = matrel;
	resultRelInfo->ri_TrigDesc = NULL;

	ExecOpenIndices(resultRelInfo, false);

	return resultRelInfo;
}

/*
 * CQMatViewClose
 *
 * Release any resources associated with the given indexes
 */
void
CQMatRelClose(ResultRelInfo *rinfo)
{
	ExecCloseIndices(rinfo);
	pfree(rinfo);
}

/*
 * ExecInsertCQMatRelIndexTuples
 *
 * This is a trimmed-down version of ExecInsertIndexTuples
 */
void
ExecInsertCQMatRelIndexTuples(ResultRelInfo *indstate, TupleTableSlot *slot,
							  ItemPointer tid, EState *estate)
{
	int			i;
	int			numIndexes;
	RelationPtr relationDescs;
	Relation	heapRelation;
	IndexInfo **indexInfoArray;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];

	if (indstate->ri_RelationDesc->rd_tableam != GetHeapamTableAmRoutine())
	{
		elog(ERROR, "%s: unsupported access method for relation: %u",
			 __func__, indstate->ri_RelationDesc->rd_rel->oid);
	}

	/* bail if there are no indexes to update */
	numIndexes = indstate->ri_NumIndices;
	if (numIndexes == 0)
		return;

	relationDescs = indstate->ri_IndexRelationDescs;
	indexInfoArray = indstate->ri_IndexRelationInfo;
	heapRelation = indstate->ri_RelationDesc;

	/*
	 * for each index, form and insert the index tuple
	 */
	for (i = 0; i < numIndexes; i++)
	{
		IndexInfo  *indexInfo;

		indexInfo = indexInfoArray[i];

		/* If the index is marked as read-only, ignore it */
		if (!indexInfo->ii_ReadyForInserts)
			continue;

		/* Index expressions need an EState to be eval'd in */
		if (indexInfo->ii_Expressions)
		{
			ExprContext *econtext = GetPerTupleExprContext(estate);
			econtext->ecxt_scantuple = slot;
		}

		FormIndexDatum(indexInfo, slot, estate, values, isnull);
		index_insert(relationDescs[i], values, isnull, tid,
				heapRelation, relationDescs[i]->rd_index->indisunique ?
					 UNIQUE_CHECK_YES : UNIQUE_CHECK_NO, false, indexInfo);
	}
}

/*
 * ExecCQMatViewUpdate
 *
 * Update an existing row of a CV materialization table.
 *
 * Return true on success, false on failure
 */
bool
ExecCQMatRelUpdate(ResultRelInfo *ri, TupleTableSlot *slot, EState *estate)
{
	HeapTuple tup;
	bool result = true;
	TU_UpdateIndexes update_indexes;

	if (ri->ri_RelationDesc->rd_tableam != GetHeapamTableAmRoutine())
	{
		elog(ERROR, "%s: unsupported access method for relation: %u",
			 __func__, ri->ri_RelationDesc->rd_rel->oid);
	}

	if (ri->ri_RelationDesc->rd_att->constr)
	{
		/*
		 * We don't want the entire sync transaction to fail when a constraint fails
		 */
		PG_TRY();
		{
			ExecConstraints(ri, slot, estate);
		}
		PG_CATCH();
		{
			EmitErrorReport();
			FlushErrorState();

			result = false;
		}
		PG_END_TRY();
	}

	if (!result)
		return false;

	tup = ExecFetchSlotHeapTuple(slot, true, NULL);

	simple_heap_update(ri->ri_RelationDesc, &tup->t_self, tup, &update_indexes);

	if (!HeapTupleIsHeapOnly(tup))
		ExecInsertCQMatRelIndexTuples(ri, slot, &tup->t_self, estate);

	return true;
}

/*
 * ExecCQMatViewInsert
 *
 * Insert a new row into a CV materialization table
 *
 * Return true on success, false on failure
 */
bool
ExecCQMatRelInsert(ResultRelInfo *ri, TupleTableSlot *slot, EState *estate)
{
	bool result = true;

	if (ri->ri_RelationDesc->rd_tableam != GetHeapamTableAmRoutine())
	{
		elog(ERROR, "%s: unsupported access method for relation: %u",
			 __func__, ri->ri_RelationDesc->rd_rel->oid);
	}

	if (ri->ri_RelationDesc->rd_att->constr)
	{
		/*
		 * We don't want the entire sync transaction to fail when a constraint fails
		 */
		PG_TRY();
		{
			ExecConstraints(ri, slot, estate);
		}
		PG_CATCH();
		{
			EmitErrorReport();
			FlushErrorState();

			result = false;
		}
		PG_END_TRY();
	}

	if (!result)
		return false;

	table_tuple_insert(ri->ri_RelationDesc, slot, GetCurrentCommandId(true), 0,
					   NULL);

	ExecInsertCQMatRelIndexTuples(ri, slot, &slot->tts_tid, estate);

	return true;
}

/*
 * CVNameToOSRelName
 */
char *
CVNameToOSRelName(char *cv_name)
{
	char *relname = palloc0(NAMEDATALEN);

	strcpy(relname, cv_name);
	append_suffix(relname, CQ_OSREL_SUFFIX, NAMEDATALEN);

	return relname;
}

/*
 * CVNameToMatRelName
 */
char *
CVNameToMatRelName(char *cv_name)
{
	char *relname = palloc0(NAMEDATALEN);

	strcpy(relname, cv_name);
	append_suffix(relname, CQ_MATREL_SUFFIX, NAMEDATALEN);

	return relname;
}

/*
 * CVNameToDefRelName
 */
char *
CVNameToDefRelName(char *cv_name)
{
	char *relname = palloc0(NAMEDATALEN);

	strcpy(relname, cv_name);
	append_suffix(relname, CQ_DEFREL_SUFFIX, NAMEDATALEN);

	return relname;
}

/*
 * CVNameToSeqRelName
 */
char *
CVNameToSeqRelName(char *cv_name)
{
	char *relname = palloc0(NAMEDATALEN);

	strcpy(relname, cv_name);
	append_suffix(relname, CQ_SEQREL_SUFFIX, NAMEDATALEN);

	return relname;
}
