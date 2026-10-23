#!/usr/bin/env python3
"""Build with installed Arduino packages, without editing core/library files."""
import argparse
import json
from pathlib import Path
import shutil
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cli', default='arduino-cli')
parser.add_argument('--config-file')
parser.add_argument('--bsec-dir', type=Path)
parser.add_argument('--compiler-path', help='Optional installed ARM GCC bin directory')
parser.add_argument('--output-dir', type=Path, default=ROOT.parent / (ROOT.name + '_build'))
args = parser.parse_args()
cli = [args.cli]
if args.config_file:
    cli += ['--config-file', args.config_file]
installed = json.loads(subprocess.check_output(cli + ['lib', 'list', '--format', 'json'], text=True))
libraries = {entry['library']['name']: Path(entry['library']['install_dir'])
             for entry in installed.get('installed_libraries', [])}
bsec = args.bsec_dir or libraries.get('bsec2')
bme = libraries.get('BME68x Sensor library')
radio = libraries.get('RadioLib')
if not all([bsec, bme, radio]):
    parser.error('Install bsec2 1.10.2610, BME68x Sensor library 1.3.40408 and RadioLib 7.1.2 first.')
for checker, library in [('verify_bsec.py', bsec), ('verify_radiolib.py', radio)]:
    subprocess.run(['python3', str(ROOT / 'tools' / checker), str(library)], check=True)
if 'version=1.3.40408' not in (bme / 'library.properties').read_text().splitlines():
    parser.error('BME68x Sensor library must be version 1.3.40408')

with tempfile.TemporaryDirectory(prefix='thermistor-build-') as temporary:
    work = Path(temporary)
    stage = work / 'Obedient_Monitoring_Thermistor'
    stage.mkdir()
    for pattern in ('*.ino', '*.h', '*.cpp'):
        for source in ROOT.glob(pattern):
            shutil.copyfile(source, stage / source.name)
    compile_args = cli + ['compile', '--fqbn', 'SparkFun:apollo3:lora_thing_plus',
                          '--build-path', str(work / 'objects')]
    for library in [radio, bme, bsec]:
        compile_args += ['--library', str(library.resolve())]
    if args.compiler_path:
        compile_args += ['--build-property', 'compiler.path=' + args.compiler_path]
    properties = subprocess.check_output(compile_args + ['--show-properties=expanded', str(stage)], text=True)
    values = dict(line.split('=', 1) for line in properties.splitlines() if '=' in line)
    original_linker = Path(values['build.ldscript'])
    linker_text = original_linker.read_text()
    if 'LENGTH = 0x000F0000' not in linker_text:
        parser.error('Unexpected board linker layout; reserved flash needs review.')
    linker_text = linker_text.replace('LENGTH = 0x000F0000', 'LENGTH = 0x000EA000')
    linker_text += '\nASSERT(LOADADDR(.data) + SIZEOF(.data) <= 0xFA000, "Firmware overlaps nonce journal")\n'
    linker = work / 'nonce-guard.ld'
    linker.write_text(linker_text)
    archive = bsec.resolve() / 'src/cortex-m4/fpv4-sp-d16-hard/libalgobsec.a'
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    subprocess.run(compile_args + [
        '--build-property', 'build.ldscript=' + str(linker),
        '--build-property', 'upload.maximum_size=958464',
        '--build-property', 'compiler.ld.extra_flags="' + str(archive) + '"',
        '--output-dir', str(output), str(stage)], check=True)
    print('Build complete. Firmware files:', output)
    print('Upload explicitly with:')
    print(shlex.join(cli + ['upload', '--fqbn', 'SparkFun:apollo3:lora_thing_plus',
                            '--port', '/dev/ttyUSB0', '--input-dir', str(output)]))
