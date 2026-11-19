CREATE FOREIGN TABLE ttl_stream (x integer) SERVER pipelinedb;

CREATE VIEW ttl0 WITH (ttl='3 seconds', ttl_column='ts',
                       partition_by='ts', partition_duration='1 second')
	AS SELECT arrival_timestamp AS ts, x FROM ttl_stream;

SELECT c.relname, ttl, ttl_attno FROM pipelinedb.cont_query cq
JOIN pg_class c ON c.oid = cq.relid WHERE c.relname = 'ttl0'
ORDER BY c.relname;

INSERT INTO ttl_stream (x) VALUES (0);
INSERT INTO ttl_stream (x) VALUES (1);
INSERT INTO ttl_stream (x) VALUES (2);

SELECT x, "$pk" FROM ttl0_mrel ORDER BY ts;

SELECT pg_sleep(3);

SELECT 0 * pipelinedb.ttl_expire('ttl0');

SELECT x, "$pk" FROM ttl0_mrel ORDER BY ts;

INSERT INTO ttl_stream (x) VALUES (0);
INSERT INTO ttl_stream (x) VALUES (1);
INSERT INTO ttl_stream (x) VALUES (2);

SELECT x, "$pk" FROM ttl0_mrel ORDER BY ts;

SELECT pg_sleep(3);
SELECT 0 * pipelinedb.ttl_expire('ttl0');

SELECT x, "$pk" FROM ttl0_mrel ORDER BY ts;

DROP VIEW ttl0;

CREATE VIEW ttl2 WITH (partition_by='ts', partition_duration='1 second')
    AS SELECT arrival_timestamp AS ts, x FROM ttl_stream;

DROP VIEW ttl2;

DROP FOREIGN TABLE ttl_stream CASCADE;
