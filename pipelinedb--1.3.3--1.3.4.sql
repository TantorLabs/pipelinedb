DROP AGGREGATE combine_interval_avg(interval[]);
DROP AGGREGATE partial_combine_interval_avg(intervqal[]);

CREATE AGGREGATE combine_interval_avg(interval[]) (
  sfunc = interval_avg_accum,
  stype = internal,
  finalfunc = interval_avg,
  combinefunc = interval_avg_combine,
  parallel = safe
);

CREATE AGGREGATE partial_combine_interval_avg(interval) (
  sfunc = interval_avg_accum,
  stype = internal,
  combinefunc = interval_avg_combine,
  parallel = safe
);
