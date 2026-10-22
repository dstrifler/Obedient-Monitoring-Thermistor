#!/usr/bin/env bash
set -euo pipefail
REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLS_DIR="${THERMISTOR_TOOLS_DIR:-/workspace/.thermistor-tools}"
BUILD_ROOT="${THERMISTOR_BUILD_DIR:-/workspace/work}"
STAGE_DIR="$BUILD_ROOT/Obedient_Monitoring_Thermistor"
CORE_DIR="$TOOLS_DIR/user/hardware/SparkFun/apollo3"
LOG_BUILD_FLAGS=()
if [[ -n "${THERMISTOR_LOG_LEVEL:-}" ]]; then
  case "$THERMISTOR_LOG_LEVEL" in
    0|1|2|3|4|5) ;;
    *) echo "THERMISTOR_LOG_LEVEL must be 0..5" >&2; exit 2 ;;
  esac
  # Preserve this core's dependency-generation flag when overriding extras.
  LOG_BUILD_FLAGS+=(--build-property "compiler.cxx.extra_flags=-MMD -DFW_LOG_LEVEL=$THERMISTOR_LOG_LEVEL")
fi
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
  --library "$TOOLS_DIR/user/libraries/RadioLib" \
  --fqbn SparkFun:apollo3:lora_thing_plus \
  --build-property "compiler.path=$TOOLS_DIR/toolchains/gcc-7-2017q4/bin" \
  --build-property "build.ldscript=$BUILD_ROOT/nonce-guard.ld" \
  --build-property upload.maximum_size=958464 \
  "${LOG_BUILD_FLAGS[@]}" \
  --build-path "$BUILD_ROOT/board-build" \
  "$STAGE_DIR"
