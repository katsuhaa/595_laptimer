#!/bin/bash
# 595_laptimer: one entry point for the work done on the Mac.
#   ./tools/go.sh test      build and run every host test (PASS/FAIL per test)
#   ./tools/go.sh laps      lap timing on testdata/*.rcz compared with RaceChrono's lap times
#   ./tools/go.sh vbo       write VBO files for the RaceChrono import check into out/
#   ./tools/go.sh render    draw every UI screen into out/render/*.png and check the UI (redraw, touch, glyphs, timings)
#   ./tools/go.sh assets    download the pinned fonts, regenerate ui/fonts_data.{h,cpp}, then run render
#   ./tools/go.sh report    the session report (out/report_<id>.html) from testdata/ as the device makes it
#   ./tools/go.sh web       the device's Web pages against a stand-in of the device (needs python3 playwright
#                           and Google Chrome); screenshots in out/web/
#   ./tools/go.sh webcheck  build firmware/webcheck, flash it to the ESP32 on USB and show its serial
#                           output until Ctrl-C (P0-W). A port can be given: webcheck /dev/cu.xxx
#   ./tools/go.sh build     build the laptimer firmware (ESP32-S3-Touch-AMOLED-2.41) without flashing
#   ./tools/go.sh flash     build, flash it to the 2.41 board on USB and show its serial output until Ctrl-C
# The output is also saved to out/<command>.log. Works with the bash 3.2 that ships with macOS.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="build/host"
OUT="out"

CXXFLAGS="-std=c++17 -O2 -Wall -Wextra -Wshadow -I."
# core/ also runs on the ESP32-S3, whose FPU is single precision: flag hidden double math
COREFLAGS="-Wdouble-promotion"

# firmware/webcheck: classic ESP32 (ESP32 Dev Module), 4 MB flash, default partitions, DIO flash mode
SKETCH="firmware/webcheck"
FQBN="esp32:esp32:esp32:PartitionScheme=default,FlashMode=dio"

# firmware/laptimer: ESP32-S3 (OPI PSRAM, 16 MB QIO flash), 3 MB app + FAT partition table, USB CDC serial
LT_SKETCH="firmware/laptimer"
LT_FQBN="esp32:esp32:esp32s3:PSRAM=opi,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc,USBMode=hwcdc,CPUFreq=240"

