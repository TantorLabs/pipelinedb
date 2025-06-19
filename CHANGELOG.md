# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.3.5] - 2025-06-18

### Added

- Human-readable changelog in CHANGELOG.md (GL-39).

### Fixed

- Regression introduced in 1.3.4 leading to assertion failures in reaper
  processes on cassert-enabled PostgreSQL builds (GL-40).
- Two potential cases of uninitialized variables usage reported by Clang
  (GL-33).
- Development and testing scripts were ported to Python 3 (GL-41).
- Combiner processes used the server timezone when defining range bounds for
  automatically created partitions, if the partitioning attribute was of the
  TIMESTAMP WITH TIME ZONE type. That resulted in problems caused by overlapping
  or disjoint partition ranges after the default server timezone has changed for
  whatever reasons. Fixed by sticking to UTC when rounding timestamptz values to
  calculate partition range bounds (GL-44).
- The sliding-window queries could work incorrectly or lead to a combiner
  process crash (GL-50).

## [1.3.4] - 2025-02-20

### Fixed

- If the pg_store_plans extension was installed together with Tantor PipelineDB,
  it could result in combiner failures when processing query execution plans
  created by PipelineDB for internal purposes. Fixed by configuring
  pg_store_plans to avoid examining execution plans created by PipelineDB
  background workers (GL-35, GL-36).
- If Time-To-Live option was configured for partitioned CVs with some partitions
  converted to Hydra Columnar, the repear processes could end up in a broken
  state when deleting tuples from Columnar partitions. Fixed by disabling the
  TTL feature for partitioned CVs until partition-aware reaper functionality is
  implemented in a later release (GL-34).
- README.md is now up-to-date.

## [1.3.3] - 2024-12-24

### Fixed

- PostgreSQL 15.9 introduced an incompatible change resulting in the "missing
lock for relation" warning message printed in the log during ALTER VIEW or ALTER
SCHEMA commands in cassert-enabled PipelineDB builds when CVs were involved
(GL-12).
- Typo in 1.3.1 -> 1.3.2 extension upgrade script name.

## [1.3.2] - 2024-12-05

### Added

- New GUC `pipelinedb.index_fillfactor` controls fillfactor for automatically
  created indexes for CV materialized relations. This applies to both
  partitioned and regular CVs (GL-15).

## [1.3.1] - 2024-11-20

### Fixed

- The `pipelinedb.fillfactor` GUC did not work for partitioned CVs (GL-14).
- For partitioned CVs with aggregates, combiner processes could sometimes add
  new tuples with instead of updating existing ones (GL-17).
- Combiner processes could leak memory, if a partitioned CV update failed with
  an error for whatever reasons (GL-18).
- Converting a partition of a partitioned CV to Hydra Columnar could fail with
  the "could not open relation with OID" error.
- After converting a partition of a partitioned CV to Hydra Columnar combiner
  processes cound end up in a broken state, requiring a restart to continue
  processing data (GL-20).

### Removed

- The "anonymous update checks" functionality that would periodically check for
  PipelineDB updates in the background and provide anonymized statistics at
  the same time (GL-16).

## [1.3.0] - 2024-11-12

This is the first formal Tantor release.

### Added

- Support for PostgreSQL 15.
- Support for partitioned continuous views.
- Support for converting CV partitions to other table access methods such as
  Hydra Columnar.

### Fixed

- A number of memory management and functionality bugs revealed by
  AddressSanitizer and UndefinedBehaviorSanitizer.

## [1.0.0-13] - 2019-02-05

This was the last release from [pipelinedb.com](https://www.pipelinedb.com) ([GitHub](https://github.com/pipelinedb/pipelinedb)).

[unreleased]: https://gitlab.tantorlabs.ru/database/pipelinedb/-/compare/1.3.4-REL_15...REL_15_STABLE
[1.3.4]: https://gitlab.tantorlabs.ru/database/pipelinedb/-/compare/1.3.3-REL_15...1.3.4-REL_15
[1.3.3]: https://gitlab.tantorlabs.ru/database/pipelinedb/-/compare/1.3.2-REL_15...1.3.3-REL_15
[1.3.2]: https://gitlab.tantorlabs.ru/database/pipelinedb/-/compare/1.3.1-REL_15...1.3.2-REL_15
[1.3.1]: https://gitlab.tantorlabs.ru/database/pipelinedb/-/compare/1.3.0-REL_15...1.3.1-REL_15
[1.3.0]: https://gitlab.tantorlabs.ru/database/pipelinedb/-/compare/1.0.0-13...1.3.0-REL_15
[1.0.0-13]: https://gitlab.tantorlabs.ru/database/pipelinedb/-/tags/1.0.0-13
