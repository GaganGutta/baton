# syntax=docker/dockerfile:1

# ---- build stage -----------------------------------------------------------
FROM ubuntu:24.04 AS build

RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ cmake ninja-build \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt CMakePresets.json ./
COPY cmake ./cmake
COPY src ./src
COPY tools ./tools

# Tests, benchmarks and fuzzers are off, so nothing is fetched from the network
# and the image build is reproducible from the source tree alone.
RUN cmake -S . -B /build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DBATON_BUILD_TESTS=OFF \
      -DBATON_STATIC_RUNTIME=ON \
 && cmake --build /build --target baton baton-logcheck

# ---- runtime stage ---------------------------------------------------------
FROM ubuntu:24.04

RUN useradd --system --uid 10001 --home-dir /var/lib/baton --create-home baton
COPY --from=build /build/src/server/baton /build/tools/baton-logcheck /usr/local/bin/

USER baton
WORKDIR /var/lib/baton
VOLUME ["/var/lib/baton"]

# 7379: RESP protocol. 7380: metrics and dashboard (HTTP, read-only).
EXPOSE 7379 7380

# Inside a container the network namespace is the boundary, so listen on all of
# its interfaces; publish the port only where it should be reachable
# (-p 127.0.0.1:7379:7379). Arguments given to `docker run` are appended, and a
# later flag overrides an earlier one.
ENTRYPOINT ["baton", "--dir", "/var/lib/baton/data", "--bind", "0.0.0.0"]
