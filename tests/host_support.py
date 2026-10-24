"""Hardware substitutes for consolidated-service host tests.

Production sources are copied unchanged. Specific hardware unit harnesses may
select documented private sections while the board build compiles whole services.
"""
from pathlib import Path
import os
import re
import shutil
import subprocess
ROOT = Path(__file__).resolve().parents[1]
RADIO = Path(os.environ.get('RADIOLIB_UPSTREAM', str(ROOT.parent / '.thermistor-tools/user/libraries/RadioLib')))
ARDUINO = r'''
#pragma once
#include <cstdint>
#include <cstddef>
#include <cassert>
#include <string>
extern uint32_t ticks;
extern thread_local bool inWorker;
struct SerialFake {
 bool ready=true,blocking=true;unsigned begins=0;std::string output;
 void begin(unsigned long){++begins;}
 void set_blocking(bool value){blocking=value;}
 bool writeable(){return ready;}
 size_t write(uint8_t value){assert(!inWorker && ready && !blocking);output+=char(value);return 1;}
};
extern SerialFake Serial;
uint32_t millis();uint32_t micros();void delay(uint32_t);
'''
MBED = r'''
#pragma once
#include <mutex>
namespace rtos {struct Mutex {std::mutex mutex;void lock(){mutex.lock();}void unlock(){mutex.unlock();}};}
void NVIC_SystemReset();
'''
EEPROM = r'''
#pragma once
#include "Arduino.h"
#include <cstring>
struct EEPROMFake {
 uint8_t bytes[512];unsigned puts=0,inits=0;bool failWrite=false,corruptWrite=false;
 EEPROMFake(){memset(bytes,255,sizeof(bytes));}
 int init(){assert(!inWorker);++inits;return 0;}
 void setLength(unsigned n){assert(n==512);}
 uint8_t read(size_t index){assert(!inWorker && index<512);return bytes[index];}
 template<class T>void put(size_t index,const T& data){
  assert(!inWorker && index+sizeof(T)<=512);++puts;
  if(!failWrite){memcpy(bytes+index,&data,sizeof(T));if(corruptWrite)bytes[index]^=1;}
 }
};extern EEPROMFake EEPROM;
'''
FLASH = r'''
#pragma once
#include "Arduino.h"
#include <cstring>
struct FlashFakeState {
 uint8_t bytes[16384];bool eraseFails=false,readFails=false,programFails=false;
 unsigned erases=0;const bool* acquisitionStopped=nullptr;
 FlashFakeState(){memset(bytes,255,sizeof(bytes));}
};
inline FlashFakeState& flashState(){static FlashFakeState state;return state;}
class FlashIAPBlockDevice {
 public:
 FlashIAPBlockDevice(uint32_t base,uint32_t size){assert(base==0xFA000 && size==16384);}
 int init(){assert(!inWorker);return 0;}
 uint32_t get_program_size(){return 4;}uint32_t get_erase_size(){return 8192;}
 int read(void* data,uint32_t offset,size_t size){
  assert(!inWorker && offset+size<=16384);auto& f=flashState();
  if(f.readFails){return -1;}memcpy(data,f.bytes+offset,size);return 0;
 }
 int program(const void* data,uint32_t offset,size_t size){
  assert(!inWorker && offset+size<=16384);auto& f=flashState();if(f.programFails)return -1;
  auto* bytes=static_cast<const uint8_t*>(data);
  for(size_t i=0;i<size;++i){f.bytes[offset+i]&=bytes[i];}return 0;
 }
 int erase(uint32_t offset,size_t size){
  assert(!inWorker && offset+size<=16384);auto& f=flashState();
  if(f.acquisitionStopped){assert(*f.acquisitionStopped);}++f.erases;
  if(f.eraseFails){return -1;}memset(f.bytes+offset,255,size);return 0;
 }
};
'''
STORAGE_LINK_FLAGS = ['-no-pie', '-Wl,--defsym=__etext=0x10000',
                      '-Wl,--defsym=__data_start__=0x20000000',
                      '-Wl,--defsym=__data_end__=0x20000000']

def prepare(folder):
    for pattern in ('*.h', '*.cpp'):
        for source in ROOT.glob(pattern):
            if source.name != 'config.h':
                shutil.copyfile(source, folder / source.name)
    constants = '\n'.join(line for line in (RADIO / 'src/TypeDef.h').read_text().splitlines()
                          if re.match(r'#define RADIOLIB_ERR_\w+\s+\(-?\d+\)', line))
    files = {'Arduino.h': ARDUINO, 'mbed.h': MBED, 'EEPROM.h': EEPROM,
             'FlashIAPBlockDevice.h': FLASH,
             'RadioLib.h': '#pragma once\n#define RADIOLIB_LORAWAN_NONCES_BUF_SIZE 18\n' + constants,
             'config.h': '#pragma once\n#define SERIAL_BAUD_RATE 115200\n'}
    for name, text in files.items():
        (folder / name).write_text(text)

def compile_host(folder, sources, name, flags=()):
    sanitizer = ['-fsanitize=undefined', '-fno-sanitize-recover=all'] if os.environ.get('HOST_SANITIZERS') == '1' else []
    subprocess.run(['g++', '-std=c++11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', '-pthread',
                    *sanitizer, *(STORAGE_LINK_FLAGS if 'storage_service.cpp' in sources else []), *flags, '-I', str(folder), *[str(folder / source) for source in sources],
                    '-o', str(folder / name)], check=True)
    return folder / name
