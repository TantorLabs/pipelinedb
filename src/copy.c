/*-------------------------------------------------------------------------
 *
 * copy.c
 *		Implements the COPY utility command for use with streams
 *
 * Portions Copyright (c) 2018, PipelineDB, Inc.
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_type.h"
#include "commands/copy.h"
#include "commands/copyfrom_internal.h"
#include "commands/defrem.h"
#include "commands/trigger.h"
#include "compat.h"
#include "copy.h"
#include "executor/executor.h"
#if (PG_VERSION_NUM >= 110000)
#include "executor/execPartition.h"
#endif
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "optimizer/planner.h"
#include "nodes/makefuncs.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "pipeline_stream.h"
#include "rewrite/rewriteHandler.h"
#include "storage/fd.h"
#include "stream_fdw.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/portal.h"
#include "utils/rel.h"
#include "utils/rls.h"
#include "utils/snapmgr.h"


#define ISOCTAL(c) (((c) >= '0') && ((c) <= '7'))
#define OCTVALUE(c) ((c) - '0')

copy_data_source_cb stream_copy_hook = NULL;

/* DestReceiver for COPY (query) TO */
typedef struct
{
	DestReceiver pub;			/* publicly-known function pointers */
	CopyFromState	cstate;		/* CopyStateData for the command */
	uint64		processed;		/* # of tuples processed */
} DR_copy;

#if (PG_VERSION_NUM / 100 != 1700)
#error DoStreamCopy is a copy of DoCopy from the Postgres source code with \
	minor modifications. You may want to update it.
#endif

/*
 *	 DoCopy executes the SQL COPY statement
 *
 * Either unload or reload contents of table <relation>, depending on <from>.
 * (<from> = true means we are inserting into the table.)  In the "TO" case
 * we also support copying the output of an arbitrary SELECT, INSERT, UPDATE
 * or DELETE query.
 *
 * If <pipe> is false, transfer is between the table and the file named
 * <filename>.  Otherwise, transfer is between the table and our regular
 * input/output stream. The latter could be either stdin/stdout or a
 * socket, depending on whether we're running under Postmaster control.
 *
 * Do not allow a Postgres user without the 'pg_read_server_files' or
 * 'pg_write_server_files' role to read from or write to a file.
 *
 * Do not allow the copy if user doesn't have proper permission to access
 * the table or the specifically requested columns.
 */
