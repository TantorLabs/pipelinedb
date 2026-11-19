from base import pipeline, clean_db

def test_part_agg_after_restart(pipeline, clean_db):
  pipeline.create_stream('s_metric_instances',
                         instance_id='smallint',
                         metric_key='text',
                         metric_val='jsonb',
                         metric_ts='timestamp(6) with time zone',
                         measure_interval='interval(6)',
                         )
  q = """
     SELECT max(s_metric_instances.metric_ts) AS metric_ts,
       s_metric_instances.metric_val ->> 'query_hash'::text AS query_hash,
       s_metric_instances.metric_val ->> 'plan_hash'::text AS plan_hash,
       (s_metric_instances.metric_val ->> 'query_id'::text)::bigint AS query_id,
       s_metric_instances.metric_val ->> 'plan'::text AS plan,
       s_metric_instances.instance_id
     FROM s_metric_instances
     WHERE s_metric_instances.metric_key = 'pg_store_plans'::text AND
       ((s_metric_instances.metric_val ->> 'plan_calls'::text)::bigint) > 0
     GROUP BY (s_metric_instances.metric_val ->> 'query_hash'::text),
       (s_metric_instances.metric_val ->> 'plan_hash'::text),
       ((s_metric_instances.metric_val ->> 'query_id'::text)::bigint),
       (s_metric_instances.metric_val ->> 'plan'::text),
       s_metric_instances.instance_id
  """

  pipeline.create_cv('cv_pg_store_plans_text_partitioned', q,
                     ttl='1 month', ttl_column='metric_ts',
                     partition_duration='1 day', partition_by='metric_ts')

  pipeline.insert('s_metric_instances', ['instance_id', 'metric_key',
                                         'metric_val', 'metric_ts',
                                         'measure_interval',
                                         'arrival_timestamp'],
                  [(2, 'pg_store_plans',
                    '{"query_hash": "bar", "plan_hash": "bar", "query_id": "456", "plan_calls": 10}',
                    '2024-05-22 11:00:01', '1 second', '2024-05-22 21:00:01')]
                  )

  result = pipeline.execute("""
SELECT * FROM cv_pg_store_plans_text_partitioned ORDER BY metric_ts DESC
  """)[0]
  assert result['metric_ts'].strftime('%Y-%m-%d %H:%M:%S') == '2024-05-22 11:00:01'

  pipeline.stop()
  pipeline.run()

  pipeline.insert('s_metric_instances', ['instance_id', 'metric_key',
                                         'metric_val', 'metric_ts',
                                         'measure_interval',
                                         'arrival_timestamp'],
                  [(3, 'pg_store_plans',
                    '{"query_hash": "foo", "plan_hash": "foo", "query_id": "123", "plan_calls": 10}',
                    '2024-05-23 11:00:01', '1 second', '2024-05-23 21:00:01')]
                  )

  result = pipeline.execute("""
SELECT * FROM cv_pg_store_plans_text_partitioned ORDER BY metric_ts DESC
  """)[0]
  assert result['metric_ts'].strftime('%Y-%m-%d %H:%M:%S') == '2024-05-23 11:00:01'

  pipeline.stop()
  pipeline.run()

  pipeline.insert('s_metric_instances', ['instance_id', 'metric_key',
                                         'metric_val', 'metric_ts',
                                         'measure_interval',
                                         'arrival_timestamp'],
                  [(4, 'pg_store_plans',
                    '{"query_hash": "baz", "plan_hash": "baz", "query_id": "789", "plan_calls": 10}',
                    '2024-05-24 11:00:01', '1 second', '2024-05-24 21:00:01')]
                  )

  result = pipeline.execute("""
SELECT * FROM cv_pg_store_plans_text_partitioned ORDER BY metric_ts DESC
  """)[0]
  assert result['metric_ts'].strftime('%Y-%m-%d %H:%M:%S') == '2024-05-24 11:00:01'

