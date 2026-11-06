#!/bin/bash
# 595_laptimer: one entry point for the work done on the Mac.
#   ./tools/go.sh test   build and run every host test (PASS/FAIL per test)
#   ./tools/go.sh laps   lap timing on testdata/*.rcz compared with RaceChrono's lap times
#   ./tools/go.sh vbo    write VBO files for the RaceChrono import check into out/
# The output is also saved to out/<command>.log. Works with the bash 3.2 that ships with macOS.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="build/host"
OUT="out"

CXXFLAGS="-std=c++17 -O2 -Wall -Wextra -Wshadow -I."
# core/ also runs on the ESP32-S3, whose FPU is single precision: flag hidden double math
COREFLAGS="-Wdouble-promotion"

usage() {
  echo "usage: ./tools/go.sh test | laps | vbo"
  echo "  test  build and run every host test"
  echo "  laps  lap timing on testdata/*.rcz compared with RaceChrono"
  echo "  vbo   write VBO files for the RaceChrono import check into out/"
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

# Unzips testdata/*.rcz into build/host/rcz/<name>/ and prints one directory per line
extract_rcz() {
  for f in testdata/*.rcz; do
    [ -e "$f" ] || continue
    dir="$BUILD/rcz/$(basename "$f" .rcz)"
    mkdir -p "$dir"
    if ! unzip -o -q "$f" -d "$dir"; then
      echo "FAIL: cannot unzip $f"
      return 1
    fi
    echo "$dir"
  done
}

run_test() {
  header
  build "$BUILD/host_tests" test/*.cpp
  "$BUILD/host_tests"
}

run_laps() {
  header
  build "$BUILD/laps" tools/host/laps.cpp tools/host/rcz.cpp
  dirs="$(extract_rcz)" || {
    echo "$dirs"
    echo "RESULT: FAIL"
    exit 1
  }
  if [ -z "$dirs" ]; then
    echo "RESULT: FAIL (no testdata/*.rcz)"
    exit 1
  fi
  old_ifs="$IFS"
  IFS='
'
  set -- $dirs
  IFS="$old_ifs"
  "$BUILD/laps" "$@"
}

run_vbo() {
  header
  build "$BUILD/vbogen" tools/host/vbogen.cpp tools/host/rcz.cpp test/synth.cpp
  failed=0
  files=0
  "$BUILD/vbogen" "$OUT/test_sodegaura_synth.vbo" || failed=$((failed + 1))
  files=$((files + 1))
  dirs="$(extract_rcz)" || {
    echo "$dirs"
    failed=$((failed + 1))
    dirs=""
  }
  old_ifs="$IFS"
  IFS='
'
  for d in $dirs; do
    IFS="$old_ifs"
    "$BUILD/vbogen" --rcz "$d" "$OUT/$(basename "$d").vbo" || failed=$((failed + 1))
    files=$((files + 1))
  done
  IFS="$old_ifs"
  if [ "$failed" -gt 0 ]; then
    echo "RESULT: FAIL ($failed of $files files)"
    exit 1
  fi
  echo "RESULT: PASS ($files files in $OUT/)"
}

CMD="${1:-}"
case "$CMD" in
  test | laps | vbo) ;;
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
