from base import pipeline, clean_db
import time


def _get_bg_worker_count(pipeline):
  """Return the number of running PipelineDB background workers."""
  result = pipeline.execute(
    "SELECT count(*) FROM pg_stat_activity "
    "WHERE backend_type LIKE 'worker%' OR backend_type LIKE 'combiner%'")
  return result[0][0]


def _get_index_oids(pipeline, cv_name):
  """Return (pkidxid, lookupidxid) for a continuous view from pipelinedb.cont_query."""
  result = pipeline.execute(
    "SELECT pkidxid, lookupidxid FROM pipelinedb.cont_query "
    "WHERE relid = (SELECT oid FROM pg_class WHERE relname = '%s')" % cv_name)
  return result[0]['pkidxid'], result[0]['lookupidxid']


def _oid_exists(pipeline, oid):
  """Check whether an OID refers to a real pg_class entry."""
  result = pipeline.execute(
    "SELECT count(*) FROM pg_class WHERE oid = %d" % oid)
  return result[0][0] == 1


def test_reindex_grouped_cv(pipeline, clean_db):
  """
  REINDEX CONCURRENTLY on a grouped CV's matrel must update the catalog
  index OIDs and allow continued inserts.
  """
  pipeline.create_stream('stream0', k='text', v='int')
  pipeline.create_cv('test_ri_grouped',
    "SELECT k::text, COUNT(*), SUM(v::int) FROM stream0 GROUP BY k")

  pipeline.insert('stream0', ['k', 'v'], [('a', 1), ('b', 2)])

  result = pipeline.execute('SELECT * FROM test_ri_grouped ORDER BY k')
  assert len(result) == 2
  assert result[0]['k'] == 'a' and result[0]['count'] == 1
  assert result[1]['k'] == 'b' and result[1]['count'] == 1

  old_pk, old_lookup = _get_index_oids(pipeline, 'test_ri_grouped')
  assert old_pk != 0
  assert old_lookup != 0

  workers_before = _get_bg_worker_count(pipeline)

  pipeline.execute('REINDEX TABLE CONCURRENTLY test_ri_grouped_mrel')

  time.sleep(2)

  workers_after = _get_bg_worker_count(pipeline)
  assert workers_after >= workers_before, \
    'workers dropped from %d to %d after REINDEX' % (workers_before, workers_after)

  new_pk, new_lookup = _get_index_oids(pipeline, 'test_ri_grouped')
  assert new_pk != old_pk, 'pkidxid was not updated after REINDEX CONCURRENTLY'
  assert new_lookup != old_lookup, 'lookupidxid was not updated after REINDEX CONCURRENTLY'
  assert _oid_exists(pipeline, new_pk), 'new pkidxid does not exist'
  assert _oid_exists(pipeline, new_lookup), 'new lookupidxid does not exist'

  pipeline.insert('stream0', ['k', 'v'], [('a', 10), ('c', 30)])

  result = pipeline.execute('SELECT * FROM test_ri_grouped ORDER BY k')
  assert len(result) == 3
  row_a = [r for r in result if r['k'] == 'a'][0]
  assert row_a['count'] == 2
  assert row_a['sum'] == 11


def test_reindex_sw_cv(pipeline, clean_db):
  """
  REINDEX CONCURRENTLY on a sliding-window CV's matrel must update the
  catalog index OIDs (ls_hash_group lookup) and preserve data integrity.
  """
  pipeline.create_stream('stream0', k='text', v='int')
  pipeline.create_cv('test_ri_sw',
    "SELECT k::text, COUNT(*), SUM(v::int) FROM stream0 "
    "WHERE (arrival_timestamp > clock_timestamp() - interval '1 hour') "
    "GROUP BY k")

  pipeline.insert('stream0', ['k', 'v'], [('x', 5), ('y', 10)])

  result = pipeline.execute('SELECT * FROM test_ri_sw ORDER BY k')
  assert len(result) == 2

  old_pk, old_lookup = _get_index_oids(pipeline, 'test_ri_sw')
  assert old_pk != 0
  assert old_lookup != 0

  workers_before = _get_bg_worker_count(pipeline)

  pipeline.execute('REINDEX TABLE CONCURRENTLY test_ri_sw_mrel')

  time.sleep(2)

  workers_after = _get_bg_worker_count(pipeline)
  assert workers_after >= workers_before

  new_pk, new_lookup = _get_index_oids(pipeline, 'test_ri_sw')
  assert new_pk != old_pk, 'pkidxid was not updated'
  assert new_lookup != old_lookup, 'lookupidxid was not updated'
  assert _oid_exists(pipeline, new_pk)
  assert _oid_exists(pipeline, new_lookup)

  pipeline.insert('stream0', ['k', 'v'], [('x', 50), ('z', 200)])

  result = pipeline.execute('SELECT * FROM test_ri_sw ORDER BY k')
  assert len(result) == 3
  row_x = [r for r in result if r['k'] == 'x'][0]
  assert row_x['count'] == 2
  assert row_x['sum'] == 55


