/*-------------------------------------------------------------------------
 *
 * compat.h
 *  Interface for functionality that breaks between major PG versions
 *
 * Copyright (c) 2023, Tantor Labs, Inc.
 * Copyright (c) 2018, PipelineDB, Inc.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/objectaddress.h"
#include "executor/tuptable.h"
#include "nodes/execnodes.h"
#include "optimizer/pathnode.h"

extern bool CompatProcOidIsAgg(Oid oid);
extern void CompatAnalyzeVacuumStmt(VacuumStmt *stmt);

extern Relids CompatCalcNestLoopRequiredOuter(Path *outer, Path *inner);
extern void CompatPrepareEState(PlannedStmt *pstmt, EState *estate);

extern void
CompatExecTuplesHashPrepare(int numCols,Oid *eqOperators,
#if (PG_VERSION_NUM < 110000)
		FmgrInfo **eqFunctions,
#else
		Oid **eqFunctions,
#endif
		FmgrInfo **hashFunctions);

extern TupleHashTable
CompatBuildTupleHashTable(TupleDesc inputDesc,
					int numCols, AttrNumber *keyColIdx,
#if (PG_VERSION_NUM < 110000)
					FmgrInfo *eqfuncs,
#else
					Oid *eqfuncs,
#endif
					FmgrInfo *hashfunctions,
					Oid *collations,
					long nbuckets, Size additionalsize,
					MemoryContext tablecxt,
					MemoryContext tempcxt, bool use_variable_hash_iv);

extern char *CompatGetAttName(Oid relid, AttrNumber att);
extern void ComaptExecAssignResultTypeFromTL(PlanState *ps);
extern HeapTuple CompatSearchSysCacheLocked1(int cacheId, Datum key1);
extern void CompatUnlockTupleInplaceUpdate(Relation relation, ItemPointer tid);
