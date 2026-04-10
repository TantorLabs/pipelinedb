from base import pipeline, clean_db
import getpass
import psycopg2
import random
import threading
import time


def test_concurrent_add_drop(pipeline, clean_db):
  """
  Adds and drops continuous views while inserting into a stream so that we
  see add/drops in the middle of transactions in workers and combiners.
  """
  pipeline.create_stream('stream0', x='int')
  q = 'SELECT x::int,  COUNT(*) FROM stream0 GROUP BY x'
  pipeline.create_cv('cv', q)

  stop = False
  values = [(x,) for x in range(10000)]
  num_inserted = [0]

  def insert():
    while True:
      if stop:
        break
      pipeline.insert('stream0', ['x'], values)
      num_inserted[0] += 1

  def add_drop(prefix):
    # Don't share the connection object with the insert thread because we want
    # these queries to happen in parallel.
    conn = psycopg2.connect('dbname=postgres user=%s host=localhost port=%s' %
                (getpass.getuser(), pipeline.port))
    add = 'CREATE VIEW %s AS ' + q
    drop = 'DROP VIEW %s'
    cur = conn.cursor()
    cvs = []
    while True:
      if stop:
        break
      if not cvs:
        cv = '%s%s' % (prefix, str(random.random())[2:])
        try:
          cur.execute(add % cv)
        except psycopg2.errors.DeadlockDetected:
          pass
        cvs.append(cv)
      elif len(cvs) > 10:
        try:
          cur.execute(drop % cvs.pop())
        except (psycopg2.errors.DeadlockDetected, psycopg2.errors.UndefinedTable):
          pass
      else:
        r = random.random()
        if r > 0.5:
          cv = '%s%s' % (prefix, str(r)[2:])
          try:
            cur.execute(add % cv)
          except psycopg2.errors.DeadlockDetected:
            pass
          cvs.append(cv)
        else:
          try:
            cur.execute(drop % cvs.pop())
          except (psycopg2.errors.UndefinedTable, psycopg2.errors.DeadlockDetected):
            pass
      conn.commit()
      time.sleep(0.0025)
    cur.close()
    conn.close()

  threads = [threading.Thread(target=insert),
         threading.Thread(target=add_drop, args=('cv1_',)),
         threading.Thread(target=add_drop, args=('cv2_',))]

  [t.start() for t in threads]

  time.sleep(10)
  stop = True

  [t.join() for t in threads]

  views = pipeline.execute('SELECT name FROM pipelinedb.get_views()')
  mrels = pipeline.execute("SELECT relname FROM pg_class WHERE relname LIKE 'cv%%_mrel'")
  assert len(views) == len(mrels)

  counts = pipeline.execute('SELECT * FROM cv')
  assert len(counts) == 10000
  # Worker crashes from deadlocks can cause batches to be lost, so the count
  # may be less than num_inserted. Verify all groups saw the same number of
  # batches and that the count is within a valid range.
  actual_counts = set(r['count'] for r in counts)
  assert len(actual_counts) == 1, 'all groups must have the same count, got %s' % actual_counts
  actual = actual_counts.pop()
  assert 0 < actual <= num_inserted[0], \
    'count %d out of range (0, %d]' % (actual, num_inserted[0])
