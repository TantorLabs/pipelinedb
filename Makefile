MODULE_big = pipelinedb
EXTVERSION = $(shell grep default_version $(MODULE_big).control | \
               sed -e "s/default_version[[:space:]]*=[[:space:]]*'\([^']*\)'/\1/")
EXTREVISION = $(shell git rev-parse --short HEAD 2>/dev/null || \
               grep commit_sha $(MODULE_big).control | \
               sed -e "s/commit_sha[[:space:]]*=[[:space:]]*'\([^']*\)'/\1/")

PG_CPPFLAGS += -DPIPELINE_VERSION_STR=\"$(EXTVERSION)\"
PG_CPPFLAGS += -DPIPELINE_REVISION_STR=\"$(EXTREVISION)\"

SOURCES = $(shell find src -type f -name '*.c' -not -path 'src/test/*')
OBJS = $(patsubst %.c,%.o,$(SOURCES))

PG_CONFIG := pg_config

EXTENSION = pipelinedb
REGRESS = $(EXTENSION)
# pg_regress is invoked as: ... $(REGRESS_OPTS) $(REGRESS), so this adds an extra
# test sql/pipelinedb.sql after serial_schedule. The file must exist and should
# stay empty: pg_regress runs psql with query echo, so any SQL (even comments) would
# appear in results/pipelinedb.out and fail the diff against expected/pipelinedb.out.
# TODO: Parallel tests fail randomly. Needs to be investigated. For now, we
# use serial_schedule instead of parallel_schedule
REGRESS_OPTS = --schedule=./src/test/regress/serial_schedule \
  --host=localhost \
  --inputdir=./src/test/regress \
  --outputdir=./src/test/regress \
  --load-extension=pipelinedb \
  --temp-config=./src/test/regress/pipelinedb.conf \
  --bindir=$(bindir) \
  --temp-instance=./src/test/tmp/pgsql

# pg_regress/initdb default to SQL_ASCII when the environment has no UTF-8 locale
# (common in minimal Docker/CI). Pipelinedb and its regression SQL use COLLATE "C.utf8",
# which requires a UTF-8 database encoding (see PostgreSQL PGXS: ENCODING / --encoding).
ENCODING = UTF8

DATA = $(shell find . -type f -name 'pipelinedb--*.sql')
SHLIB_LINK += -lzmq -lstdc++

ifdef USE_PGXS
PG_CPPFLAGS += -I./include -I$(shell $(PG_CONFIG) --includedir)
PG_CFLAGS += -ggdb

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else

bindir = $(shell $(PG_CONFIG) --bindir)
REGRESS_OPTS = --schedule=./src/test/regress/parallel_schedule \
  --host=localhost \
  --inputdir=./src/test/regress \
  --outputdir=./src/test/regress \
  --load-extension=pipelinedb \
  --temp-config=./src/test/regress/pipelinedb.conf \
  --bindir=$(bindir)

NO_GENERATED_HEADERS = 1
NO_PGXS = 1
NO_TEMP_INSTALL = 1
top_builddir = $(shell $(PG_CONFIG) --pkglibdir)/pgxs

include $(shell $(PG_CONFIG) --pkglibdir)/pgxs/src/Makefile.global
include $(shell $(PG_CONFIG) --pgxs)

endif

bin_dir = ./bin

headers_dir = $(shell $(PG_CONFIG) --includedir-server)/../pipelinedb

# Headers for other extensions to build against
install-headers:
	$(MKDIR_P) $(headers_dir)
	$(INSTALL_DATA) $(CURDIR)/include/*.h '$(headers_dir)'

install: install-headers

bootstrap:
	$(bin_dir)/bootstrap

run:
	$(bin_dir)/run-dev

test:
	$(MKDIR_P) $(CURDIR)/src/test/tmp
ifdef USE_PGXS
	make install
	make installcheck
else
	make check
endif
	make -C $(CURDIR)/src/test/py test