void
DoStreamCopy(ParseState *pstate, const CopyStmt *stmt,
	   int stmt_location, int stmt_len,
	   uint64 *processed)
{
	bool		is_from = stmt->is_from;
	bool		pipe = (stmt->filename == NULL);
	Relation	rel;
	Oid			relid;
	RawStmt    *query = NULL;
	Node	   *whereClause = NULL;

	/*
	 * Disallow COPY to/from file or program except to users with the
	 * appropriate role.
	 */
	if (!pipe)
	{
		if (stmt->is_program)
		{
			if (!has_privs_of_role(GetUserId(), ROLE_PG_EXECUTE_SERVER_PROGRAM))
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to COPY to or from an external program"),
						 errdetail("Only roles with privileges of the \"%s\" role may COPY to or from an external program.",
								   "pg_execute_server_program"),
						 errhint("Anyone can COPY to stdout or from stdin. "
								 "psql's \\copy command also works for anyone.")));
		}
		else
		{
			if (is_from && !has_privs_of_role(GetUserId(), ROLE_PG_READ_SERVER_FILES))
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to COPY from a file"),
						 errdetail("Only roles with privileges of the \"%s\" role may COPY from a file.",
								   "pg_read_server_files"),
						 errhint("Anyone can COPY to stdout or from stdin. "
								 "psql's \\copy command also works for anyone.")));

			if (!is_from && !has_privs_of_role(GetUserId(), ROLE_PG_WRITE_SERVER_FILES))
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to COPY to a file"),
						 errdetail("Only roles with privileges of the \"%s\" role may COPY to a file.",
								   "pg_write_server_files"),
						 errhint("Anyone can COPY to stdout or from stdin. "
								 "psql's \\copy command also works for anyone.")));
		}
	}

	if (stmt->relation)
	{
		LOCKMODE	lockmode = is_from ? RowExclusiveLock : AccessShareLock;
		ParseNamespaceItem *nsitem;
		RTEPermissionInfo *perminfo;
		TupleDesc	tupDesc;
		List	   *attnums;
		ListCell   *cur;

		Assert(!stmt->query);

		/* Open and lock the relation, using the appropriate lock type. */
		rel = table_openrv(stmt->relation, lockmode);

		relid = RelationGetRelid(rel);

		nsitem = addRangeTableEntryForRelation(pstate, rel, lockmode,
											   NULL, false, false);

		perminfo = nsitem->p_perminfo;
		perminfo->requiredPerms = (is_from ? ACL_INSERT : ACL_SELECT);

		if (stmt->whereClause)
		{
			/* add nsitem to query namespace */
			addNSItemToQuery(pstate, nsitem, false, true, true);

			/* Transform the raw expression tree */
			whereClause = transformExpr(pstate, stmt->whereClause, EXPR_KIND_COPY_WHERE);

			/* Make sure it yields a boolean result. */
			whereClause = coerce_to_boolean(pstate, whereClause, "WHERE");

			/* we have to fix its collations too */
			assign_expr_collations(pstate, whereClause);

			whereClause = eval_const_expressions(NULL, whereClause);

			whereClause = (Node *) canonicalize_qual((Expr *) whereClause, false);
			whereClause = (Node *) make_ands_implicit((Expr *) whereClause);
		}

		tupDesc = RelationGetDescr(rel);
		attnums = CopyGetAttnums(tupDesc, rel, stmt->attlist);
		foreach(cur, attnums)
		{
			int			attno;
			Bitmapset **bms;

			attno = lfirst_int(cur) - FirstLowInvalidHeapAttributeNumber;
			bms = is_from ? &perminfo->insertedCols : &perminfo->selectedCols;

			*bms = bms_add_member(*bms, attno);
		}
		ExecCheckPermissions(pstate->p_rtable, list_make1(perminfo), true);

		/*
		 * Permission check for row security policies.
		 *
		 * check_enable_rls will ereport(ERROR) if the user has requested
		 * something invalid and will otherwise indicate if we should enable
		 * RLS (returns RLS_ENABLED) or not for this COPY statement.
		 *
		 * If the relation has a row security policy and we are to apply it
		 * then perform a "query" copy and allow the normal query processing
		 * to handle the policies.
		 *
		 * If RLS is not enabled for this, then just fall through to the
		 * normal non-filtering relation handling.
		 */
		if (check_enable_rls(relid, InvalidOid, false) == RLS_ENABLED)
		{
			SelectStmt *select;
			ColumnRef  *cr;
			ResTarget  *target;
			RangeVar   *from;
			List	   *targetList = NIL;

			if (is_from)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("COPY FROM not supported with row-level security"),
						 errhint("Use INSERT statements instead.")));

			/*
			 * Build target list
			 *
			 * If no columns are specified in the attribute list of the COPY
			 * command, then the target list is 'all' columns. Therefore, '*'
			 * should be used as the target list for the resulting SELECT
			 * statement.
			 *
			 * In the case that columns are specified in the attribute list,
			 * create a ColumnRef and ResTarget for each column and add them
			 * to the target list for the resulting SELECT statement.
			 */
			if (!stmt->attlist)
			{
				cr = makeNode(ColumnRef);
				cr->fields = list_make1(makeNode(A_Star));
				cr->location = -1;

				target = makeNode(ResTarget);
				target->name = NULL;
				target->indirection = NIL;
				target->val = (Node *) cr;
				target->location = -1;

				targetList = list_make1(target);
			}
			else
			{
				ListCell   *lc;

				foreach(lc, stmt->attlist)
				{
					/*
					 * Build the ColumnRef for each column.  The ColumnRef
					 * 'fields' property is a String node that corresponds to
					 * the column name respectively.
					 */
					cr = makeNode(ColumnRef);
					cr->fields = list_make1(lfirst(lc));
					cr->location = -1;

					/* Build the ResTarget and add the ColumnRef to it. */
					target = makeNode(ResTarget);
					target->name = NULL;
					target->indirection = NIL;
					target->val = (Node *) cr;
					target->location = -1;

					/* Add each column to the SELECT statement's target list */
					targetList = lappend(targetList, target);
				}
			}

			/*
			 * Build RangeVar for from clause, fully qualified based on the
			 * relation which we have opened and locked.  Use "ONLY" so that
			 * COPY retrieves rows from only the target table not any
			 * inheritance children, the same as when RLS doesn't apply.
			 */
			from = makeRangeVar(get_namespace_name(RelationGetNamespace(rel)),
								pstrdup(RelationGetRelationName(rel)),
								-1);
			from->inh = false;	/* apply ONLY */

			/* Build query */
			select = makeNode(SelectStmt);
			select->targetList = targetList;
			select->fromClause = list_make1(from);

			query = makeNode(RawStmt);
			query->stmt = (Node *) select;
			query->stmt_location = stmt_location;
			query->stmt_len = stmt_len;

			/*
			 * Close the relation for now, but keep the lock on it to prevent
			 * changes between now and when we start the query-based COPY.
			 *
			 * We'll reopen it later as part of the query-based COPY.
			 */
			table_close(rel, NoLock);
			rel = NULL;
		}
	}
	else
	{
		Assert(stmt->query);

		/* MERGE is allowed by parser, but unimplemented. Reject for now */
		if (IsA(stmt->query, MergeStmt))
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("MERGE not supported in COPY"));

		query = makeNode(RawStmt);
		query->stmt = stmt->query;
		query->stmt_location = stmt_location;
		query->stmt_len = stmt_len;

		relid = InvalidOid;
		rel = NULL;
	}

	if (is_from)
	{
		CopyFromState cstate;

		Assert(rel);

		/* check read-only transaction and parallel mode */
		if (XactReadOnly && !rel->rd_islocaltemp)
			PreventCommandIfReadOnly("COPY FROM");

		cstate = BeginCopyFrom(pstate, rel, whereClause,
							   stmt->filename, stmt->is_program,
							   stream_copy_hook, stmt->attlist, stmt->options);
		*processed = CopyStreamFrom(cstate);	/* copy from file to database */
		EndCopyFrom(cstate);
	}
	else
	{
		/* We only allow COPY FROM */
		Assert(false);
	}

	if (rel != NULL)
		table_close(rel, NoLock);
}


