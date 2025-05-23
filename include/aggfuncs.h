/*-------------------------------------------------------------------------
 *
 * aggfuncs.h
 *	  Interface for for aggregate support functions
 *
 * Copyright (c) 2023, Tantor Labs, Inc.
 * Copyright (c) 2018, PipelineDB, Inc.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PIPELINEDB_AGGFUNCS_H
#define PIPELINEDB_AGGFUNCS_H

#include "fmgr.h"

extern Datum combinable_array_agg_serialize(PG_FUNCTION_ARGS);
extern Datum combinable_array_agg_deserialize(PG_FUNCTION_ARGS);

#endif
