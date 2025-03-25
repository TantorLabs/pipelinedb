drop function if exists list_partitions(name);
create function
  list_partitions(
    baserel name
  )
  returns table (
    parent name, child text, options text[]
  )
  as $BODY$
    begin
      return query select
        parent.relname      as parent,
        regexp_replace(child.relname, 'mrel_[0-9]+_', 'mrel_OID_')       as child,
        child.reloptions as options
      from pg_inherits
        join pg_class parent        on pg_inherits.inhparent = parent.oid
        join pg_class child         on pg_inherits.inhrelid   = child.oid
        join pg_namespace nmsp_parent   on nmsp_parent.oid  = parent.relnamespace
        join pg_namespace nmsp_child    on nmsp_child.oid   = child.relnamespace
      where
        parent.relname = baserel || '_mrel'
      order by child
      ;
    end;
  $BODY$
language plpgsql;

create foreign table part_ff_stream (ts timestamp, x int) server pipelinedb;

create view cv_part_ff
with (partition_by=ts, partition_duration='1 hour')
as select ts, count(*) from part_ff_stream group by ts;

\d+ cv_part_ff_mrel;

insert into part_ff_stream values ('2024-11-12 10:18:42', 1);

select * from list_partitions('cv_part_ff');

insert into part_ff_stream values ('2024-11-12 20:18:42', 2);

select * from list_partitions('cv_part_ff');

drop foreign table part_ff_stream cascade;
