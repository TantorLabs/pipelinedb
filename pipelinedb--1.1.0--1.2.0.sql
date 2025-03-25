
-- Add system metadata for partitions 
ALTER TABLE pipelinedb.cont_query ADD partition_duration int4;
ALTER TABLE pipelinedb.cont_query ADD partition_key_attno int2;

DROP AGGREGATE combine(anyelement);
/*
 * Dummy combine aggregate for user combines
 *
 * User combines will always take a finalized aggregate value as input
 * and return a combined aggregate of the same type, so this dummy aggregate
 * ensures we make it through the analyzer with correct types everywhere.
 */
CREATE AGGREGATE combine(anyelement) (
  sfunc = combine_trans_dummy,
  msfunc = combine_trans_dummy,
  minvfunc = combine_trans_dummy,
  mstype = anyelement,
  minitcond = 'combine_dummy',
  stype = anyelement,
  parallel = safe
);

DROP AGGREGATE sw_combine(anyelement);
/*
 * Dummy combine aggregate used in SW overlay queries
 *
 * It's convenient to differentiate between these and user combines in the planner,
 * so we indicate SW overlay combines using this aggregate
 */
CREATE AGGREGATE sw_combine(anyelement) (
  sfunc = combine_trans_dummy,
  msfunc = combine_trans_dummy,
  minvfunc = combine_trans_dummy,
  mstype = anyelement,
  minitcond = 'sw_combine_dummy',
  stype = anyelement,
  parallel = safe
);
