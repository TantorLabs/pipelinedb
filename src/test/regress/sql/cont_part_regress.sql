-- Regression tests for partitioning

create foreign table s_metric_instances (
    instance_id smallint,
    metric_key text,
    metric_val jsonb,
    metric_ts timestamp(6) with time zone,
    measure_interval interval(6),
    arrival_timestamp timestamp with time zone
)
server pipelinedb;

create view "cv_pg_store_plans_text_partitioned" with (ttl = '1 month', ttl_column = 'metric_ts', partition_duration = '1 day', partition_by = 'metric_ts') AS
select 
 metric_ts,
    (metric_val ->> 'query_hash'::text) as query_hash,
    (metric_val ->> 'plan_hash'::text) as plan_hash,
    ((metric_val ->> 'query_id'::text))::bigint as query_id,
    instance_id
   from s_metric_instances
  where ((metric_key = 'pg_store_plans'::text) and (((metric_val ->> 'plan_calls'::text))::bigint > 0))
  group by
        (metric_val ->> 'query_hash'::text),
        (metric_val ->> 'plan_hash'::text),
        ((metric_val ->> 'query_id'::text))::bigint,
        instance_id,
        metric_ts;

insert into s_metric_instances values (1, 'pg_store_plans', '{"query_hash": "bar", "plan_hash": "bar", "query_id": "123", "plan_calls": 10}', '2024-05-23 23:00:01', '1 second', '2024-05-23 23:00:01');
insert into s_metric_instances values (1, 'pg_store_plans', '{"query_hash": "bar", "plan_hash": "bar", "query_id": "123", "plan_calls": 10}', '2024-05-23 23:00:01', '1 second', '2024-05-23 23:00:01');

insert into s_metric_instances values (1, 'pg_store_plans', '{"query_hash": "bar", "plan_hash": "bar", "query_id": "123", "plan_calls": 10}', '2024-05-23 18:00:01', '1 second', '2024-05-23 23:00:01');
insert into s_metric_instances values (1, 'pg_store_plans', '{"query_hash": "bar", "plan_hash": "bar", "query_id": "123", "plan_calls": 10}', '2024-05-23 18:00:01', '1 second', '2024-05-23 23:00:01');

select * from cv_pg_store_plans_text_partitioned; 

-- Check that partitioning a matrel by an attribute other than the first one works
create view "cv_cont_part_regress" with (ttl = '1 month', ttl_column = 'metric_ts', partition_duration = '1 day', partition_by = 'metric_ts') AS
select 
    instance_id,
    (metric_val ->> 'query_hash'::text) as query_hash,
    (metric_val ->> 'plan_hash'::text) as plan_hash,
    ((metric_val ->> 'query_id'::text))::bigint as query_id,
    metric_ts
   from s_metric_instances
  where ((metric_key = 'pg_store_plans'::text) and (((metric_val ->> 'plan_calls'::text))::bigint > 0))
  group by
        (metric_val ->> 'query_hash'::text),
        (metric_val ->> 'plan_hash'::text),
        ((metric_val ->> 'query_id'::text))::bigint,
        instance_id,
        metric_ts;

insert into s_metric_instances values (1, 'pg_store_plans', '{"query_hash": "bar", "plan_hash": "bar", "query_id": "123", "plan_calls": 10}', '2024-05-23 23:00:01', '1 second', '2024-05-23 23:00:01');
insert into s_metric_instances values (1, 'pg_store_plans', '{"query_hash": "bar", "plan_hash": "bar", "query_id": "123", "plan_calls": 10}', '2024-05-23 23:00:01', '1 second', '2024-05-23 23:00:01');

insert into s_metric_instances values (1, 'pg_store_plans', '{"query_hash": "bar", "plan_hash": "bar", "query_id": "123", "plan_calls": 10}', '2024-05-23 18:00:01', '1 second', '2024-05-23 23:00:01');
insert into s_metric_instances values (1, 'pg_store_plans', '{"query_hash": "bar", "plan_hash": "bar", "query_id": "123", "plan_calls": 10}', '2024-05-23 18:00:01', '1 second', '2024-05-23 23:00:01');

select * from cv_cont_part_regress; 

drop foreign table s_metric_instances cascade;

-- Projections are incompatible with physical lookup scan and caused all kinds
-- of issues
create foreign table s (
    ts timestamp,
    c1 text,
    c2 bigint,
    c3 text,
    c4 smallint,
    c5 bigint,
    c6 bigint,
    c7 bigint,
    c8 bigint,
    c9 bigint
) server pipelinedb;

create view cv
with
  (partition_duration='1 day', partition_by='ts')
as select
    ts,
    c1,
    c2,
    c3,
    c4,
    sum(c5) c5,
    sum(c6) c6,
    sum(c7) c7,
    sum(c8) c8,
    sum(c9) c9
  from s
  group by
    ts,
    c1,
    c2,
    c3,
    c4;

insert into s (c1, c2, c3, c4, ts)
values
  ('foo', 1, 'bar', 1, '2024-10-25 12:00:00');

insert into s (c1, c2, c3, c4, ts)
values
  ('foo', 1, 'bar', 1, '2024-10-25 12:00:00');

select * from cv_mrel;

insert into s (c1, c2, c3, c4, ts)
values
  ('foo', 1, 'bar', 1, '2024-10-26 12:00:00');

insert into s (c1, c2, c3, c4, ts)
values
  ('foo', 1, 'bar', 1, '2024-10-26 12:00:00');

select * from cv_mrel;

drop foreign table s cascade;

-- GL-17: Combiner adds new tuples instead of updating existing ones

create foreign table s (x int, ts timestamp) server pipelinedb;
create view cv with (partition_by=ts, partition_duration='1 hour') as select ts, count(*) from s group by ts;

insert into s values (1, '2024-11-12 06:03:01.123'), (1, '2024-11-13 08:31:42.42');
select * from cv;

insert into s values (1, '2024-11-12 07:03:01.123'), (1, '2024-11-13 08:31:42.42');
select * from cv;

drop foreign table s cascade;
