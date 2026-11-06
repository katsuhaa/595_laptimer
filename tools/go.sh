#!/bin/bash
# 595_laptimer: one entry point for the work done on the Mac.
#   ./tools/go.sh test   build and run every host test (PASS/FAIL per test)
#   ./tools/go.sh vbo    write out/test_sodegaura_synth.vbo for the RaceChrono import check
# The output is also saved to out/<command>.log. Works with the bash 3.2 that ships with macOS.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="build/host"
OUT="out"

CXXFLAGS="-std=c++17 -O2 -Wall -Wextra -Wshadow -I."
# core/ also runs on the ESP32-S3, whose FPU is single precision: flag hidden double math
COREFLAGS="-Wdouble-promotion"

usage() {
  echo "usage: ./tools/go.sh test | vbo"
  echo "  test  build and run every host test"
  echo "  vbo   write out/test_sodegaura_synth.vbo (RaceChrono import check)"
}

find_cxx() {
  for c in c++ clang++ g++; do
    if command -v "$c" >/dev/null 2>&1; then
      echo "$c"
      return 0
    fi
  done
  return 1
}

header() {
  echo "595_laptimer go.sh $CMD  $(date '+%Y-%m-%d %H:%M:%S')"
  echo "compiler: $CXX ($("$CXX" --version 2>/dev/null | head -n 1))"
}

build_failed() {
  cat "$1"
  if grep -q "xcrun: error" "$1"; then
    echo "hint: the Xcode command line tools are missing. Run: xcode-select --install"
  fi
  echo "RESULT: FAIL (build)"
  exit 1
}

# build <binary> <sources...>  (core/*.cpp are always added)
build() {
  bin="$1"
  shift
  log="$BUILD/$(basename "$bin").log"
  : >"$log"
  objs=""
  for src in core/*.cpp; do
    obj="$BUILD/core_$(basename "$src" .cpp).o"
    "$CXX" $CXXFLAGS $COREFLAGS -c "$src" -o "$obj" >>"$log" 2>&1 || build_failed "$log"
    objs="$objs $obj"
  done
  "$CXX" $CXXFLAGS -o "$bin" "$@" $objs >>"$log" 2>&1 || build_failed "$log"
  if [ -s "$log" ]; then
    echo "build warnings ($log):"
    cat "$log"
  fi
  echo "build: OK"
}

run_test() {
  header
  build "$BUILD/host_tests" test/*.cpp
  "$BUILD/host_tests"
}

run_vbo() {
  header
  build "$BUILD/vbogen" tools/host/vbogen.cpp test/synth.cpp
  "$BUILD/vbogen" "$OUT/test_sodegaura_synth.vbo"
}

CMD="${1:-}"
case "$CMD" in
  test | vbo) ;;
  *)
    usage
    exit 2
    ;;
esac

cd "$ROOT" || exit 1
mkdir -p "$BUILD" "$OUT"
CXX="$(find_cxx)" || {
  echo "FAIL: no C++ compiler found. Run: xcode-select --install"
  exit 1
}

run_"$CMD" 2>&1 | tee "$OUT/$CMD.log"
status="${PIPESTATUS[0]}"
echo "log: $OUT/$CMD.log"
exit "$status"
