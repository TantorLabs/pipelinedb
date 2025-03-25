create foreign table ff_stream (ts timestamp, x int) server pipelinedb;

create view cv_ff
as select ts, count(*) from ff_stream group by ts;

\d+ cv_ff_mrel;