def test_reindex_ungrouped_cv(pipeline, clean_db):
  """
  REINDEX CONCURRENTLY on an ungrouped CV's matrel (PK index only,
  lookupidxid = 0) must update pkidxid and preserve data.
  """
  pipeline.create_stream('stream0', v='int')
  pipeline.create_cv('test_ri_ungrouped',
    "SELECT COUNT(*), SUM(v::int) FROM stream0")

  pipeline.insert('stream0', ['v'], [(1,), (2,), (3,)])

  result = pipeline.execute('SELECT * FROM test_ri_ungrouped')
  assert result[0]['count'] == 3
  assert result[0]['sum'] == 6

  old_pk, old_lookup = _get_index_oids(pipeline, 'test_ri_ungrouped')
  assert old_pk != 0
  assert old_lookup == 0, 'ungrouped CV should not have a lookup index'

  pipeline.execute('REINDEX TABLE CONCURRENTLY test_ri_ungrouped_mrel')

  time.sleep(2)

  new_pk, new_lookup = _get_index_oids(pipeline, 'test_ri_ungrouped')
  assert new_pk != old_pk, 'pkidxid was not updated'
  assert new_lookup == 0, 'lookupidxid should remain 0'
  assert _oid_exists(pipeline, new_pk)

  pipeline.insert('stream0', ['v'], [(4,)])

  result = pipeline.execute('SELECT * FROM test_ri_ungrouped')
  assert result[0]['count'] == 4
  assert result[0]['sum'] == 10


def test_reindex_multiple_cvs(pipeline, clean_db):
  """
  REINDEX CONCURRENTLY when multiple CVs share the same stream. All CVs
  should remain functional and have valid catalog entries.
  """
  pipeline.create_stream('stream0', k='text', v='int')
  pipeline.create_cv('test_ri_multi1',
    "SELECT k::text, COUNT(*) FROM stream0 GROUP BY k")
  pipeline.create_cv('test_ri_multi2',
    "SELECT k::text, SUM(v::int) FROM stream0 GROUP BY k")

  pipeline.insert('stream0', ['k', 'v'], [('a', 10), ('b', 20)])

  pk1_old, lk1_old = _get_index_oids(pipeline, 'test_ri_multi1')
  pk2_old, lk2_old = _get_index_oids(pipeline, 'test_ri_multi2')

  pipeline.execute('REINDEX TABLE CONCURRENTLY test_ri_multi1_mrel')
  pipeline.execute('REINDEX TABLE CONCURRENTLY test_ri_multi2_mrel')

  time.sleep(2)

  pk1_new, lk1_new = _get_index_oids(pipeline, 'test_ri_multi1')
  pk2_new, lk2_new = _get_index_oids(pipeline, 'test_ri_multi2')

  assert pk1_new != pk1_old
  assert lk1_new != lk1_old
  assert pk2_new != pk2_old
  assert lk2_new != lk2_old

  assert _oid_exists(pipeline, pk1_new)
  assert _oid_exists(pipeline, lk1_new)
  assert _oid_exists(pipeline, pk2_new)
  assert _oid_exists(pipeline, lk2_new)

  pipeline.insert('stream0', ['k', 'v'], [('a', 100), ('c', 300)])

  r1 = pipeline.execute('SELECT * FROM test_ri_multi1 ORDER BY k')
  assert len(r1) == 3
  assert [r['k'] for r in r1] == ['a', 'b', 'c']

  r2 = pipeline.execute('SELECT * FROM test_ri_multi2 ORDER BY k')
  assert len(r2) == 3
  row_a = [r for r in r2 if r['k'] == 'a'][0]
  assert row_a['sum'] == 110


def test_reindex_workers_survive(pipeline, clean_db):
  """
  After REINDEX CONCURRENTLY, background workers and combiners must
  remain alive and continue processing inserts.
  """
  pipeline.create_stream('stream0', x='int')
  pipeline.create_cv('test_ri_survive',
    "SELECT x::int, COUNT(*) FROM stream0 GROUP BY x")

  workers_before = _get_bg_worker_count(pipeline)
  assert workers_before > 0

  pipeline.insert('stream0', ['x'], [(i,) for i in range(100)])

  pipeline.execute('REINDEX TABLE CONCURRENTLY test_ri_survive_mrel')

  time.sleep(3)

  workers_after = _get_bg_worker_count(pipeline)
  assert workers_after >= workers_before, \
    'expected >= %d workers, got %d' % (workers_before, workers_after)

  pipeline.insert('stream0', ['x'], [(i,) for i in range(100)])

  result = pipeline.execute(
    'SELECT sum(count) AS total FROM test_ri_survive')
  assert result[0]['total'] == 200

  pipeline.insert('stream0', ['x'], [(i,) for i in range(100)])

  result = pipeline.execute(
    'SELECT sum(count) AS total FROM test_ri_survive')
  assert result[0]['total'] == 300
