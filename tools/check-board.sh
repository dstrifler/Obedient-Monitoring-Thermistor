#!/usr/bin/env bash
set -euo pipefail
REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLS_DIR="${THERMISTOR_TOOLS_DIR:-/workspace/.thermistor-tools}"
BUILD_ROOT="${THERMISTOR_BUILD_DIR:-/workspace/work}"
STAGE_DIR="$BUILD_ROOT/Obedient_Monitoring_Thermistor"
CORE_DIR="$TOOLS_DIR/user/hardware/SparkFun/apollo3"
FW_BUILD_FLAGS=()
LIBRARY_FLAGS=(--library "$TOOLS_DIR/user/libraries/RadioLib")
EXTRA_CXX_FLAGS="-MMD"
if [[ -n "${THERMISTOR_LOG_LEVEL:-}" ]]; then
  case "$THERMISTOR_LOG_LEVEL" in
    0|1|2|3|4|5) ;;
    *) echo "THERMISTOR_LOG_LEVEL must be 0..5" >&2; exit 2 ;;
  esac
  EXTRA_CXX_FLAGS+=" -DFW_LOG_LEVEL=$THERMISTOR_LOG_LEVEL"
fi
case "${THERMISTOR_SENSOR_ENABLED:-1}" in
  0|1) SENSOR_ENABLED="${THERMISTOR_SENSOR_ENABLED:-1}" ;;
  *) echo "THERMISTOR_SENSOR_ENABLED must be 0 or 1" >&2; exit 2 ;;
esac
EXTRA_CXX_FLAGS+=" -DSENSOR_SERVICE_ENABLED=$SENSOR_ENABLED"
case "${THERMISTOR_BSEC_ENABLED:-1}" in
  0|1) BSEC_ENABLED="${THERMISTOR_BSEC_ENABLED:-1}" ;;
  *) echo "THERMISTOR_BSEC_ENABLED must be 0 or 1" >&2; exit 2 ;;
esac
EXTRA_CXX_FLAGS+=" -DSENSOR_BSEC_ENABLED=$BSEC_ENABLED"
if [[ "$SENSOR_ENABLED" == 1 ]]; then
  BME68X_DIR="${BME68X_UPSTREAM:-$TOOLS_DIR/user/libraries/BME68x}"
  if [[ ! -f "$BME68X_DIR/library.properties" ]] ||
     ! rg -q '^version=1\.3\.40408\r?$' "$BME68X_DIR/library.properties"; then
    echo "Install Bosch BME68x Sensor library 1.3.40408, or set BME68X_UPSTREAM to its directory." >&2
    exit 2
  fi
  LIBRARY_FLAGS+=(--library "$BME68X_DIR")
  if [[ "$BSEC_ENABLED" == 1 ]]; then
    BSEC_DIR="${BSEC_UPSTREAM:-$TOOLS_DIR/user/libraries/bsec2}"
    python3 "$REPO_DIR/tools/verify_bsec.py" "$BSEC_DIR"
    LIBRARY_FLAGS+=(--library "$BSEC_DIR")
    # The Apollo3 core's stock linker recipe omits Arduino's precompiled-library
    # placeholder. Supply Bosch's unmodified hard-float archive through the
    # supported build property; no core or third-party files are edited.
    FW_BUILD_FLAGS+=(--build-property "compiler.ld.extra_flags=$BSEC_DIR/src/cortex-m4/fpv4-sp-d16-hard/libalgobsec.a")
  fi
fi
# Preserve this core's dependency-generation flag when overriding extras.
FW_BUILD_FLAGS+=(--build-property "compiler.cxx.extra_flags=$EXTRA_CXX_FLAGS")
mkdir -p "$STAGE_DIR"
cp "$REPO_DIR"/*.ino "$REPO_DIR"/*.h "$REPO_DIR"/*.cpp "$STAGE_DIR/"
# Keep the core linker layout, adding a link-time guard for the two journal
# pages and the legacy EEPROM page. Runtime checks provide a second guard.
python3 - "$CORE_DIR/tools/uploaders/svl/0x10000.ld" "$BUILD_ROOT/nonce-guard.ld" <<'PY'
import pathlib, sys
linker = pathlib.Path(sys.argv[1]).read_text()
linker = linker.replace('LENGTH = 0x000F0000', 'LENGTH = 0x000EA000')
linker += '\nASSERT(LOADADDR(.data) + SIZEOF(.data) <= 0xFA000, "Firmware overlaps nonce journal")\n'
pathlib.Path(sys.argv[2]).write_text(linker)
PY
python3 "$REPO_DIR/tools/verify_radiolib.py" "$TOOLS_DIR/user/libraries/RadioLib"
exec "$TOOLS_DIR/bin/arduino-cli" compile \
  --config-file "$TOOLS_DIR/arduino-cli.yaml" \
  "${LIBRARY_FLAGS[@]}" \
  --fqbn SparkFun:apollo3:lora_thing_plus \
  --build-property "compiler.path=$TOOLS_DIR/toolchains/gcc-7-2017q4/bin" \
  --build-property "build.ldscript=$BUILD_ROOT/nonce-guard.ld" \
  --build-property upload.maximum_size=958464 \
  "${FW_BUILD_FLAGS[@]}" \
  --build-path "$BUILD_ROOT/board-build" \
  "$STAGE_DIR"
