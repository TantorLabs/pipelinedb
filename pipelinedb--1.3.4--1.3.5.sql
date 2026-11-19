DROP AGGREGATE IF EXISTS combine_interval_avg(internal);
DROP AGGREGATE IF EXISTS partial_combine_interval_avg(internal);
DROP AGGREGATE IF EXISTS combine_interval_sum(internal);

CREATE AGGREGATE combine_interval_avg(internal) (
  sfunc = interval_avg_combine,
  stype = internal,
  finalfunc = interval_avg,
  combinefunc = interval_avg_combine,
  serialfunc = interval_avg_serialize,
  deserialfunc = interval_avg_deserialize,
  parallel = safe
);

CREATE AGGREGATE partial_combine_interval_avg(internal) (
  sfunc = interval_avg_combine,
  stype = internal,
  finalfunc = interval_avg,
  combinefunc = interval_avg_combine,
  serialfunc = interval_avg_serialize,
  deserialfunc = interval_avg_deserialize,
  parallel = safe
);

CREATE AGGREGATE combine_interval_sum(internal) (
  sfunc = interval_avg_combine,
  stype = internal,
  finalfunc = interval_sum,
  combinefunc = interval_avg_combine,
  serialfunc = interval_avg_serialize,
  deserialfunc = interval_avg_deserialize,
  parallel = safe
);
