#!/usr/bin/env bash
#
# mayhem/build.sh - build VapourSynth's fuzz harnesses and the oracle probe. Runs in the commit image
# (mayhem/Dockerfile) as `mayhem` in /mayhem, offline: apt packages and the pinned glslang /
# Vulkan-Headers sources are baked into the image by the Dockerfile (/opt/toolchains).
set -euo pipefail

[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${MAYHEM_JOBS:=$(nproc)}"
: "${COVERAGE_FLAGS=}"
: "${SRC:=/mayhem}"
export SANITIZER_FLAGS DEBUG_FLAGS CC CXX LIB_FUZZING_ENGINE MAYHEM_JOBS COVERAGE_FLAGS

cd "$SRC"

# Meson subprojects come from the image cache; --wrap-mode=nodownload keeps meson off the network.
for dep in glslang vulkan-headers; do
    [ -f "/opt/toolchains/$dep/meson.build" ] || { echo "missing /opt/toolchains/$dep (baked by mayhem/Dockerfile)" >&2; exit 1; }
    ln -sfn "/opt/toolchains/$dep" "subprojects/$dep"
done

# Checks relaxed for the fuzz build, all benign-pattern floods rather than memory errors:
#   vptr               libvapoursynth and libvapoursynthfilters are separate .so files with hidden
#                      visibility, so cross-library RTTI checks fail on every plugin call
#   alignment          jitasm binds references to unaligned instruction-buffer addresses
#   enum               plugins cast user-supplied ints to enums (e.g. VSColorFamily) before validating them
#   nonnull-attribute  jitasm memset(NULL, ..., 0) on empty buffers
RELAX="-fno-sanitize=vptr,alignment,enum,nonnull-attribute"
# -fsanitize=fuzzer-no-link puts SanCov in the library so Mayhem sees its edges, not just the harness's.
INSTR="$SANITIZER_FLAGS -fsanitize=fuzzer-no-link $RELAX $DEBUG_FLAGS"
MESON_OPTS=(-Db_lto=false -Db_ndebug=true -Db_lundef=false --wrap-mode=nodownload --buildtype=debugoptimized)
TARGETS=(libvapoursynth.so.4 libvapoursynthfilters.so)

# 1) Sanitized + instrumented libraries.
rm -rf mayhem/build
CFLAGS="$INSTR" CXXFLAGS="$INSTR" LDFLAGS="$SANITIZER_FLAGS $RELAX" \
    meson setup mayhem/build "${MESON_OPTS[@]}"
ninja -C mayhem/build -j"$MAYHEM_JOBS" "${TARGETS[@]}"

# 2) Harnesses: fuzzer binary + run-once standalone reproducer, both linked with clang++.
HARNESS_FLAGS="$SANITIZER_FLAGS $RELAX $DEBUG_FLAGS"
LINK_LIB=(-Lmayhem/build -l:libvapoursynth.so.4 '-Wl,-rpath,$ORIGIN/mayhem/build')
$CXX $HARNESS_FLAGS -c mayhem/lsan_off.cc -o mayhem/build/lsan_off.o
$CC $HARNESS_FLAGS -c "$STANDALONE_FUZZ_MAIN" -o mayhem/build/standalone_main.o
for t in fuzz_expr fuzz_filters; do
    $CXX $HARNESS_FLAGS $LIB_FUZZING_ENGINE -std=c++17 -I include "mayhem/$t/$t.cc" mayhem/build/lsan_off.o \
        "${LINK_LIB[@]}" -o "$SRC/$t"
    $CXX $HARNESS_FLAGS -std=c++17 -I include "mayhem/$t/$t.cc" mayhem/build/lsan_off.o mayhem/build/standalone_main.o \
        "${LINK_LIB[@]}" -o "$SRC/$t-standalone"
done

# 3) Oracle: the same libraries with upstream's normal flags, plus the known-answer probe.
rm -rf mayhem/build-tests
CFLAGS="$COVERAGE_FLAGS" CXXFLAGS="$COVERAGE_FLAGS" LDFLAGS="$COVERAGE_FLAGS" \
    env -u SANITIZER_FLAGS meson setup mayhem/build-tests -Db_lto=false -Db_ndebug=true -Db_lundef=false --wrap-mode=nodownload
ninja -C mayhem/build-tests -j"$MAYHEM_JOBS" "${TARGETS[@]}"
$CC -O1 $COVERAGE_FLAGS -I include mayhem/kat/kat_probe.c -o "$SRC/kat_probe" \
    -Lmayhem/build-tests -l:libvapoursynth.so.4 '-Wl,-rpath,$ORIGIN/mayhem/build-tests'
file "$SRC/kat_probe" | grep -q 'dynamically linked'
