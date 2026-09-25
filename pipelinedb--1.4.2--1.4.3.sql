-- PostgreSQL 15.19+ (CVE-2026-14680) no longer coerces internal to "any" or
-- polymorphic types when resolving aggregate final functions, so these
-- aggregates must use final functions with exact internal-state signatures.
-- This script also runs on fresh installs, where 1.0.0 already creates the
-- new signatures, so every statement must be idempotent.

CREATE OR REPLACE FUNCTION set_cardinality(internal)
RETURNS integer
AS 'MODULE_PATHNAME', 'set_cardinality'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE OR REPLACE AGGREGATE exact_count_distinct(anynonarray) (
  sfunc = set_agg_trans,
  stype = internal,
  finalfunc = set_cardinality,
  combinefunc = set_agg_combine,
  deserialfunc = array_agg_deserialize,
  serialfunc = array_agg_serialize,
  parallel = safe
);

CREATE OR REPLACE AGGREGATE combine_exact_count_distinct(internal) (
  sfunc = set_agg_combine,
  stype = internal,
  finalfunc = set_cardinality,
  combinefunc = set_agg_combine,
  deserialfunc = array_agg_deserialize,
  serialfunc = array_agg_serialize,
  parallel = safe
);

DROP FUNCTION IF EXISTS set_cardinality(internal, anynonarray);

CREATE OR REPLACE FUNCTION combinable_array_agg_finalfn2(internal, internal)
RETURNS "any"
AS 'MODULE_PATHNAME', 'combinable_array_agg_finalfn'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE OR REPLACE AGGREGATE combine_set_agg(internal) (
  sfunc = set_agg_combine,
  stype = internal,
  finalfunc_extra,
  finalfunc = combinable_array_agg_finalfn2,
  combinefunc = set_agg_combine,
  deserialfunc = array_agg_deserialize,
  serialfunc = array_agg_serialize,
  parallel = safe
);

DROP FUNCTION IF EXISTS combinable_array_agg_finalfn2(internal, "any");