#if (PG_VERSION_NUM / 100 != 1700)
#error CopyStreamFrom is a heavily modified copy of CopyFrom from the Postgres \
	source code. You may want to update it.
#endif

/*
 * Copy FROM something to a stream
 *
 * This is just a highly trimmed down version of PG's CopyFrom designed
 * for efficient COPY into streams.
 */
uint64
CopyStreamFrom(CopyFromState cstate)
{
	HeapTuple	tuple;
	TupleDesc	tupDesc;
	Datum *values;
	bool *nulls;
	ResultRelInfo *resultRelInfo;
	EState *estate = CreateExecutorState();
	ExprContext *econtext;
	TupleTableSlot *myslot;
	MemoryContext oldcontext = CurrentMemoryContext;
	ErrorContextCallback errcallback;
	uint64 processed = 0;

	Assert(cstate->rel);
	Assert(list_length(cstate->range_table) == 1);

	/*
	 * This function is meant to be called explicitly from extension code, so we Assert here
	 * instead of throw an error because if this isn't a stream then the code is wrong.
	 */
	Assert(RelidIsStream(RelationGetRelid(cstate->rel)));

	tupDesc = RelationGetDescr(cstate->rel);

	/*
	 * We need a ResultRelInfo so we can use the regular executor's
	 * index-entry-making machinery.  (There used to be a huge amount of code
	 * here that basically duplicated execUtils.c ...)
	 */
	ExecInitRangeTable(estate, cstate->range_table, cstate->rteperminfos);
	resultRelInfo = makeNode(ResultRelInfo);
	ExecInitResultRelation(estate, resultRelInfo, 1);

	/*
	 * Copy the RTEPermissionInfos into estate as well, so that
	 * ExecGetInsertedCols() et al will work correctly.
	 */
	estate->es_rteperminfos = cstate->rteperminfos;

	BeginStreamModify(NULL, resultRelInfo, NIL, 0, 0);

	/* Set up a tuple slot too */
	myslot = ExecInitExtraTupleSlot(estate, tupDesc, &TTSOpsBufferHeapTuple);

	values = (Datum *) palloc(tupDesc->natts * sizeof(Datum));
	nulls = (bool *) palloc(tupDesc->natts * sizeof(bool));

	econtext = GetPerTupleExprContext(estate);

	/* Set up callback to identify error line number */
	errcallback.callback = CopyFromErrorCallback;
	errcallback.arg = (void *) cstate;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	for (;;)
	{
		CHECK_FOR_INTERRUPTS();

		/*
		 * Reset the per-tuple exprcontext. We do this after every tuple, to
		 * clean-up after expression evaluations etc.
		 */
		ResetPerTupleExprContext(estate);

		/*
		 * Switch to per-tuple context before calling NextCopyFrom, which does
		 * evaluate default expressions etc. and requires per-tuple context.
		 */
		MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));

		if (!NextCopyFrom(cstate, econtext, values, nulls))
			break;

		/* And now we can form the input tuple. */
		tuple = heap_form_tuple(tupDesc, values, nulls);

		MemoryContextSwitchTo(oldcontext);

		/*
		 * Place tuple in tuple slot --- but slot shouldn't free it
		 *
		 * We deliberately don't use a cheaper ExecStoreBufferHeapTuple()
		 * here, because that would require passing buffer == InvalidBuffer,
		 * and this case has been broken in the PostgreSQL code for
		 * casserts-enabled builds for quite some time: the function
		 * documentation says passing InvalidBuffer is acceptable to indicate
		 * the tuple is not on a disk page, but the assertion in the function
		 * does not allow that, which is a regression from the table access
		 * methods refactoring.
		 *
		 * It would probably be worth reporting it -hackers, but that will
		 * unlikely fix older, especially unsupported, PostgreSQL
		 * versions. For now, let's just use a less efficient
		 * ExecForceStoreHeapTuple(). If this becomes a performance problem,
		 * we can proceed with more creative ways to fix it, like reporting
		 * and fixing it upstream, or duplicating and fixing the necessary
		 * code from the PostgreSQL source tree.
		 */
		ExecForceStoreHeapTuple(tuple, myslot, false);

		ExecStreamInsert(NULL, resultRelInfo, myslot, NULL);
		processed++;
	}

	/* Done, clean up */
	error_context_stack = errcallback.previous;

	MemoryContextSwitchTo(oldcontext);

	pfree(values);
	pfree(nulls);

	ExecResetTupleTable(estate->es_tupleTable, false);

	/* Close the result relations, including any trigger target relations */
	ExecCloseResultRelations(estate);
	ExecCloseRangeTableRelations(estate);

	FreeExecutorState(estate);

	EndStreamModify(NULL, resultRelInfo);

	return processed;
}