usage() {
  echo "usage: ./tools/go.sh test | laps | vbo | render | assets | report | web | webcheck [port] | build | flash [port]"
  echo "  test      build and run every host test"
  echo "  laps      lap timing on testdata/*.rcz compared with RaceChrono"
  echo "  vbo       write VBO files for the RaceChrono import check into out/"
  echo "  render    draw every UI screen into out/render/ and check the UI"
  echo "  assets    download the fonts and regenerate ui/fonts_data.{h,cpp} (then render)"
  echo "  report    session reports from testdata/ into out/ (as the device makes them)"
  echo "  web       check the device's Web pages against a stand-in of the device (screenshots in out/web/)"
  echo "  webcheck  flash the Web test to the ESP32 on USB and show its serial output (Ctrl-C to stop)"
  echo "  build     build the laptimer firmware (no board needed)"
  echo "  flash     build, flash the laptimer firmware to the 2.41 board and show its serial output (Ctrl-C to stop)"
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
  if [ -n "$CXX" ]; then
    echo "compiler: $CXX ($("$CXX" --version 2>/dev/null | head -n 1))"
  fi
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
  build "$BUILD/laps" tools/host/laps.cpp tools/host/rcz.cpp tools/host/vboread.cpp
  dirs="$(extract_rcz)" || {
    echo "$dirs"
    echo "RESULT: FAIL"
    exit 1
  }
  if [ -z "$dirs" ]; then
    echo "RESULT: FAIL (no testdata/*.rcz)"
    exit 1
  fi
  # RaceChrono VBO exports in testdata/ carry RaceChrono's track definition
  args=""
  for v in testdata/*.vbo; do
    [ -e "$v" ] || continue
    args="$args--course
$v
"
  done
  args="$args$dirs"
  old_ifs="$IFS"
  IFS='
'
  set -- $args
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

# ---- UI on the PC --------------------------------------------------------------------------------

run_render() {
  header
  log="$BUILD/ui_render.log"
  : >"$log"
  "$CXX" $CXXFLAGS -o "$BUILD/ui_render" ui/*.cpp tools/host/render*.cpp >>"$log" 2>&1 || build_failed "$log"
  if [ -s "$log" ]; then
    echo "build warnings ($log):"
    cat "$log"
  fi
  echo "build: OK"
  "$BUILD/ui_render" "$OUT/render"
}

run_assets() {
  header
  if ! python3 -c "import freetype, uharfbuzz, numpy, PIL" >/dev/null 2>&1; then
    echo "FAIL: Python modules missing. Run: pip3 install freetype-py uharfbuzz numpy pillow"
    exit 1
  fi
  sh tools/assets/fetch_fonts.sh "$BUILD/fonts" || {
    echo "RESULT: FAIL (fonts)"
    exit 1
  }
  python3 -I tools/assets/gen_assets.py "$BUILD/fonts" ui || {
    echo "RESULT: FAIL (generator)"
    exit 1
  }
  run_render
}

# ---- report / web pages --------------------------------------------------------------------------

build_report() {
  build "$BUILD/report" tools/host/report.cpp tools/host/report_session.cpp tools/host/report_build.cpp \
    tools/host/rcz.cpp tools/host/vboread.cpp
}

run_report() {
  header
  build_report
  dirs="$(extract_rcz)" || {
    echo "$dirs"
    echo "RESULT: FAIL"
    exit 1
  }
  args=""
  for v in testdata/*.vbo; do
    [ -e "$v" ] || continue
    args="$args$v
"
  done
  args="$args$dirs"
  if [ -z "$args" ]; then
    echo "RESULT: FAIL (no testdata/*.vbo or *.rcz)"
    exit 1
  fi
  old_ifs="$IFS"
  IFS='
'
  set -- $args
  IFS="$old_ifs"
  "$BUILD/report" "$@" --courses data/courses.json --out "$OUT"
}

run_web() {
  header
  if ! command -v python3 >/dev/null 2>&1; then
    echo "FAIL: python3 not found. Run: xcode-select --install"
    exit 1
  fi
  python3 tools/web/embed.py || exit 1
  build_report
  dirs="$(extract_rcz)" || {
    echo "$dirs"
    echo "RESULT: FAIL"
    exit 1
  }
  vbo=""
  for v in testdata/*.vbo; do
    [ -e "$v" ] && vbo="$v" && break
  done
  if [ -z "$vbo" ]; then
    echo "RESULT: FAIL (no testdata/*.vbo)"
    exit 1
  fi
  old_ifs="$IFS"
  IFS='
'
  set -- --report-bin "$BUILD/report" --vbo "$vbo" --out "$OUT/web"
  for d in $dirs; do
    set -- "$@" --rcz "$d"
  done
  IFS="$old_ifs"
  python3 tools/web/page_test.py "$@"
}

# ---- webcheck (P0-W) ---------------------------------------------------------------------------

# arduino-cli from PATH, or the one inside the Arduino IDE 2 app (with the IDE's settings)
find_cli() {
  CLI_CONF=""
  if command -v arduino-cli >/dev/null 2>&1; then
    CLI="$(command -v arduino-cli)"
    return 0
  fi
  for app in "/Applications/Arduino IDE.app" "$HOME/Applications/Arduino IDE.app"; do
    [ -d "$app" ] || continue
    c="$(find "$app" -type f -name arduino-cli 2>/dev/null | head -n 1)"
    if [ -n "$c" ] && [ -x "$c" ]; then
      CLI="$c"
      if [ -f "$HOME/.arduinoIDE/arduino-cli.yaml" ]; then
        CLI_CONF="$HOME/.arduinoIDE/arduino-cli.yaml"
      fi
      return 0
    fi
  done
  return 1
}

cli() {
  if [ -n "$CLI_CONF" ]; then
    "$CLI" --config-file "$CLI_CONF" "$@"
  else
    "$CLI" "$@"
  fi
}

# The USB serial port of the board: the one given, or the only one present
find_port() {
  if [ -n "$PORT_ARG" ]; then
    if [ -e "$PORT_ARG" ]; then
      PORT="$PORT_ARG"
      return 0
    fi
    echo "FAIL: $PORT_ARG does not exist"
    return 1
  fi
  ports=""
  n=0
  for p in /dev/cu.usbserial* /dev/cu.SLAB_USBtoUART* /dev/cu.wchusbserial* /dev/cu.usbmodem*; do
    [ -e "$p" ] || continue
    ports="$ports $p"
    n=$((n + 1))
    PORT="$p"
  done
  if [ "$n" -eq 1 ]; then
    return 0
  fi
  if [ "$n" -eq 0 ]; then
    echo "FAIL: no USB serial port found. Connect the ESP32 with a USB data cable."
    echo "ports now: $(ls /dev/cu.* 2>/dev/null | tr '\n' ' ')"
  else
    echo "FAIL: several USB serial ports:$ports"
    echo "give the board's port: ./tools/go.sh webcheck <port>"
  fi
  return 1
}

# Prints the serial output with the time until Ctrl-C or until the port goes away
monitor() {
  echo "serial: $PORT 115200. Ctrl-C stops; the output is also in $OUT/$CMD.log"
  if ! exec 3<"$PORT"; then
    echo "FAIL: cannot open $PORT"
    return 1
  fi
  stty 115200 cs8 -cstopb -parenb raw -echo clocal <&3
  while IFS= read -r line <&3; do
    echo "$(date '+%H:%M:%S') ${line%$'\r'}"
  done
  echo "serial port closed"
}

run_webcheck() {
  header
  if [ ! -f "$SKETCH/wifi_secrets.h" ]; then
    echo "FAIL: $SKETCH/wifi_secrets.h is missing. Make it from the example and enter the Wi-Fi names and passwords:"
    echo "  cp $SKETCH/wifi_secrets_example.h $SKETCH/wifi_secrets.h"
    exit 1
  fi
  if cmp -s "$SKETCH/wifi_secrets.h" "$SKETCH/wifi_secrets_example.h"; then
    echo "FAIL: $SKETCH/wifi_secrets.h still has the example values. Enter the Wi-Fi names and passwords."
    exit 1
  fi
  if command -v git >/dev/null 2>&1 && git --no-optional-locks rev-parse --git-dir >/dev/null 2>&1; then
    if ! git --no-optional-locks check-ignore -q "$SKETCH/wifi_secrets.h"; then
      echo "FAIL: git does not ignore $SKETCH/wifi_secrets.h (the passwords would go into the public repository)"
      exit 1
    fi
  fi
  if ! find_cli; then
    echo "FAIL: arduino-cli not found (neither in PATH nor in the Arduino IDE app)"
    exit 1
  fi
  echo "arduino-cli: $CLI$([ -n "$CLI_CONF" ] && echo " (settings: $CLI_CONF)")"
  core="$(cli core list 2>/dev/null | awk '$1 == "esp32:esp32" { print $2 }')"
  if [ -z "$core" ]; then
    echo "FAIL: the esp32 boards package (Espressif) is not installed for this arduino-cli"
    exit 1
  fi
  echo "esp32 core: $core"
  case "$core" in
    3.*) ;;
    *) echo "note: the sketch is written for arduino-esp32 3.x" ;;
  esac
  find_port || exit 1
  echo "port: $PORT"

  mkdir -p build/webcheck
  echo "build: $FQBN"
  if ! cli compile --fqbn "$FQBN" --build-path "$ROOT/build/webcheck" "$SKETCH" >build/webcheck/compile.log 2>&1; then
    cat build/webcheck/compile.log
    echo "RESULT: FAIL (build)"
    exit 1
  fi
  grep -E "^Sketch uses|^Global variables" build/webcheck/compile.log
  echo "flash: $PORT"
  if ! cli upload -p "$PORT" --fqbn "$FQBN" --input-dir "$ROOT/build/webcheck" "$SKETCH" >build/webcheck/upload.log 2>&1; then
    tail -n 5 build/webcheck/upload.log
    echo "flash failed at the default speed; trying again at 115200"
    if ! cli upload -p "$PORT" --fqbn "$FQBN,UploadSpeed=115200" --input-dir "$ROOT/build/webcheck" "$SKETCH" >build/webcheck/upload.log 2>&1; then
      cat build/webcheck/upload.log
      echo "hint: close other programs that use $PORT (e.g. the Arduino IDE serial monitor)."
      echo "hint: if it stops at 'Connecting...', hold the board's BOOT button (if it has one) until writing starts."
      echo "RESULT: FAIL (flash)"
      exit 1
    fi
  fi
  grep -i -E "chip is|chip type|features|crystal|^mac|flash size|hash of data verified|hard resetting" build/webcheck/upload.log |
    awk '!seen[$0]++'
  echo "flash: OK"
  monitor
}

# ---- laptimer firmware (2.41 board) ------------------------------------------------------------

# Checks the Arduino environment the laptimer needs: esp32 core 3.x and NimBLE-Arduino 1.4.3
lt_env() {
  if ! find_cli; then
    echo "FAIL: arduino-cli not found (neither in PATH nor in the Arduino IDE app)"
    return 1
  fi
  echo "arduino-cli: $CLI$([ -n "$CLI_CONF" ] && echo " (settings: $CLI_CONF)")"
  core="$(cli core list 2>/dev/null | awk '$1 == "esp32:esp32" { print $2 }')"
  case "$core" in
    3.*) echo "esp32 core: $core" ;;
    "")
      echo "FAIL: the esp32 boards package (Espressif) is not installed for this arduino-cli"
      return 1
      ;;
    *)
      echo "FAIL: esp32 core $core; the laptimer needs 3.x (3.3.11 is what it was built with)"
      return 1
      ;;
  esac
  nimble="$(cli lib list 2>/dev/null | awk '$1 == "NimBLE-Arduino" { print $2 }')"
  if [ "$nimble" != "1.4.3" ]; then
    echo "FAIL: NimBLE-Arduino ${nimble:-not installed}; the laptimer (like the round gauge) needs 1.4.3:"
    echo "  install it in the Arduino IDE library manager by choosing version 1.4.3"
    return 1
  fi
  echo "NimBLE-Arduino: $nimble"
}

# Generated sources: web pages (web/*.html) and the default config files (data/*.json)
lt_prepare() {
  if ! command -v python3 >/dev/null 2>&1; then
    echo "FAIL: python3 not found. Run: xcode-select --install"
    return 1
  fi
  python3 tools/web/embed.py || return 1
  python3 - "$LT_SKETCH/src/app/defaults_data.h" data/courses.json data/maint.json data/checklist.json <<'PY' || return 1
import sys, pathlib
out = pathlib.Path(sys.argv[1])
names = {"courses.json": "kDefaultCoursesJson", "maint.json": "kDefaultMaintJson", "checklist.json": "kDefaultChecklistJson"}
lines = ["// defaults_data.h - generated by tools/go.sh from data/*.json (do not edit; not in git)", "#pragma once", ""]
for p in map(pathlib.Path, sys.argv[2:]):
    text = p.read_text(encoding="utf-8")
    body = text.replace("\\", "\\\\").replace('"', '\\"').split("\n")
    lines.append("static const char %s[] =" % names[p.name])
    lines += ['    "%s\\n"' % l for l in body[:-1]] + ['    "%s";' % body[-1]]
    lines.append("")
out.write_text("\n".join(lines), encoding="utf-8")
print("defaults: %s" % out)
PY
  # every Japanese character of the UI and the app must be in the generated fonts (else drawn as a box)
  python3 - ui/fonts_data.cpp ui firmware/laptimer/src/app <<'PY' || return 1
import pathlib, re, sys
codes = set(int(x, 16) for x in re.findall(r"\{0x([0-9A-F]{4}),", pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")))
missing = {}
for d in sys.argv[2:]:
    for p in sorted(pathlib.Path(d).glob("*.cpp")):
        if p.name == "fonts_data.cpp":
            continue
        text = re.sub(r"//[^\n]*", "", p.read_text(encoding="utf-8"))
        for lit in re.findall(r'"((?:[^"\\\n]|\\.)*)"', text):
            for ch in lit:
                if ord(ch) >= 0x80 and ord(ch) not in codes:
                    missing.setdefault(ch, p.name)
if missing:
    print("FAIL: characters not in ui/fonts_data.cpp: %s (run ./tools/go.sh assets)" % "".join(sorted(missing)))
    sys.exit(1)
print("glyphs: every UI and app character is in the fonts")
PY
  # the Wi-Fi networks are registered on the device screen (settings > Wi-Fi), not built in
  if [ -f "$LT_SKETCH/wifi_secrets.h" ]; then
    echo "note: $LT_SKETCH/wifi_secrets.h is not used (Wi-Fi is set on the device screen); it can be deleted"
  fi
}

lt_compile() {
  mkdir -p build/laptimer
  echo "build: $LT_FQBN"
  if ! cli compile --fqbn "$LT_FQBN" --build-path "$ROOT/build/laptimer" --library "$ROOT/core" --library "$ROOT/ui" \
    --warnings default "$LT_SKETCH" >build/laptimer/compile.log 2>&1; then
    cat build/laptimer/compile.log
    echo "RESULT: FAIL (build)"
    return 1
  fi
  grep -E "^Sketch uses|^Global variables" build/laptimer/compile.log
}

run_build() {
  header
  lt_env || exit 1
  lt_prepare || exit 1
  lt_compile || exit 1
  echo "RESULT: PASS (build)"
}

run_flash() {
  header
  lt_env || exit 1
  find_port || exit 1
  echo "port: $PORT"
  lt_prepare || exit 1
  lt_compile || exit 1
  echo "flash: $PORT"
  if ! cli upload -p "$PORT" --fqbn "$LT_FQBN" --input-dir "$ROOT/build/laptimer" "$LT_SKETCH" >build/laptimer/upload.log 2>&1; then
    cat build/laptimer/upload.log
    echo "hint: close other programs that use $PORT. If it does not connect, hold BOOT while pressing RESET, then retry."
    echo "RESULT: FAIL (flash)"
    exit 1
  fi
  grep -i -E "chip is|chip type|features|crystal|^mac|flash size|hash of data verified|hard resetting" build/laptimer/upload.log |
    awk '!seen[$0]++'
  echo "flash: OK"
  monitor
}

CMD="${1:-}"
case "$CMD" in
  test | laps | vbo | render | assets | report | web | webcheck | build | flash) ;;
  *)
    usage
    exit 2
    ;;
esac
PORT_ARG="${2:-}"

cd "$ROOT" || exit 1
mkdir -p "$BUILD" "$OUT"
CXX=""
if [ "$CMD" != "webcheck" ] && [ "$CMD" != "build" ] && [ "$CMD" != "flash" ]; then
  CXX="$(find_cxx)" || {
    echo "FAIL: no C++ compiler found. Run: xcode-select --install"
    exit 1
  }
fi

# Ctrl-C stops the command; tee ignores it so that the log keeps everything up to that point
trap 'echo; echo "stopped (Ctrl-C)"' INT
run_"$CMD" 2>&1 | (
  trap '' INT
  tee "$OUT/$CMD.log"
)
status="${PIPESTATUS[0]}"
echo "log: $OUT/$CMD.log"
exit "$status"