def test_part_convert_after_restart(pipeline, clean_db):
  pipeline.create_stream('s',
                         x='int',
                         ts='timestamp',
                         )
  pipeline.create_cv('cv', 'SELECt ts, COUNT(*) FROM s GROUP BY ts',
                     partition_duration='1 hour', partition_by='ts')

  pipeline.insert('s', ('x', 'ts'),
                  [
                    (1, '2024-11-11 08:31:42.42'),
                    (2, '2024-11-12 08:31:42.42'),
                    (3, '2024-11-13 08:31:42.42'),
                  ])

  # Restart the server
  pipeline.stop()
  pipeline.run()

  pipeline.insert('s', ('x', 'ts'),
                  [
                    (4, '2024-11-14 08:31:42.42'),
                    (5, '2024-11-15 08:31:42.42'),
                  ])

  mat_rel_id = pipeline.execute("""
  SELECT oid FROM pg_class WHERE relname='cv_mrel'
  """)[0]['oid']

  assert(int(mat_rel_id) > 0)

  part_name = 'mrel_{}_20241111080000_20241111090000'.format(mat_rel_id)

  # Now simulate a partition conversion to another access method by dropping it
  # and attaching a new table

  pipeline.execute("""
  CREATE TABLE newpart (LIKE {} INCLUDING ALL)""".format(part_name))

  pipeline.execute("""
  INSERT INTO newpart SELECT * FROM """ + part_name)

  pipeline.execute("""DROP TABLE """ + part_name)

  pipeline.execute("""ALTER TABLE newpart RENAME TO """ + part_name)

  pipeline.execute("""
  ALTER TABLE cv_mrel ATTACH PARTITION {}
  FOR VALUES FROM ('2024-11-11 08:00:00') TO ('2024-11-11 09:00:00')
  """.format(part_name))

  pipeline.insert('s', ('x', 'ts'),
                  [
                    (4, '2024-11-14 08:31:42.42'),
                    (5, '2024-11-15 08:31:42.42'),
                  ])

  result = pipeline.execute('SELECT * FROM cv_mrel ORDER BY count')

  assert(len(result) == 5)
  for i in range(3):
    assert(result[i]['count'] == 1)

  assert(result[3]['count'] == 2)
  assert(result[4]['count'] == 2)


def test_part_after_reattach(pipeline, clean_db):
  pipeline.create_stream('s', ts='timestamp')
  q = """
     SELECT ts FROM s
  """

  pipeline.create_cv('cv', q,
                     partition_duration='1 hour', partition_by='ts')

  pipeline.insert('s', ['ts'],
                  ["('2024-11-12 06:03:01.123')", "('2024-11-12 07:03:01.123')"]
                  )

  pipeline.stop()
  pipeline.run()

  pipeline.insert('s', ['ts'],
                  ["('2024-11-12 06:03:01.123')", "('2024-11-12 07:03:01.123')"]
                  )

  mat_rel_id = pipeline.execute("""
  SELECT oid FROM pg_class WHERE relname='cv_mrel'
  """)[0]['oid']

  assert(int(mat_rel_id) > 0)

  part_name = 'mrel_{}_20241112060000_20241112070000'.format(mat_rel_id)

  # Now simulate a partition conversion to another access method by dropping it
  # and attaching a new table

  pipeline.execute("""
  ALTER TABLE cv_mrel DETACH PARTITION {}
  """.format(part_name))

  pipeline.execute("""
  CREATE TABLE newpart (LIKE {} INCLUDING ALL)""".format(part_name))

  pipeline.execute("""DROP TABLE """ + part_name)

  pipeline.execute("""ALTER TABLE newpart RENAME TO """ + part_name)

  pipeline.execute("""
  ALTER TABLE cv_mrel ATTACH PARTITION {}
  FOR VALUES FROM ('2024-11-12 06:00:00') TO ('2024-11-12 07:00:00')
  """.format(part_name))

  pipeline.insert('s', ['ts'],
                  ["('2024-11-12 07:03:01.123')", "('2024-11-13 08:31:42.42')"]
                  )
