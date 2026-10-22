#!/usr/bin/env python3
"""Exercise the real serial logger with a bounded, instrumented UART backend."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
ARDUINO = r'''
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <cassert>
extern uint32_t fakeUs;
extern uint64_t delayedMs;
struct TestSerial {
 bool ready=true,blocking=true,failWrite=false;
 int quota=-1;
 unsigned begins=0,writes=0,readyChecks=0;
 unsigned long baud=0;
 uint32_t writeCostUs=0;
 std::string output;
 void begin(unsigned long value) { baud=value;++begins; }
 void set_blocking(bool value) { blocking=value; }
 bool writeable() { ++readyChecks;return ready && quota!=0; }
 size_t write(uint8_t value) {
  assert(ready && quota!=0 && !blocking);++writes;
  if(failWrite)return 0;
  output.push_back(char(value));fakeUs+=writeCostUs;
  if(quota>0)--quota;
  return 1;
 }
};
extern TestSerial Serial;
uint32_t micros();
uint32_t millis();
void delay(unsigned long ms);
'''
COMMON = r'''
#include "log_service.h"
#include <string>
#include <cassert>
#include <algorithm>
uint32_t fakeUs=0;
uint64_t delayedMs=0;
TestSerial Serial;
uint32_t micros() { return fakeUs; }
uint32_t millis() { return fakeUs/1000; }
void delay(unsigned long ms) { delayedMs+=ms;fakeUs+=uint32_t(ms*1000); }
void drain() { for(unsigned i=0;i<100;++i)logPump(); }
'''


def run(name, level, body, extras=None):
    with tempfile.TemporaryDirectory(prefix='thermistor-serial-') as tmp:
        folder = Path(tmp)
        (folder / 'Arduino.h').write_text(ARDUINO)
        (folder / 'main.cpp').write_text(COMMON + body)
        paths = []
        for filename, contents in (extras or {}).items():
            path = folder / filename
            path.write_text(contents)
            paths.append(str(path))
        program = folder / name
        sanitizer = (['-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                      '-fno-sanitize-recover=all'] if os.environ.get('LOGGING_SANITIZERS') == '1' else [])
        subprocess.run(['g++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', *sanitizer, '-I', str(folder), '-I', str(ROOT),
                        '-DFW_LOG_LEVEL=' + str(level), str(folder / 'main.cpp'),
                        str(ROOT / 'log_service.cpp'), *paths, '-o', str(program)], check=True)
        subprocess.run([str(program)], check=True)
        if level < 4:
            assert b'DISABLED_DEBUG_SENTINEL' not in program.read_bytes()
    print('PASS:', name, flush=True)


for level in range(6):
    run('serial_level_' + str(level), level, r'''
int main() {
 logBegin(115200);logBegin(115200);
 unsigned arguments=0;
 LOG_ERROR("Test", "E%u", ++arguments);
 LOG_WARN("Test", "W%u", ++arguments);
 LOG_INFO("Test", "I%u", ++arguments);
 LOG_DEBUG("Test", "DISABLED_DEBUG_SENTINEL D%u", ++arguments);
 LOG_TRACE("Test", "T%u", ++arguments);
 assert(arguments==FW_LOG_LEVEL);
 drain();
 assert(Serial.begins==(FW_LOG_LEVEL?1U:0U));
 if(FW_LOG_LEVEL)assert(Serial.baud==115200 && !Serial.blocking);
 const char* names[]={"ERROR","WARN","INFO","DEBUG","TRACE"};
 const char* bodies[]={"E1","W2","I3",
#if FW_LOG_LEVEL >= FW_LOG_LEVEL_DEBUG
 "DISABLED_DEBUG_SENTINEL D4",
#else
 "",
#endif
 "T5"};
 std::string expected;
 for(int i=0;i<FW_LOG_LEVEL;++i)expected+="["+std::string(names[i])+"][Test] "+bodies[i]+"\r\n";
 assert(Serial.output==expected);
 unsigned unused=0;
#if FW_LOG_LEVEL < FW_LOG_LEVEL_TRACE
 LOG_TRACE_HEX((++unused,"Test"),reinterpret_cast<const uint8_t*>(++unused),++unused);
 assert(unused==0);
#endif
 (void)unused;
 logServiceDelay(25);assert(delayedMs==25);
}
''')

run('serial_bounds_and_backpressure', 5, r'''
int main() {
 logBegin(115200);LOG_INFO("Test", "%s", std::string(100,'x').c_str());
 Serial.ready=false;
 logPump();assert(Serial.writes==0 && Serial.output.empty());
 Serial.ready=true;Serial.failWrite=true;
 logPump();assert(Serial.writes==1 && Serial.output.empty());
 Serial.failWrite=false;unsigned before=Serial.writes;
 logPump();assert(Serial.writes-before==64); // A call never drains an unlimited queue.
 drain();assert(Serial.output=="[INFO][Test] "+std::string(100,'x')+"\r\n");
 Serial.output.clear();Serial.writeCostUs=250;fakeUs=UINT32_MAX-100;
 LOG_WARN("Test", "Budget across micros rollover");before=Serial.writes;
 logPump();assert(Serial.writes-before==4 && uint32_t(fakeUs-(UINT32_MAX-100))==1000);
 Serial.writeCostUs=0;drain();
 assert(Serial.output=="[WARN][Test] Budget across micros rollover\r\n");
 LOG_INFO("Test", "Startup delay still drains output");
 logServiceDelay(105);
 assert(delayedMs==105 && Serial.output.find("Startup delay still drains output")!=std::string::npos);
}
''')

run('serial_overflow_and_priority', 5, r'''
int main() {
 logBegin(115200);Serial.ready=false;
 for(unsigned i=0;i<10;++i)LOG_INFO("Test", "I%u",i);
 assert(logDroppedCount(LogLevel::INFO)==1);
 for(unsigned i=0;i<3;++i)LOG_WARN("Test", "W%u",i);
 LOG_ERROR("Test", "Important");
 assert(logDroppedCount(LogLevel::INFO)==2); // Evict INFO before WARN/ERROR.
 LOG_TRACE("Test", "Discard");assert(logDroppedCount(LogLevel::TRACE)==1);
 logPump();assert(Serial.writes==0);
 Serial.ready=true;drain();
 assert(Serial.output.find("I0\r\n")==std::string::npos);
 assert(Serial.output.find("I9\r\n")==std::string::npos);
 for(unsigned i=1;i<9;++i)assert(Serial.output.find("I"+std::to_string(i)+"\r\n")!=std::string::npos);
 for(unsigned i=0;i<3;++i)assert(Serial.output.find("W"+std::to_string(i)+"\r\n")!=std::string::npos);
 assert(Serial.output.find("[ERROR][Test] Important\r\n")!=std::string::npos);
 assert(Serial.output.find("Dropped totals: ERROR=0 WARN=0 INFO=2 DEBUG=0 TRACE=1")!=std::string::npos);
 assert(logDroppedCount(static_cast<LogLevel>(255))==0);
}
''')

run('serial_partial_lines_and_error_saturation', 5, r'''
int main() {
 logBegin(115200);LOG_TRACE("Test", "HEAD_RETAINED");
 Serial.quota=5;logPump();assert(Serial.output=="[TRAC");
 for(unsigned i=0;i<8;++i)LOG_TRACE("Test", "T%u",i);
 for(unsigned i=0;i<3;++i)LOG_WARN("Test", "W%u",i);
 LOG_ERROR("Test", "E");assert(logDroppedCount(LogLevel::TRACE)==1);
 Serial.quota=-1;drain();
 assert(Serial.output.find("[TRACE][Test] HEAD_RETAINED\r\n")==0);
 assert(Serial.output.find("[ERROR][Test] E\r\n")!=std::string::npos);
 assert(Serial.output.find("T0\r\n")==std::string::npos);
 Serial.output.clear();Serial.ready=false;
 for(unsigned i=0;i<13;++i)LOG_ERROR("Test", "E%u",i);
 assert(logDroppedCount(LogLevel::ERROR)==1); // Even all-ERROR pressure stays bounded.
 Serial.ready=true;drain();
 assert(Serial.output.find("E12\r\n")==std::string::npos);
 assert(Serial.output.find("[WARN][Logger] Dropped totals: ERROR=1")!=std::string::npos);
}
''')

run('serial_lines_hex_and_ring_wrap', 5, r'''
int main() {
 logBegin(115200);
 LOG_INFO("A\nB", "%s", std::string(1000,'x').c_str());drain();
 assert(Serial.output.size()==159 && Serial.output.substr(0,12)=="[INFO][A B] ");
 assert(Serial.output.substr(154)=="...\r\n");
 assert(std::count(Serial.output.begin(),Serial.output.end(),'\n')==1);
 Serial.output.clear();
 uint8_t data[]={0,1,0xAB,0xFF};LOG_TRACE_HEX("Bytes",data,sizeof(data));drain();
 assert(Serial.output=="[TRACE][Bytes] 000: 0001ABFF\r\n");Serial.output.clear();
 std::string expected;
 for(unsigned i=0;i<200;++i){
  LOG_INFO("Wrap", "Batch %u",i);LOG_WARN("Wrap", "W%u",i);drain();
  expected+="[INFO][Wrap] Batch "+std::to_string(i)+"\r\n[WARN][Wrap] W"+std::to_string(i)+"\r\n";
 }
 assert(Serial.output==expected && logDroppedCount(LogLevel::INFO)==0);
}
''')

run('serial_error_only_overflow_notice', 1, r'''
int main() {
 logBegin(115200);Serial.ready=false;
 for(unsigned i=0;i<13;++i)LOG_ERROR("Test", "E%u",i);
 Serial.ready=true;drain();
 assert(logDroppedCount(LogLevel::ERROR)==1);
 assert(Serial.output.find("[ERROR][Logger] Dropped totals: ERROR=1")!=std::string::npos);
 assert(Serial.output.find("[WARN]")==std::string::npos);
}
''')

run('serial_shared_across_translation_units', 3, r'''
void otherModule();
int main() {
 LOG_INFO("App", "Before begin");otherModule();logPump();assert(Serial.output.empty());
 logBegin(115200);drain();
 assert(Serial.output=="[INFO][App] Before begin\r\n[WARN][Other] Shared sink\r\n");
}
''', {'other.cpp': '#include "log_service.h"\nvoid otherModule(){LOG_WARN("Other", "Shared sink");}\n'})

# Keep all firmware producers behind the service; never touch reference sketches
# or dependency debug code, and never retain the old raw event captures.
for source in [*ROOT.glob('*.h'), *ROOT.glob('*.ino'), *ROOT.glob('*.cpp')]:
    if source.name == 'config.h':
        continue
    text = source.read_text()
    if source.name != 'log_service.cpp':
        code = re.sub(r'//[^\n]*|/\*.*?\*/', '', text, flags=re.S)
        assert not re.search(r'\bSerial\s*\.', code), source.name
    assert 'clockLogTimestamp' not in text, source.name
    assert '[Timing]' not in text and 'stimerTicks=' not in text, source.name
for name in ['log_service.h', 'log_service.cpp']:
    assert not re.search(r'\b(fopen|ofstream|EEPROM|FlashIAP)\b', (ROOT / name).read_text()), name
print('PASS: serial ownership and removed raw timestamp captures', flush=True)
