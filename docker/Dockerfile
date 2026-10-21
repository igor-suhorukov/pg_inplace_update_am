# syntax=docker/dockerfile:1.7
#
# pg_inplace_update_am development image.
#
# Everything is built and tested inside containers; nothing is compiled or
# installed on the host.  This image builds PostgreSQL 19 from source at a
# pinned REL_19_STABLE commit and carries the toolchain needed to build the
# extension through PGXS and run its regression, isolation and TAP tests.
#
#   PG_COMMIT   REL_19_STABLE commit to build (default: the pinned one)
#   PG_BUILD    debug   --enable-cassert --enable-debug --enable-injection-points,
#                       -O1; used by every test run
#               release -O2, no assertions; used by benchmarks only
#   DEV_UID     uid of the in-container user "pg"; the host user's uid, so
#               files written into bind-mounted output directories are owned
#               by the host user.  The server refuses to run as root.

ARG DEBIAN_IMAGE=debian:trixie-slim

FROM ${DEBIAN_IMAGE} AS pgbuild
ARG PG_REPO=https://github.com/postgres/postgres.git
ARG PG_COMMIT=d68601258e652a3cb034f3f218a63779456ec5f7
ARG PG_BUILD=debug

RUN set -eu; \
    export DEBIAN_FRONTEND=noninteractive; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
        build-essential bison flex git ca-certificates pkg-config perl \
        libipc-run-perl libreadline-dev zlib1g-dev libicu-dev liblz4-dev \
        libzstd-dev python3; \
    rm -rf /var/lib/apt/lists/*

WORKDIR /usr/src
RUN set -eu; \
    git init -q postgres; \
    cd postgres; \
    git remote add origin "$PG_REPO"; \
    git fetch -q --depth 1 origin "$PG_COMMIT"; \
    git checkout -q FETCH_HEAD; \
    git log -1 --format='%H %cd %s' > /usr/src/postgres/.pg_commit

RUN set -eu; \
    cd postgres; \
    common="--prefix=/opt/pg19 --enable-tap-tests --with-icu --with-lz4 --with-zstd"; \
    case "$PG_BUILD" in \
      debug)   ./configure $common --enable-debug --enable-cassert --enable-injection-points \
                   CFLAGS="-O1 -g -fno-omit-frame-pointer" ;; \
      release) ./configure $common --enable-debug CFLAGS="-O2 -g -fno-omit-frame-pointer" ;; \
      *) echo "PG_BUILD must be debug or release" >&2; exit 1 ;; \
    esac; \
    make -j"$(nproc)" -s world-bin; \
    make -s install-world-bin; \
    make -s -C src/test/isolation install; \
    make -s -C src/test/perl install; \
    if [ "$PG_BUILD" = debug ]; then make -s -C src/test/modules/injection_points install; fi; \
    make -s -C src/test/regress all

FROM ${DEBIAN_IMAGE} AS dev
ARG DEV_UID=1000
ARG PG_BUILD=debug

RUN set -eu; \
    export DEBIAN_FRONTEND=noninteractive; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
        build-essential perl libipc-run-perl libreadline-dev zlib1g-dev \
        libicu-dev liblz4-dev libzstd-dev locales procps gdb less git \
        ca-certificates python3 python3-venv; \
    sed -i 's/^# *\(en_US.UTF-8\)/\1/' /etc/locale.gen; \
    locale-gen; \
    rm -rf /var/lib/apt/lists/*

# DuckDB, pinned, for the analytical benchmarks (bench/olap): its tpch and
# tpcds extensions generate TPC-H's and TPC-DS's data and queries, so that
# nothing of TPC's kits is copied into the tree.
ARG DUCKDB_VERSION=1.5.5
RUN set -eu; \
    python3 -m venv /opt/duckdb; \
    /opt/duckdb/bin/pip install --no-cache-dir "duckdb==${DUCKDB_VERSION}"; \
    /opt/duckdb/bin/python -c "import duckdb; c = duckdb.connect(config={'extension_directory': '/opt/duckdb/extensions'}); c.install_extension('tpch'); c.install_extension('tpcds')"; \
    chmod -R a+rX /opt/duckdb

COPY --from=pgbuild /opt/pg19 /opt/pg19
# The built source tree: core regression/isolation suites (run against
# iheap as the default table AM) and src/test/regress/regress.so.
COPY --from=pgbuild /usr/src/postgres /usr/src/postgres

RUN set -eu; \
    if [ "$DEV_UID" = 0 ]; then echo "DEV_UID must not be 0" >&2; exit 1; fi; \
    useradd -m -u "$DEV_UID" -s /bin/bash pg; \
    chown -R pg /opt/pg19 /usr/src/postgres; \
    echo "$PG_BUILD" > /opt/pg19/.pg_build

# /build: the in-container copy of the (read-only) source tree
RUN install -d -o pg /build /out

ENV PATH=/opt/pg19/bin:$PATH \
    PG_CONFIG=/opt/pg19/bin/pg_config \
    LANG=en_US.UTF-8 \
    LC_ALL=en_US.UTF-8
USER pg
WORKDIR /home/pg
CMD ["bash"]
