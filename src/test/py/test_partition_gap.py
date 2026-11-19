"""Exercise inserts into a gap after a non-UTC historical partition bound."""

import pytest

from base import pipeline, clean_db
from test_crash_recovery import get_combiner_pids


@pytest.mark.parametrize('end_hour', [21, 24], ids=['three-hour-gap', 'continuous-bounds'])
def test_insert_after_historical_partition(pipeline, clean_db, end_hour):
  pipeline.stop()
  pipeline.run({
      'pipelinedb.num_combiners': 1,
      'pipelinedb.num_workers': 1,
      'TimeZone': 'UTC',
      'DateStyle': 'ISO, YMD',
  })
  pipeline.execute("SET statement_timeout = '5s'")
  pipeline.create_stream('gap_stream', instance_id='int', metric_ts='timestamptz', value='bigint')
  # No aggregation: this regression is independent of cross-partition group updates.
  pipeline.create_cv('gap_cv', 'SELECT instance_id, metric_ts, value FROM gap_stream',
      partition_duration='1 day', partition_by='metric_ts')
  day = pipeline.execute("SELECT (now() AT TIME ZONE 'UTC')::date::text AS day")[0]['day']
  pipeline.execute("""
    CREATE TABLE gap_historical PARTITION OF gap_cv_mrel
    FOR VALUES FROM ('%s'::timestamptz - interval '1 day')
    TO ('%s'::timestamptz + interval '%s hours');
    CREATE TABLE gap_tomorrow PARTITION OF gap_cv_mrel
    FOR VALUES FROM ('%s'::timestamptz + interval '1 day')
    TO ('%s'::timestamptz + interval '2 days');
  """ % (day, day, end_hour, day, day))

  if end_hour == 21:
    pids = get_combiner_pids(pipeline)
    assert len(pids) == 1
    log_start = len(pipeline.server_log)
    pipeline.execute("SET pipelinedb.stream_insert_level = 'sync_receive'")
  pipeline.execute("""
    INSERT INTO gap_stream VALUES
      (1, '%s'::timestamptz + interval '22 hours', 42)
  """ % day)
  if end_hour == 24:
    row, = pipeline.execute('SELECT instance_id, value FROM gap_cv')
    assert (row['instance_id'], row['value']) == (1, 42)
    return

  pipeline.wait_for_log(
      b'no partition of materialization table "gap_cv_mrel" found for row', log_start)
  pipeline.wait_for_log(
      ('Partition key of the failing row contains (metric_ts) = (%s 22:00:00+00).' %
       day).encode(), log_start)
  assert pipeline.execute('SELECT instance_id FROM gap_cv') == []
  pipeline.execute("RESET pipelinedb.stream_insert_level")
  pipeline.execute("""
    INSERT INTO gap_stream VALUES (2, '%s 20:00:00+00'::timestamptz, 43)
  """ % day)
  row, = pipeline.execute('SELECT instance_id, value FROM gap_cv')
  assert (row['instance_id'], row['value']) == (2, 43)
  assert get_combiner_pids(pipeline) == pids
