#!/usr/bin/env python3
"""Check self-contained headers and the real Arduino wrapper after module changes."""
from pathlib import Path
import subprocess
import tempfile
from host_support import ROOT, prepare, compile_host
HEADERS=['app_service.h','app_settings.h','settings_defaults.h','sensor_types.h','protocol_types.h',
         'storage_service.h','sensor_service.h','scheduler_service.h','encoder_service.h','lorawan_service.h',
         'clock_service.h','lorawan_timer.h','nonce_store.h','bsec_state.h','bsec_service.h',
         'eeprom_service.h','firmware_features.h','log_config.h','log_service.h']
with tempfile.TemporaryDirectory(prefix='monitor-modules-') as temp:
    folder=Path(temp);prepare(folder)
    for name in HEADERS:
        subprocess.run(['g++','-std=c++11','-Wall','-Wextra','-Werror','-I',str(folder),
                        '-x','c++','-fsyntax-only','-'],input=('#include "'+name+'"\n')*2,text=True,check=True)
    (folder/'sketch.cpp').write_text((ROOT/'Obedient_Monitoring_Thermistor.ino').read_text())
    (folder/'fixture.cpp').write_text('''#include <cassert>
unsigned setupCalls=0,loopCalls=0;void appSetup(){++setupCalls;}void appLoop(){++loopCalls;}
void setup();void loop();int main(){setup();loop();loop();assert(setupCalls==1&&loopCalls==2);}
''')
    exe=compile_host(folder,['fixture.cpp','sketch.cpp'],'wrapper')
    subprocess.run([str(exe)],check=True)
    print('PASS: 19 self-contained/repeatable module headers and real Arduino wrapper')
