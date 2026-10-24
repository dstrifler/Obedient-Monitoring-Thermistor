#!/usr/bin/env python3
"""Enforce the seven-service constitution and compile the real Arduino wrapper."""
from pathlib import Path
import re
import subprocess
import tempfile
from host_support import ROOT, prepare, compile_host

SERVICES = ['core_service', 'sensor_service', 'storage_service', 'scheduler_service',
            'encoder_service', 'lorawan_service', 'log_service']
PAIRS = SERVICES + ['common']
HEADERS = [name + '.h' for name in PAIRS] + ['settings_defaults.h']
expected = {name + suffix for name in PAIRS for suffix in ('.h', '.cpp')} | {'config.h', 'settings_defaults.h'}
actual = {p.name for p in ROOT.iterdir() if p.suffix in ('.h', '.cpp')}
assert actual == expected, (actual - expected, expected - actual)
assert {p.name for p in ROOT.glob('*.ino')} == {'Obedient_Monitoring_Thermistor.ino'}

# Shared code contains pure contracts/helpers, never another hidden service.
common = '\n'.join((ROOT / ('common' + suffix)).read_text() for suffix in ('.h', '.cpp'))
common = re.sub(r'//[^\n]*|/\*.*?\*/', '', common, flags=re.S)
assert not re.search(r'\b(Serial|EEPROM|FlashIAP|rtos|clockBegin|lwTransmit)\b', common)
for name in expected - {'config.h'}:
    text = (ROOT / name).read_text()
    assert not re.search(r'#include\s+"[^"\n]+\.(cpp|inc)"', text), name
    for target in re.findall(r'^#include\s+"([^"\n]+)"', text, re.M):
        assert (ROOT / target).is_file(), (name, target)

# Header contracts are acyclic; common does not depend on service headers.
graph = {name: re.findall(r'^#include\s+"([^"\n]+)"', (ROOT / name).read_text(), re.M)
         for name in HEADERS}
def visit(name, ancestors):
    assert name not in ancestors, (name, ancestors)
    for dependency in graph.get(name, []):
        visit(dependency, ancestors | {name})
for name in HEADERS:
    visit(name, set())
assert not graph['common.h']

with tempfile.TemporaryDirectory(prefix='monitor-modules-') as temp:
    folder = Path(temp); prepare(folder)
    for name in HEADERS:
        subprocess.run(['g++', '-std=c++11', '-Wall', '-Wextra', '-Werror', '-I', str(folder),
                        '-x', 'c++', '-fsyntax-only', '-'],
                       input=('#include "' + name + '"\n') * 2, text=True, check=True)
    (folder / 'sketch.cpp').write_text((ROOT / 'Obedient_Monitoring_Thermistor.ino').read_text())
    (folder / 'fixture.cpp').write_text('''#include <cassert>
unsigned setupCalls=0,loopCalls=0;void appSetup(){++setupCalls;}void appLoop(){++loopCalls;}
void setup();void loop();int main(){setup();loop();loop();assert(setupCalls==1&&loopCalls==2);}
''')
    exe = compile_host(folder, ['fixture.cpp', 'sketch.cpp'], 'wrapper')
    subprocess.run([str(exe)], check=True)
print('PASS: exactly seven service pairs + common, pure shared code, acyclic/self-contained headers and real Arduino wrapper')
