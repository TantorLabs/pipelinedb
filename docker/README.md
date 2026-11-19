# Docker development environment

An Ubuntu 26.04 image with PostgreSQL (from the [PGDG Apt repository](https://wiki.postgresql.org/wiki/Apt)),
ZeroMQ and the Python test dependencies, for building PipelineDB and running its
regression tests. The source tree is bind-mounted at `/src`, so build artifacts
end up in your checkout.

## Usage

```bash
docker compose -f docker/compose.yaml build
docker compose -f docker/compose.yaml run --rm dev

# inside the container
make USE_PGXS=1 -j$(nproc)
make USE_PGXS=1 install
make USE_PGXS=1 test           # install + SQL regression tests + Python tests

# SQL regression tests only
mkdir -p src/test/tmp && make USE_PGXS=1 installcheck
```

`make test` creates `src/test/tmp`; `installcheck` needs it to exist but does not
create it.

`export COMPOSE_FILE=docker/compose.yaml` lets you drop the `-f` option.

The container runs as the unprivileged user `dev` (`initdb` refuses to run as
root), which owns the PostgreSQL install directories so `make install` works
without `sudo`. Passwordless `sudo` is available for anything else.

## PostgreSQL major version

The PostgreSQL major is taken from the `# pg_major = N` comment in
`pipelinedb.control`, the same source used by the `Makefile` and
`.ci/build_deb.sh`. The Dockerfile reads it at image build time, so no
configuration is needed to build an image for the current branch.

`compose.yaml` cannot read the control file, so it uses the `PG_MAJOR`
environment variable for the image tag and the Compose project name. If unset,
both default to `local` (`pipelinedb-dev:local`).

Set `PG_MAJOR` when you work with several branches or worktrees, so each major
gets its own image instead of overwriting a shared `local` tag:

```bash
echo "PG_MAJOR=$(sed -n 's/^#[[:space:]]*pg_major[[:space:]]*=[[:space:]]*\([0-9]*\).*/\1/p' pipelinedb.control)" > docker/.env
```

`docker/.env` is read by Compose automatically and is ignored by git. A value
set in `PG_MAJOR` also overrides the control file when building the image.

If an image built for one major is used with a branch targeting another, the
build fails with `PostgreSQL major version does not match pg_major in
pipelinedb.control`; rebuild the image (or set `PG_MAJOR` as above).

## Environment variables

All variables are optional. They are read by `compose.yaml` from the shell
environment or from `docker/.env`. Changes to the build arguments take effect
after `docker compose -f docker/compose.yaml build`.

| Variable     | Default                           | Purpose |
|--------------|-----------------------------------|---------|
| `PG_MAJOR`   | `pg_major` in `pipelinedb.control` | PostgreSQL major to install; also used in the image tag and project name (`local` when unset) |
| `BASE_IMAGE` | `ubuntu:26.04`                    | Base image; must be a Debian/Ubuntu release supported by PGDG |
| `USER_UID`   | `1000`                            | UID of the `dev` user in the container |
| `USER_GID`   | `1000`                            | GID of the `dev` user in the container |

## Notes

- **Floating-point results on arm64.** GCC fuses multiply-adds by default on
  aarch64, which changes the last digits of a few results compared with the
  expected outputs (`cont_os_agg`, `cont_dist_agg`, `cont_sw_os_agg`). The image
  sets `PG_CFLAGS=-ffp-contract=off` to prevent that. Use
  `PG_CFLAGS= make USE_PGXS=1` to build with the compiler default.
- **`test_upgrade.py` on macOS.** `pg_upgrade` creates its Unix socket in the
  current directory, which is the bind-mounted checkout. Docker Desktop's file
  sharing does not support Unix sockets, so this test fails on macOS hosts with
  `could not create any Unix-domain sockets` in
  `src/test/py/test_binary_upgrade_data_dir0/pg_upgrade_output.d/*/log/pg_upgrade_server.log`.
  To work around it, point `pg_upgrade` at a container-local socket directory.
  It reads `PGSOCKETDIR`; nothing else in the test suite does, so it is safe to
  set for the whole run:

  ```bash
  PGSOCKETDIR=/tmp make USE_PGXS=1 test
  ```
