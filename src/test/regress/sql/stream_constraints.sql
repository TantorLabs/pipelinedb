--
-- Basic NOT NULL constraint
--
CREATE FOREIGN TABLE stream_notnull (
    id integer NOT NULL,
    payload text
) SERVER pipelinedb;

CREATE VIEW cv_test_notnull AS
SELECT id, count(*) FROM stream_notnull GROUP BY id;

INSERT INTO stream_notnull (id, payload) VALUES (1, 'valid data');

-- This should fail with a NOT NULL violation
INSERT INTO stream_notnull (id, payload) VALUES (NULL, 'invalid data');

SELECT * FROM cv_test_notnull ORDER BY id;

DROP FOREIGN TABLE stream_notnull CASCADE;

--
-- DEFAULT values for streams
--
CREATE FOREIGN TABLE stream_default (
    id integer NOT NULL,
    status integer DEFAULT 99,
    created_at timestamptz DEFAULT now()
) SERVER pipelinedb;

CREATE VIEW cv_test_default AS
SELECT id, status, created_at IS NOT NULL as has_timestamp
FROM stream_default;

-- Insert without specifying DEFAULT columns
INSERT INTO stream_default (id) VALUES (1);
INSERT INTO stream_default (id, status) VALUES (2, 100);

DROP FOREIGN TABLE stream_default CASCADE;

--
-- SERIAL column support
--
CREATE FOREIGN TABLE stream_serial (
    id SERIAL,
    user_id integer,
    value float
) SERVER pipelinedb;

CREATE VIEW cv_test_serial AS
SELECT user_id, count(*), min(id) as min_id, max(id) as max_id
FROM stream_serial GROUP BY user_id;

INSERT INTO stream_serial (user_id, value) VALUES (100, 1.5);
INSERT INTO stream_serial (user_id, value) VALUES (100, 2.5);
INSERT INTO stream_serial (user_id, value) VALUES (200, 3.5);

SELECT user_id, count, min_id, max_id FROM cv_test_serial ORDER BY user_id;

DROP FOREIGN TABLE stream_serial CASCADE;

--
-- Combined NOT NULL and DEFAULT
--
CREATE FOREIGN TABLE stream_combined (
    id integer NOT NULL,
    name text NOT NULL DEFAULT 'anonymous',
    active boolean DEFAULT true
) SERVER pipelinedb;

CREATE VIEW cv_test_combined AS
SELECT id, name, active FROM stream_combined;

INSERT INTO stream_combined (id) VALUES (1);

INSERT INTO stream_combined (id, name) VALUES (NULL, 'test');

INSERT INTO stream_combined (id, name) VALUES (2, NULL);

SELECT * FROM cv_test_combined ORDER BY id;

DROP FOREIGN TABLE stream_combined CASCADE;

--
-- Constraints other than NOT NULL and DEFAULT are not allowed
--

CREATE FOREIGN TABLE stream_unique (
    id integer UNIQUE,
    data text
) SERVER pipelinedb;

CREATE FOREIGN TABLE stream_pkey (
    id integer PRIMARY KEY,
    data text
) SERVER pipelinedb;

CREATE FOREIGN TABLE stream_check (
    id integer,
    value integer CHECK (value > 0)
) SERVER pipelinedb;

CREATE TABLE reference_table (id integer PRIMARY KEY);
CREATE FOREIGN TABLE stream_fkey (
    id integer,
    ref_id integer REFERENCES reference_table(id)
) SERVER pipelinedb;

DROP TABLE reference_table;

CREATE FOREIGN TABLE stream_exclude (
    id integer,
    period tstzrange,
    EXCLUDE USING gist (period WITH &&)
) SERVER pipelinedb;

--
-- ALTER FOREIGN TABLE ... SET DEFAULT support for streams
--

CREATE FOREIGN TABLE stream_alter_default (
    id integer NOT NULL,
    status integer,
    created timestamptz
) SERVER pipelinedb;

CREATE VIEW cv_alter_default AS
SELECT id, status, created IS NOT NULL as has_created
FROM stream_alter_default;

ALTER FOREIGN TABLE ONLY stream_alter_default
    ALTER COLUMN status SET DEFAULT 100;

ALTER FOREIGN TABLE ONLY stream_alter_default
    ALTER COLUMN created SET DEFAULT now();

INSERT INTO stream_alter_default (id) VALUES (1);
INSERT INTO stream_alter_default (id, status) VALUES (2, 200);
INSERT INTO stream_alter_default (id, status, created)
    VALUES (3, 300, '2023-01-01 00:00:00'::timestamptz);

SELECT id, status, has_created
FROM cv_alter_default
WHERE id IN (1, 2, 3)
ORDER BY id;

ALTER FOREIGN TABLE ONLY stream_alter_default
    ALTER COLUMN status SET DEFAULT 999;

INSERT INTO stream_alter_default (id) VALUES (4);

SELECT id, status
FROM cv_alter_default
WHERE id = 4;

--
-- ALTER FOREIGN TABLE ... DROP DEFAULT support for streams
--
ALTER FOREIGN TABLE ONLY stream_alter_default
    ALTER COLUMN status DROP DEFAULT;

INSERT INTO stream_alter_default (id) VALUES (5);

SELECT id, status
FROM cv_alter_default
WHERE id = 5;

--
-- Unsupported ALTER FOREIGN TABLE ... on streams
--
ALTER FOREIGN TABLE ONLY stream_alter_default
    ADD CONSTRAINT fake_constraint UNIQUE (id);

DROP FOREIGN TABLE stream_alter_default CASCADE;
