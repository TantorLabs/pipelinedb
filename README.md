> **Note:** **Tantor PipelineDB** is a fork of the [original PipelineDB extension](https://github.com/pipelinedb/pipelinedb). In addition to compatibility with newer PostgreSQL versions, it includes:
> - Support for [partitioning](https://docs.tantorlabs.ru/pipelinedb/en/1.3-en/continuous-views.html#partitioning) of continuous views
> - Support for archiving continuous view partitions in Columnar format (only for the [Tantor Server](https://docs.tantorlabs.ru/tdb/en/latest/se/differences.html) now)

# Tantor PipelineDB

## Overview

Tantor PipelineDB is a PostgreSQL extension for high-performance time-series aggregation, designed to power realtime reporting and analytics applications.

Tantor PipelineDB allows you to define [continuous SQL queries](https://docs.tantorlabs.ru/pipelinedb/en/current/continuous-views.html) that perpetually aggregate time-series data and store **only the aggregate output** in regular, queryable tables. You can think of this concept as extremely high-throughput, incrementally updated materialized views that never need to be manually refreshed.

Raw time-series data is never written to disk, making Tantor PipelineDB extremely efficient for aggregation workloads.

Continuous queries produce their own [output streams](https://docs.tantorlabs.ru/pipelinedb/en/current/streams.html#output-streams), and thus can be [chained together](https://docs.tantorlabs.ru/pipelinedb/en/current/continuous-transforms.html) into arbitrary networks of continuous SQL.

## PostgreSQL compatibility

Tantor PipelineDB runs on 64-bit architectures and currently supports the following PostgreSQL versions:

* **PostgreSQL 13** 
* **PostgreSQL 15**
* **PostgreSQL 16**

Going forward, we plan to maintain it for all supported PostgreSQL versions at the time each Tantor PipelineDB release is out.

## Changelog

See [CHANGELOG.md](CHANGELOG.md).

## Getting started

If you just want to start using Tantor PipelineDB right away, head over to the [installation docs](https://docs.tantorlabs.ru/pipelinedb/en/current/installation.html) to get going.

If you'd like to build Tantor PipelineDB from source, keep reading!

## Building from source

Since Tantor PipelineDB is a PostgreSQL extension, you'll need to have the [PostgreSQL development packages](https://www.postgresql.org/download/) installed to build Tantor PipelineDB.

Next you'll have to install [ZeroMQ](http://zeromq.org/) which Tantor PipelineDB uses for inter-process communicatio:

- on Debian-based distributions: `apt install libzmq3-dev`
- on Fedora: `dnf install zeromq-devel`

Head over to [ZeroMQ project website](https://zeromq.org/download/) for installation instructions for other distributions.

You'll also need to install some Python dependencies if you'd like to run Tantor PipelineDB's Python test suite:

```
pip install -r src/test/py/requirements.txt
```

#### Build Tantor PipelineDB:

Once PostgreSQL is installed, you can build Tantor PipelineDB against it:

```
export PATH=$PATH:/usr/local/pgsql/bin
make USE_PGXS=1
make install
```

#### Test Tantor PipelineDB *(optional)*
Run the following command:

```
make USE_PGXS=1 test
```

#### Bootstrap the Tantor PipelineDB environment
Create Tantor PipelineDB's physical data directories, configuration files, etc:

```
make bootstrap
```

**`make bootstrap` only needs to be run the first time you install Tantor PipelineDB**. The resources that `make bootstrap` creates may continue to be used as you change and rebuild Tantor PipeineDB.


#### Run Tantor PipelineDB
Run all of the daemons necessary for Tantor PipelineDB to operate:

```
make run
```

Enter `Ctrl+C` to shut down Tantor PipelineDB.

`make run` uses the binaries in the Tantor PipelineDB source root compiled by `make`, so you don't need to `make install` before running `make run` after code changes--only `make` needs to be run.

The basic development flow is:

```
make
make run
^C

# Make some code changes...
make
make run
```

#### Send Tantor PipelineDB some data

Now let's generate some test data and stream it into a simple continuous view. First, create the stream and the continuous view that reads from it:

    $ psql
    =# CREATE FOREIGN TABLE test_stream (key integer, value integer) SERVER pipelinedb;
    CREATE FOREIGN TABLE
    =# CREATE VIEW test_view WITH (action=materialize) AS SELECT key, COUNT(*) FROM test_stream GROUP BY key;
    CREATE VIEW

Events can be emitted to Tantor PipelineDB streams using regular SQL `INSERTS`. Any `INSERT` target that isn't a table is considered a stream by Tantor PipelineDB, meaning streams don't need to have a schema created in advance. Let's emit a single event into the `test_stream` stream since our continuous view is reading from it:

    $ psql
    =# INSERT INTO test_stream (key, value) VALUES (0, 42);
    INSERT 0 1

The 1 in the `INSERT 0 1` response means that 1 event was emitted into a stream that is actually being read by a continuous query. Now let's insert some random data:

    =# INSERT INTO test_stream (key, value) SELECT random() * 10, random() * 10 FROM generate_series(1, 100000);
    INSERT 0 100000

Query the continuous view to verify that the continuous view was properly updated. Were there actually 100,001 events counted?

    $ psql -c "SELECT sum(count) FROM test_view"
      sum
    -------
    100001
    (1 row)

What were the 10 most common randomly generated keys?

    $ psql -c "SELECT * FROM test_view ORDER BY count DESC limit 10"
	key  | count 
	-----+-------
	 2   | 10124
	 8   | 10100
	 1   | 10042
	 7   |  9996
	 4   |  9991
	 5   |  9977
	 3   |  9963
	 6   |  9927
	 9   |  9915
	10   |  4997
	 0   |  4969

	(11 rows)
