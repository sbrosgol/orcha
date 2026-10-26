# syntax=docker/dockerfile:1.7

# =============================================================================
# Build stage - compile orcha + plugin libraries with vcpkg-managed deps.
# =============================================================================
FROM ubuntu:24.04 AS build

# Must match `builtin-baseline` in src/orcha/vcpkg.json — vcpkg looks up the
# baseline by SHA in its local git history, so this commit must be fetched.
ARG VCPKG_BASELINE=2e58bb35ff7a3a037920d959ce20cb4d8c22319a
ENV DEBIAN_FRONTEND=noninteractive \
    VCPKG_ROOT=/opt/vcpkg \
    VCPKG_DEFAULT_BINARY_CACHE=/vcpkg-cache \
    VCPKG_FORCE_SYSTEM_BINARIES=1

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        g++-13 \
        gcc-13 \
        cmake \
        ninja-build \
        git \
        curl \
        zip \
        unzip \
        tar \
        ca-certificates \
        pkg-config \
        autoconf \
        automake \
        autoconf-archive \
        libtool \
        python3 \
        bison \
        flex \
        openssl \
        libssl-dev \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-13 100 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-13 100 \
    && rm -rf /var/lib/apt/lists/*

# Install any host-side corporate root CAs (e.g. Cisco Secure Access, Zscaler)
# so vcpkg's downloads from intercepted HTTPS hosts validate. Drop *.crt files
# into ./certs/ on the host. Missing directory is harmless.
RUN mkdir -p /usr/local/share/ca-certificates/extra
COPY cert[s]/ /usr/local/share/ca-certificates/extra/
RUN set -e; \
    count=$(ls /usr/local/share/ca-certificates/extra/*.crt 2>/dev/null | wc -l); \
    echo "[orcha] installing $count corporate CA cert(s)"; \
    if [ "$count" -gt 0 ]; then \
        update-ca-certificates --fresh; \
        echo "[orcha] verifying intercepted HTTPS host:"; \
        curl -fsS -o /dev/null -w "  raw.githubusercontent.com http=%{http_code}\n" \
            https://raw.githubusercontent.com/boostorg/boost/refs/tags/boost-1.88.0/LICENSE_1_0.txt; \
    fi

# Full clone (~700MB): vcpkg's manifest mode does `git read-tree <port-sha>` for
# each pinned port version, which fails on a shallow clone — the upstream error
# is literally "Try again with a full vcpkg clone."
#
# Bootstrap notes: the prebuilt-binary download targets release-assets.github-
# usercontent.com, whose cert chain doesn't validate cleanly in Ubuntu 24.04
# containers (and VCPKG_FORCE_SYSTEM_BINARIES is documented but not honored).
# Force the from-source path by overriding the download decision; the source
# tarball is fetched from github.com/microsoft/vcpkg-tool/archive/... which works.
RUN git clone https://github.com/microsoft/vcpkg "${VCPKG_ROOT}" \
    && cd "${VCPKG_ROOT}" \
    && git checkout "${VCPKG_BASELINE}" \
    && sed -i '/^# Do the download or build\./i vcpkgDownloadTool="OFF"; vcpkgToolReleaseSha="$VCPKG_TOOL_SOURCE_SHA"' scripts/bootstrap.sh \
    && ./bootstrap-vcpkg.sh -disableMetrics \
    && mkdir -p "${VCPKG_DEFAULT_BINARY_CACHE}"

WORKDIR /src
COPY src/orcha /src

# Configure + build. The vcpkg toolchain is picked up via VCPKG_ROOT
# (see top of CMakeLists.txt). Build all targets (not just orcha) so the
# plugin shared libraries — independent CMake targets loaded at runtime — are
# produced too.
RUN --mount=type=cache,target=/vcpkg-cache \
    cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" \
    && cmake --build build --parallel

# Stage the runtime layout so the runtime image just copies one directory.
RUN mkdir -p /out/commands \
    && cp build/orcha /out/orcha \
    && cp -r build/commands/. /out/commands/ \
    && (cp orcha.yaml.example /out/orcha.yaml.example || true) \
    && (cp -r sample_workflows /out/sample_workflows || true)

# =============================================================================
# Runtime stage - minimal image.
# =============================================================================
FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        libstdc++6 \
        libssl3 \
        libpq5 \
        ca-certificates \
        curl \
        tini \
    && rm -rf /var/lib/apt/lists/* \
    && (userdel -r ubuntu 2>/dev/null || true) \
    && groupadd --system --gid 1000 orcha \
    && useradd  --system --uid 1000 --gid orcha --home /app --shell /usr/sbin/nologin orcha \
    && mkdir -p /app/logs /app/data /app/config \
    && chown -R orcha:orcha /app

WORKDIR /app
COPY --from=build --chown=orcha:orcha /out/ /app/

USER orcha

# Inside a container the server must listen on all interfaces; the published
# port is the boundary. POST /workflow still requires ORCHA_SERVER_API_TOKEN.
ENV ORCHA_SERVER_HOST=0.0.0.0

# HTTP API (matches server.port in orcha.yaml).
EXPOSE 8070

# tini reaps zombies and forwards signals so Ctrl-C / docker stop works cleanly.
ENTRYPOINT ["/usr/bin/tini", "--"]
CMD ["/app/orcha"]
