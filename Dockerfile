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

# Tests, benchmarks and fuzzers are off, so nothing is fetched from the network
# and the image build is reproducible from the source tree alone.
RUN cmake -S . -B /build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DBATON_BUILD_TESTS=OFF \
      -DBATON_STATIC_RUNTIME=ON \
 && cmake --build /build --target baton

# ---- runtime stage ---------------------------------------------------------
FROM ubuntu:24.04

RUN useradd --system --uid 10001 --home-dir /var/lib/baton --create-home baton
COPY --from=build /build/src/server/baton /usr/local/bin/baton

USER baton
WORKDIR /var/lib/baton
VOLUME ["/var/lib/baton"]

# 7379: RESP protocol. 7380: metrics and dashboard (HTTP, read-only).
EXPOSE 7379 7380

ENTRYPOINT ["baton"]
