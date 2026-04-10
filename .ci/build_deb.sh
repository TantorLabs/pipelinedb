#!/usr/bin/env bash

set -xeuo pipefail

detect_postgres_version() {
    local branch

    if [ -n "${GITLAB_CI:-}" ]; then
        echo "Detected GitLab CI"
        if [ -n "${CI_MERGE_REQUEST_TARGET_BRANCH_NAME:-}" ]; then
            branch="$CI_MERGE_REQUEST_TARGET_BRANCH_NAME"
        else
            branch="$CI_COMMIT_REF_NAME"
        fi
    elif [ -n "${GITHUB_ACTIONS:-}" ]; then
        echo "Detected GitHub Actions"
        if [ -n "${GITHUB_BASE_RE:-}F" ]; then
            branch="$GITHUB_BASE_REF"
        else
            branch="$GITHUB_REF_NAME"
        fi
    else
        echo "Warning: Not running in known CI environment"
        exit 1
    fi

    POSTGRES_VERSION=$( [[ $branch =~ REL_([0-9]+)_STABLE ]] && echo "${BASH_REMATCH[1]}" )
}

main() {
    detect_postgres_version

    apt-get update
    apt-get install -y \
        build-essential \
        curl \
        ca-certificates \
        git \
        gnupg \
        libzmq3-dev \
        lsb-release \
        postgresql-common \
        python3-pip \
        sudo

    /usr/share/postgresql-common/pgdg/apt.postgresql.org.sh -y

    apt-get install -y libpq-dev \
        postgresql-$POSTGRES_VERSION \
        postgresql-server-dev-$POSTGRES_VERSION
    pip3 install --break-system-packages -r src/test/py/requirements.txt

    make USE_PGXS=1 -j$(nproc)
    make top_srcdir="/usr/lib/postgresql/$POSTGRES_VERSION/lib/pgxs" srcdir="$PWD" install

    chown -R postgres:postgres . /usr/lib/postgresql /usr/include/postgresql /usr/share/postgresql

    sudo -u postgres PATH=/usr/lib/postgresql/$POSTGRES_VERSION/bin:"$PATH" make USE_PGXS=1 test || { cat src/test/regress/log/initdb.log; cat src/test/regress/regression.diffs; exit 1; }
}

main "$@"
