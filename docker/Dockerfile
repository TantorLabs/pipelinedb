# Development environment for building PipelineDB and running its regression tests.
# See docker/README.md for usage and configuration.
#
# The source tree is bind-mounted at /src; build artifacts land in the host tree.

ARG BASE_IMAGE=ubuntu:26.04

# The PostgreSQL major comes from the "# pg_major = N" comment in
# pipelinedb.control unless the PG_MAJOR build argument overrides it. It is
# extracted in a separate stage so that unrelated control file edits do not
# invalidate the package installation layer below.
FROM ${BASE_IMAGE} AS pg-major
ARG PG_MAJOR=
COPY pipelinedb.control /tmp/pipelinedb.control
RUN major="${PG_MAJOR:-$(sed -n -e 's/^#[[:space:]]*pg_major[[:space:]]*=[[:space:]]*\([0-9][0-9]*\).*/\1/p' /tmp/pipelinedb.control)}" \
 && case "$major" in \
      ''|*[!0-9]*) echo "Could not determine PostgreSQL major (got '$major')" >&2; exit 1 ;; \
    esac \
 && echo "$major" > /pg_major

FROM ${BASE_IMAGE}

ARG USERNAME=dev
ARG USER_UID=1000
ARG USER_GID=1000

# PG_CFLAGS: GCC fuses multiply-adds by default on aarch64, which changes the last
# digits of some float results (cont_os_agg, cont_dist_agg, cont_sw_os_agg) versus
# the expected outputs. The Makefile appends -ggdb to this. Build with
# `PG_CFLAGS= make USE_PGXS=1` to get the compiler default instead.
#
# /usr/lib/postgresql/current is a symlink to the installed major's directory,
# since ENV cannot be computed from a file.
ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    PG_CFLAGS=-ffp-contract=off \
    PATH=/usr/lib/postgresql/current/bin:${PATH}

COPY --from=pg-major /pg_major /etc/pg_major

RUN PG_MAJOR="$(cat /etc/pg_major)" \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential \
      ca-certificates \
      curl \
      gdb \
      git \
      gnupg \
      less \
      libzmq3-dev \
      lsb-release \
      postgresql-common \
      python3-pip \
      sudo \
      vim-tiny \
 && mkdir -p /etc/postgresql-common/createcluster.d \
 && echo 'create_main_cluster = false' > /etc/postgresql-common/createcluster.d/no-main-cluster.conf \
 && /usr/share/postgresql-common/pgdg/apt.postgresql.org.sh -y -v "${PG_MAJOR}" \
 && apt-get install -y --no-install-recommends \
      libpq-dev \
      postgresql-${PG_MAJOR} \
      postgresql-${PG_MAJOR}-dbgsym \
      postgresql-server-dev-${PG_MAJOR} \
 && rm -rf /var/lib/apt/lists/* \
 && ln -s ${PG_MAJOR} /usr/lib/postgresql/current

COPY src/test/py/requirements.txt /tmp/requirements.txt
RUN pip3 install --no-cache-dir --break-system-packages -r /tmp/requirements.txt \
 && rm /tmp/requirements.txt

# initdb refuses to run as root, so tests run as an unprivileged user that owns
# the PostgreSQL install dirs (lets `make install` work without sudo). Membership
# in the postgres group grants write access to /var/run/postgresql, the default
# pipelinedb.ipc_socket_directory.
RUN PG_MAJOR="$(cat /etc/pg_major)" \
 && (userdel -r ubuntu 2>/dev/null || true) \
 && groupadd -g ${USER_GID} ${USERNAME} \
 && useradd -m -s /bin/bash -u ${USER_UID} -g ${USER_GID} -G postgres ${USERNAME} \
 && echo "${USERNAME} ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/${USERNAME} \
 && chmod 0440 /etc/sudoers.d/${USERNAME} \
 && chown -R ${USERNAME}:${USERNAME} \
      /usr/lib/postgresql/${PG_MAJOR}/lib \
      /usr/share/postgresql/${PG_MAJOR} \
      /usr/include/postgresql \
 && git config --system --add safe.directory '*'

USER ${USERNAME}
WORKDIR /src

CMD ["bash"]
