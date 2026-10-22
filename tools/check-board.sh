#!/usr/bin/env bash
set -euo pipefail
REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLS_DIR="${THERMISTOR_TOOLS_DIR:-/workspace/.thermistor-tools}"
BUILD_ROOT="${THERMISTOR_BUILD_DIR:-/workspace/work}"
STAGE_DIR="$BUILD_ROOT/Obedient_Monitoring_Thermistor"
CORE_DIR="$TOOLS_DIR/user/hardware/SparkFun/apollo3"
mkdir -p "$STAGE_DIR"
cp "$REPO_DIR"/*.ino "$REPO_DIR"/*.h "$STAGE_DIR/"
# Keep the core linker layout, adding a link-time guard for the two journal
# pages and the legacy EEPROM page. Runtime checks provide a second guard.
python3 - "$CORE_DIR/tools/uploaders/svl/0x10000.ld" "$BUILD_ROOT/nonce-guard.ld" <<'PY'
import pathlib, sys
linker = pathlib.Path(sys.argv[1]).read_text()
linker = linker.replace('LENGTH = 0x000F0000', 'LENGTH = 0x000EA000')
linker += '\nASSERT(LOADADDR(.data) + SIZEOF(.data) <= 0xFA000, "Firmware overlaps nonce journal")\n'
pathlib.Path(sys.argv[2]).write_text(linker)
PY
python3 "$REPO_DIR/tools/prepare_radiolib.py"   "$TOOLS_DIR/user/libraries/RadioLib" "$BUILD_ROOT/patched-libraries/RadioLib"
exec "$TOOLS_DIR/bin/arduino-cli" compile \
  --config-file "$TOOLS_DIR/arduino-cli.yaml" \
  --library "$BUILD_ROOT/patched-libraries/RadioLib" \
  --fqbn SparkFun:apollo3:lora_thing_plus \
  --build-property "compiler.path=$TOOLS_DIR/toolchains/gcc-7-2017q4/bin" \
  --build-property "build.ldscript=$BUILD_ROOT/nonce-guard.ld" \
  --build-property upload.maximum_size=958464 \
  --build-path "$BUILD_ROOT/board-build" \
  "$STAGE_DIR"
