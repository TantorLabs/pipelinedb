-- Basic partitioning tests

-- Helper function to list matrel partitions for a given CV, masking OIDs
-- encoded into partition names by PipelineDB
drop function if exists list_partitions(name);
create function
  list_partitions(
    baserel name
  )
  returns table (
    parent_schema name, parent name, child name, child_schema text
  )
  as $BODY$
    begin
      return query select
        nmsp_parent.nspname as parent_schema,
        parent.relname      as parent,
        nmsp_child.nspname  as child,
        regexp_replace(child.relname, 'mrel_[0-9]+_', 'mrel_OID_')       as child_schema
      from pg_inherits
        join pg_class parent        on pg_inherits.inhparent = parent.oid
        join pg_class child         on pg_inherits.inhrelid   = child.oid
        join pg_namespace nmsp_parent   on nmsp_parent.oid  = parent.relnamespace
        join pg_namespace nmsp_child    on nmsp_child.oid   = child.relnamespace
      where
        parent.relname = baserel || '_mrel'
      order by child_schema
      ;
    end;
  $BODY$
language plpgsql;

create foreign table part_stream (
  x int,
  ts timestamp,
  tstz timestamptz
) server pipelinedb;

-- not a timestamp/timestamptz column in partition_by
create view cv_part_inv with (
  action=materialize,
  partition_by=x
) as select count(*) from part_stream;

-- misleading error message, see GL-4
create view cv_part_inv with (
  action=materialize,
  partition_by=ts,
  partition_duration='1 hour'
) as select count(*) from part_stream;

-- no error, see GL-6
create view cv_part_inv with (
  action=materialize,
  partition_duration='1 hour'
) as select ts, x from part_stream;

drop view cv_part_inv;

-- worker exits with failed to parse option "partition_duration", see GL-7
create view cv_part_inv with (
  action=transform,
  partition_by=ts,
  partition_duration='1 hour'
) as select ts, x from part_stream;

insert into part_stream (x, ts) values
  (0, '2024-04-11 13:53:07.82049'),
  (1, '2024-04-11 14:53:07.82049');
drop view cv_part_inv;

-- timestamp-partitioned CVs
create view cv_part_ts_noagg_1h with (
  action=materialize,
  partition_by=ts,
  partition_duration='1 hour'
) as select ts, x from part_stream;

create view cv_part_ts_agg_7s with (
  action=materialize,
  partition_by=ts,
  partition_duration='7 seconds'
) as select ts, count(*) from part_stream group by ts;

create view cv_part_ts_agg_3m with (
  action=materialize,
  partition_by=ts,
  partition_duration='3 minutes'
) as select ts, count(*) from part_stream group by ts;

create view cv_part_ts_agg_1h with (
  action=materialize,
  partition_by=ts,
  partition_duration='1 hour'
) as select ts, count(*) from part_stream group by ts;

create view cv_part_ts_agg_2w with (
  action=materialize,
  partition_by=ts,
  partition_duration='2 weeks'
) as select ts, count(*) from part_stream group by ts;

create view cv_part_ts_agg_1mo with (
  action=materialize,
  partition_by=ts,
  partition_duration='1 month'
) as select ts, count(*) from part_stream group by ts;

create view cv_part_ts_agg_1y with (
  action=materialize,
  partition_by=ts,
  partition_duration='1 year'
) as select ts, count(*) from part_stream group by ts;

create view cv_part_ts_agg_1y2m with (
  action=materialize,
  partition_by=ts,
  partition_duration='1 year 2 months'
) as select ts, count(*) from part_stream group by ts;

-- timestamptz-partitioned CVs
create view cv_part_tstz_noagg_1h with (
  action=materialize,
  partition_by=tstz,
  partition_duration='1 hour'
) as select tstz, x from part_stream;

create view cv_part_tstz_agg_7s with (
  action=materialize,
  partition_by=tstz,
  partition_duration='7 seconds'
) as select tstz, count(*) from part_stream group by tstz;

create view cv_part_tstz_agg_3m with (
  action=materialize,
  partition_by=tstz,
  partition_duration='3 minutes'
) as select tstz, count(*) from part_stream group by tstz;

create view cv_part_tstz_agg_1h with (
  action=materialize,
  partition_by=tstz,
  partition_duration='1 hour'
) as select tstz, count(*) from part_stream group by tstz;

create view cv_part_tstz_agg_2w with (
  action=materialize,
  partition_by=tstz,
  partition_duration='2 weeks'
) as select tstz, count(*) from part_stream group by tstz;

create view cv_part_tstz_agg_1mo with (
  action=materialize,
  partition_by=tstz,
  partition_duration='1 month'
) as select tstz, count(*) from part_stream group by tstz;

create view cv_part_tstz_agg_1y with (
  action=materialize,
  partition_by=tstz,
  partition_duration='1 year'
) as select tstz, count(*) from part_stream group by tstz;

create view cv_part_tstz_agg_1y2m with (
  action=materialize,
  partition_by=tstz,
  partition_duration='1 year 2 months'
) as select tstz, count(*) from part_stream group by tstz;

insert into part_stream (x, ts) values
  (0, '2024-04-11 13:53:07.82049'),
  (1, '2024-04-11 14:53:07.82049'),
  (2, '2024-04-11 15:53:07.82049'),
  (3, '2024-04-11 16:53:07.82049'),
  (4, '2024-04-11 13:53:07.82049'),
  (5, '2024-04-11 14:53:07.82049');

insert into part_stream (x, tstz) values
  (0, '2024-04-11 13:53:07.82049-03'),
  (1, '2024-04-11 14:53:07.82049-02:30'),
  (2, '2024-04-11 15:53:07.82049-01:15'),
  (3, '2024-04-11 16:53:07.82049+01:15'),
  (4, '2024-04-11 13:53:07.82049+02:30'),
  (5, '2024-04-11 14:53:07.82049+03');

select * from list_partitions('cv_part_ts_noagg_1h');
select * from cv_part_ts_noagg_1h;

select * from list_partitions('cv_part_ts_agg_7s');
select * from cv_part_ts_agg_7s;

select * from list_partitions('cv_part_ts_agg_3m');
select * from cv_part_ts_agg_3m;

select * from list_partitions('cv_part_ts_agg_1h');
select * from cv_part_ts_agg_1h;

select * from list_partitions('cv_part_ts_agg_2w');
select * from cv_part_ts_agg_2w;

select * from list_partitions('cv_part_ts_agg_1mo');
select * from cv_part_ts_agg_1mo;

select * from list_partitions('cv_part_ts_agg_1y');
select * from cv_part_ts_agg_1y;

select * from list_partitions('cv_part_ts_agg_1y2m');
select * from cv_part_ts_agg_1y2m;

select * from list_partitions('cv_part_tstz_noagg_1h');
select * from cv_part_tstz_noagg_1h;

select * from list_partitions('cv_part_tstz_agg_7s');
select * from cv_part_tstz_agg_7s;

select * from list_partitions('cv_part_tstz_agg_3m');
select * from cv_part_tstz_agg_3m;

select * from list_partitions('cv_part_tstz_agg_1h');
select * from cv_part_tstz_agg_1h;

select * from list_partitions('cv_part_tstz_agg_2w');
select * from cv_part_tstz_agg_2w;

select * from list_partitions('cv_part_tstz_agg_1mo');
select * from cv_part_tstz_agg_1mo;

select * from list_partitions('cv_part_tstz_agg_1y');
select * from cv_part_tstz_agg_1y;

select * from list_partitions('cv_part_tstz_agg_1y2m');
select * from cv_part_tstz_agg_1y2m;
