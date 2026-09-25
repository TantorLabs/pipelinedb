#!/usr/bin/env bash

set -xeuo pipefail

# Install prefix when building PostgreSQL from source (distro/arch/PG major not on PGDG Apt).
PG_SRC_ROOT="${PG_SRC_ROOT:-/tmp/postgresql-src}"
PG_PREFIX="${PG_PREFIX:-/usr/local/pgsql}"

# PGDG Apt support matrix (update when https://wiki.postgresql.org/wiki/Apt changes):
#   Debian: bullseye (11), bookworm (12), trixie (13), forky (testing), sid
#   Ubuntu: jammy, noble, questing (25.10, amd64 only), resolute (26.04)
#   Architectures: amd64, arm64, ppc64el
#   PostgreSQL: 13–19 (19 includes devel)

detect_postgres_version() {
	local branch

	if [ -n "${POSTGRES_VERSION:-}" ]; then
		return
	fi

	if [ -n "${GITLAB_CI:-}" ]; then
		echo "Detected GitLab CI"
		if [ -n "${CI_MERGE_REQUEST_TARGET_BRANCH_NAME:-}" ]; then
			branch="$CI_MERGE_REQUEST_TARGET_BRANCH_NAME"
		else
			branch="$CI_COMMIT_REF_NAME"
		fi
	elif [ -n "${GITHUB_ACTIONS:-}" ]; then
		echo "Detected GitHub Actions"
		if [ -n "${GITHUB_BASE_REF:-}" ]; then
			branch="$GITHUB_BASE_REF"
		else
			branch="$GITHUB_REF_NAME"
		fi
	else
		echo "Warning: Not running in known CI environment"
		exit 1
	fi

	# Accepts REL_N_STABLE as well as feature branches such as GL-60-REL_15.
	POSTGRES_VERSION=$( [[ $branch =~ (^|[^A-Za-z0-9])REL_([0-9]+)($|[^0-9]) ]] && echo "${BASH_REMATCH[2]}" )
	if [ -z "${POSTGRES_VERSION:-}" ]; then
		echo "Could not derive PostgreSQL major from branch: $branch"
		exit 1
	fi
}

pgdg_supported_postgresql_major() {
	local v="$1"
	[[ "$v" =~ ^[0-9]+$ ]] || return 1
	((v >= 13 && v <= 19))
}

# Returns 0 when this host should use PGDG Apt (postgresql.org script + packages).
pgdg_apt_environment_supported() {
	local id codename arch

	if [ "${BUILD_POSTGRES_FROM_SOURCE:-}" = "true" ]; then
		return 1
	fi

	if ! pgdg_supported_postgresql_major "${POSTGRES_VERSION:-}"; then
		return 1
	fi

	[ -f /etc/os-release ] || return 1
	# shellcheck source=/dev/null
	. /etc/os-release
	id="${ID:-}"
	codename="${VERSION_CODENAME:-}"
	[ -n "$codename" ] || return 1

	if ! command -v dpkg >/dev/null 2>&1; then
		return 1
	fi
	arch=$(dpkg --print-architecture)
	case "$arch" in
		amd64 | arm64 | ppc64el) ;;
		*) return 1 ;;
	esac

	case "$id" in
		ubuntu)
			case "$codename" in
				jammy | noble | resolute) return 0 ;;
				questing)
					[ "$arch" = "amd64" ] && return 0
					return 1
					;;
				*) return 1 ;;
			esac
			;;
		debian)
			case "$codename" in
				bullseye | bookworm | trixie | forky | sid) return 0 ;;
				*) return 1 ;;
			esac
			;;
		*)
			return 1 ;;
	esac
}

needs_postgresql_from_source() {
	if [ "${BUILD_POSTGRES_FROM_SOURCE:-}" = "true" ]; then
		return 0
	fi
	if pgdg_apt_environment_supported; then
		return 1
	fi
	return 0
}

install_base_packages() {
	apt-get update
	apt-get install -y \
		build-essential \
		curl \
		ca-certificates \
		git \
		gnupg \
		libzmq3-dev \
		python3-pip \
		sudo
}

install_postgresql_pgdg() {
	apt-get install -y lsb-release postgresql-common
	/usr/share/postgresql-common/pgdg/apt.postgresql.org.sh -y

	apt-get install -y libpq-dev \
		postgresql-"$POSTGRES_VERSION" \
		postgresql-server-dev-"$POSTGRES_VERSION"
}

install_build_deps_source_postgresql() {
	# Dependencies for ./configure && make on PostgreSQL REL_*_STABLE (Debian-family).
	apt-get install -y \
		bison \
		flex \
		libreadline-dev \
		zlib1g-dev \
		libssl-dev \
		libxml2-dev \
		pkg-config
}

ensure_postgres_user() {
	if ! getent passwd postgres >/dev/null; then
		useradd -r -m -d /var/lib/postgresql -s /bin/bash postgres
	fi
}

build_and_install_postgresql_from_source() {
	local pg_branch="REL_${POSTGRES_VERSION}_STABLE"
	local repo="${POSTGRES_GIT_URL:-https://git.postgresql.org/git/postgresql.git}"

	install_build_deps_source_postgresql

	rm -rf "$PG_SRC_ROOT"
	git clone --depth 1 --single-branch --branch "$pg_branch" "$repo" "$PG_SRC_ROOT"

	export PG_PREFIX="${PG_PREFIX}-${POSTGRES_VERSION}"
	cd "$PG_SRC_ROOT"
	./configure --prefix="$PG_PREFIX" \
		--enable-debug \
		--with-openssl \
		--with-readline \
		--without-icu
	make -j"$(nproc)"
	make install
	cd -

	export PATH="${PG_PREFIX}/bin:${PATH}"
	command -v pg_config
	pg_config --version
}

setup_postgresql_debian_packages() {
	install_postgresql_pgdg
	export PATH="/usr/lib/postgresql/${POSTGRES_VERSION}/bin:${PATH}"
}

run_pipelinedb_build_and_test() {
	pip3 install --break-system-packages -r src/test/py/requirements.txt

	make USE_PGXS=1 -j"$(nproc)"
	make USE_PGXS=1 install

	ensure_postgres_user
	chown -R postgres:postgres .
	if needs_postgresql_from_source; then
		chown -R postgres:postgres "$PG_PREFIX"
	else
		chown -R postgres:postgres /usr/lib/postgresql /usr/include/postgresql /usr/share/postgresql
	fi

	sudo -u postgres PATH="$(pg_config --bindir):${PATH}" \
		make USE_PGXS=1 test || {
		cat src/test/regress/log/initdb.log
		cat src/test/regress/regression.diffs
		exit 1
	}
}

main() {
	detect_postgres_version
	echo "PostgreSQL major (from branch): $POSTGRES_VERSION"

	install_base_packages

	if needs_postgresql_from_source; then
		echo "Building PostgreSQL from source (OS/arch/codename or PostgreSQL major not supported by PGDG Apt; see header in build_deb.sh)."
		build_and_install_postgresql_from_source
	else
		echo "Installing PostgreSQL from PGDG Apt packages."
		setup_postgresql_debian_packages
	fi

	run_pipelinedb_build_and_test
}

main "$@"
