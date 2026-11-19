-- Test REINDEX CONCURRENTLY on continuous view materialization tables.
CREATE FOREIGN TABLE reindex_stream (k text, v int) SERVER pipelinedb;

-------------------------------------------------------------------------------
-- Grouped continuous view (hash_group lookup index + PK index)
CREATE VIEW reindex_cv AS
  SELECT k::text, COUNT(*), SUM(v::int) FROM reindex_stream GROUP BY k;

INSERT INTO reindex_stream (k, v) VALUES ('a', 1), ('b', 2), ('c', 3);
INSERT INTO reindex_stream (k, v) VALUES ('a', 10), ('b', 20), ('c', 30);

SELECT * FROM reindex_cv ORDER BY k;

-- REINDEX CONCURRENTLY replaces indexes with new OIDs
REINDEX TABLE CONCURRENTLY reindex_cv_mrel;

-- Verify data integrity survives the reindex
INSERT INTO reindex_stream (k, v) VALUES ('a', 100), ('b', 200), ('d', 400);

SELECT * FROM reindex_cv ORDER BY k;

-- Verify catalog index OIDs still point to valid relations
SELECT
  (SELECT count(*) FROM pg_class WHERE oid = cq.pkidxid) = 1     AS pk_idx_valid,
  (SELECT count(*) FROM pg_class WHERE oid = cq.lookupidxid) = 1 AS lookup_idx_valid
FROM pipelinedb.cont_query cq
WHERE cq.relid = (SELECT oid FROM pg_class WHERE relname = 'reindex_cv');

DROP VIEW reindex_cv;

-------------------------------------------------------------------------------
-- Sliding-window continuous view with GROUP BY (ls_hash_group lookup index)
CREATE VIEW reindex_cv_sw AS
  SELECT k::text, COUNT(*), SUM(v::int) FROM reindex_stream
  WHERE (arrival_timestamp > clock_timestamp() - interval '1 hour')
  GROUP BY k;

INSERT INTO reindex_stream (k, v) VALUES ('x', 5), ('y', 10);

SELECT * FROM reindex_cv_sw ORDER BY k;

REINDEX TABLE CONCURRENTLY reindex_cv_sw_mrel;

INSERT INTO reindex_stream (k, v) VALUES ('x', 50), ('y', 100), ('z', 200);

SELECT * FROM reindex_cv_sw ORDER BY k;

SELECT
  (SELECT count(*) FROM pg_class WHERE oid = cq.pkidxid) = 1     AS pk_idx_valid,
  (SELECT count(*) FROM pg_class WHERE oid = cq.lookupidxid) = 1 AS lookup_idx_valid
FROM pipelinedb.cont_query cq
WHERE cq.relid = (SELECT oid FROM pg_class WHERE relname = 'reindex_cv_sw');

DROP FOREIGN TABLE reindex_stream CASCADE;
