/*-------------------------------------------------------------------------
 *
 * planner.h
 * 		Interface for generating/modifying CQ plans
 *
 * Copyright (c) 2023, Tantor Labs, Inc.
 * Copyright (c) 2018, PipelineDB, Inc.
 *
 */
#ifndef CONT_PLAN_H
#define CONT_PLAN_H

#include "executor/execdesc.h"
#include "optimizer/planner.h"
#include "pipeline_query.h"
#include "scheduler.h"

extern bool debug_simulate_continuous_query;

extern void InstallPlannerHooks(void);

extern PlannedStmt *GetContPlan(ContQuery *view, ContQueryProcType type);
extern PlannedStmt *GetGroupsLookupPlan(Query *query, ParseState *pstate);
extern void SetCombinerPlanTuplestorestate(PlannedStmt *plan, Tuplestorestate *tupstore);
extern FuncExpr *GetGroupHashIndexExpr(ResultRelInfo *ri);
extern PlannedStmt *GetCombinerLookupPlan(ContQuery *view);
extern PlannedStmt *GetContViewOverlayPlan(ContQuery *view);

extern FuncExpr *GetGroupHashIndexExpr(ResultRelInfo *ri);

extern EState *CreateEState(QueryDesc *query_desc);
extern void SetEStateSnapshot(EState *estate);
extern void UnsetEStateSnapshot(EState *estate);

extern PlannedStmt *PipelinePlanner(Query *parse, const char *query_string, int cursorOptions, ParamListInfo boundParams);

extern void end_plan(QueryDesc *query_desc);
#endif
