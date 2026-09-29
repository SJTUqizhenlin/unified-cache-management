#!/bin/bash
# build_so.sh — build libspdkstore.so with the real engine linked in.
#
# Extracts the exact link flags SPDK's mk system uses (obtained from
# `make -n` in the standalone test's Makefile) to resolve the full
# SPDK → DPDK → system library chain.

set -e

SPDK_ROOT_DIR=${SPDK_ROOT_DIR:-/home/qizhenlin/devSPDK/spdk}
UCM_ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
ENGINE_DIR="$UCM_ROOT/ucm/store/spdk/cc"
OUT_DIR="$UCM_ROOT/ucm/store/spdk"
BUILD_DIR="$UCM_ROOT/build/temp.linux-aarch64-cpython-311"

# 1. Rebuild the engine static library
make -C "$ENGINE_DIR" clean all SPDK_ROOT_DIR="$SPDK_ROOT_DIR"

# 2. Collect SPDK's link flags from the standalone test binary's build
# Use awk to extract everything between the .o input and the "&& rm" cleanup
LINK_FLAGS=$(cd "$UCM_ROOT/testSPDK/ucm_spdk_store" && make -n 2>/dev/null | \
    grep 'echo.*LINK' | awk -F'ucm_spdk_store\.o' '{print $2}' | awk -F'&& rm' '{print $1}')

# 3. UCM internal libraries
UCM_LIBS="$BUILD_DIR/ucm/shared/infra/logger/libinfra_logger.a \
          $BUILD_DIR/ucm/shared/metrics/libmetrics.so \
          $BUILD_DIR/_deps/spdlog-build/libspdlog.a \
          $BUILD_DIR/_deps/fmt-build/libfmt.a"

# 4. Include paths
INCLUDES="-I$ENGINE_DIR \
          -I$UCM_ROOT/ucm/store/detail/type \
          -I$UCM_ROOT/ucm/store/detail \
          -I$UCM_ROOT/ucm/store \
          -I$UCM_ROOT/ucm/shared/infra/logger \
          -I$UCM_ROOT/ucm/shared/infra/status \
          -I$UCM_ROOT/ucm/shared/infra \
          -I$UCM_ROOT/ucm/shared/metrics/cc/api \
          -I$UCM_ROOT/ucm/shared/metrics/cc/domain \
          -I$BUILD_DIR/_deps/fmt-src/include \
          -I$BUILD_DIR/_deps/spdlog-src/include \
          -I$SPDK_ROOT_DIR/include"

echo "Building libspdkstore.so (with real engine)..."
c++ -std=c++17 -shared -fPIC -O2 -g -Wall \
    $INCLUDES \
    -o "$OUT_DIR/libspdkstore.so" \
    "$ENGINE_DIR/spdk_store.cc" \
    "$ENGINE_DIR/libucm_spdk_engine.a" \
    $UCM_LIBS \
    $LINK_FLAGS \
    -lz -laio \
    -Wl,-rpath,'$ORIGIN/../../shared/metrics'

echo "Done: $OUT_DIR/libspdkstore.so"
ls -la "$OUT_DIR/libspdkstore.so"
