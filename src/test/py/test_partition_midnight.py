"""Reproduce combiner updates that cross a UTC partition boundary."""

import pytest

from base import pipeline, clean_db
from test_crash_recovery import get_combiner_pids


@pytest.mark.parametrize('block', [0, 43], ids=['ctid-collision', 'missing-page'])
@pytest.mark.parametrize('daily_group', [False, True], ids=['no-date-group', 'daily-group'])
def test_group_update_across_midnight(pipeline, clean_db, block, daily_group):
  # Use event timestamps around UTC midnight. The test does not change the
  # machine clock and runs against an isolated PostgreSQL instance.
  pipeline.stop()
  pipeline.run({'pipelinedb.num_combiners': 1, 'pipelinedb.num_workers': 1})
  pipeline.execute("SET TimeZone = 'UTC'")
  pipeline.execute("SET statement_timeout = '10s'")
  pipeline.create_stream('midnight_stream', instance_id='int', metric_ts='timestamptz', value='bigint')
  grouping = "instance_id"
  if daily_group:
    grouping += ", (metric_ts AT TIME ZONE 'UTC')::date"
  pipeline.create_cv('midnight_cv',
      'SELECT instance_id, max(metric_ts) AS metric_ts, max(value) AS value '
      'FROM midnight_stream GROUP BY ' + grouping,
      partition_duration='1 day', partition_by='metric_ts')

  day = pipeline.execute("SELECT (now() AT TIME ZONE 'UTC')::date::text AS day")[0]['day']
  pipeline.execute("""
    CREATE TABLE midnight_yesterday PARTITION OF midnight_cv_mrel
    FOR VALUES FROM ('%s'::timestamptz - interval '1 day') TO ('%s'::timestamptz)
  """ % (day, day))

  if block:
    # Allocate page 43 while keeping the measured row on the stream path.
    # Reclaim its first slot without truncating that trailing heap page.
    extra_col = ', _0' if daily_group else ''
    extra_value = ", '%s'::date - 1" % day if daily_group else ''
    pipeline.execute("""
      DO $$ DECLARE i int; pos tid; BEGIN
        FOR i IN 1..20000 LOOP
          INSERT INTO midnight_yesterday (instance_id, metric_ts, value, "$pk"%s)
          VALUES (-30000, '%s'::timestamptz - interval '1 minute', 0, -i%s)
          RETURNING ctid INTO pos;
          IF pos::text = '(43,1)' THEN
            DELETE FROM midnight_yesterday WHERE ctid = pos;
            RETURN;
          END IF;
        END LOOP;
        RAISE EXCEPTION 'Could not allocate CTID (43,1)';
      END $$
    """ % (extra_col, day, extra_value))
    pipeline.execute('VACUUM (TRUNCATE FALSE) midnight_yesterday')

  def emit(instance, timestamp, value):
    pipeline.execute("INSERT INTO midnight_stream VALUES (%d, '%s'::timestamptz, %d)" %
        (instance, timestamp, value))

  def snapshot():
    return [(r['instance_id'], r['day'], r['value']) for r in pipeline.execute("""
      SELECT instance_id, (metric_ts AT TIME ZONE 'UTC')::date::text AS day, value
      FROM midnight_cv WHERE instance_id IN (-1, 1) ORDER BY instance_id, metric_ts
    """)]

  yesterday = pipeline.execute("SELECT ('%s'::date - 1)::text AS day" % day)[0]['day']
  emit(1, yesterday + ' 23:59:00+00', 100)
  old_ctid = pipeline.execute(
      'SELECT ctid::text AS pos FROM midnight_yesterday WHERE instance_id = 1')[0]['pos']
  assert old_ctid == '(%d,1)' % block
  pipeline.execute('DELETE FROM midnight_yesterday WHERE instance_id = -30000')

  emit(-1, day + ' 00:00:30+00', 777)
  control_ctid = pipeline.execute(
      'SELECT ctid::text AS pos FROM midnight_cv_mrel WHERE instance_id = -1')[0]['pos']
  assert control_ctid == '(0,1)'
  before = snapshot()
  if not daily_group:
    pids = get_combiner_pids(pipeline)
    assert len(pids) == 1
    log_start = len(pipeline.server_log)
    pipeline.execute("SET pipelinedb.stream_insert_level = 'sync_receive'")
  # A different group keeps yesterday in the same combiner lookup even
  # after the worker aggregates the stream batch by instance_id.
  pipeline.insert('midnight_stream', ('instance_id', 'metric_ts', 'value'), [
      (2, yesterday + ' 23:59:30+00', 10),
      (1, day + ' 00:01:00+00', 200),
  ])
  if daily_group:
    assert snapshot() == [(-1, day, 777), (1, yesterday, 100), (1, day, 200)]
    return

  pipeline.wait_for_log(
      b'cross-partition update of materialization table "midnight_cv_mrel" is not supported',
      log_start)
  pipeline.wait_for_log(
      ('The updated row %s of partition "midnight_yesterday"' % old_ctid).encode(),
      log_start)
  assert snapshot() == before
  pipeline.execute("RESET pipelinedb.stream_insert_level")
  emit(-1, day + ' 00:02:00+00', 778)
  assert snapshot() == [(-1, day, 778), (1, yesterday, 100)]
  assert get_combiner_pids(pipeline) == pids
