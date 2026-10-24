#!/usr/bin/env python3
"""Exercise clock discipline, nonce power-loss recovery and unchanged stock radio behavior."""
"""Run actual firmware logic against host fakes; does not replace a board build."""

from pathlib import Path

import re

import os

import subprocess

import tempfile

ROOT = Path(__file__).resolve().parents[1]

def run(name, source, headers=None, link_flags=()):
    with tempfile.TemporaryDirectory(prefix='thermistor-check-') as tmp:
        folder = Path(tmp)
        for path, contents in dict({'log_service.h': ''}, **(headers or {})).items():
            (folder / path).write_text(contents)
        cpp = folder / (name + '.cpp')
        source = source.replace('storageNonceSave(', 'nonceStoreSave(').replace('storageNonceLoad(', 'nonceStoreLoad(').replace('storageNonceBegin()', 'nonceStoreBegin()')
        cpp.write_text(source)
        level_flags = (['-DFW_LOG_LEVEL=' + os.environ['LOGIC_LOG_LEVEL']]
                       if 'LOGIC_LOG_LEVEL' in os.environ else [])
        subprocess.run(['g++', '-std=c++11', '-Wall', '-Wextra', '-Werror', *level_flags,
                        '-Wno-unused-function', '-I', str(folder), str(cpp),
                        *link_flags, '-o', str(folder / name)], check=True)
        subprocess.run([str(folder / name)], check=True)
    print('PASS:', name, flush=True)

COMMON = r'''
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cassert>
#include <algorithm>
#include <vector>
#include <iostream>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <math.h>
#include <limits>
constexpr int16_t LW_ERR_RADIO_GPIO=-31997;
bool radioHalHealthy=true;
bool lwRadioHalHealthy(){return radioHalHealthy;}
using __FlashStringHelper = char;
#define F(x) x
#define HEX 16
#define SERIAL_BAUD_RATE 115200
struct SerialFake {
 bool ready=true,blocking=true;
 unsigned begins=0;
 std::string output;
 void begin(unsigned long) { ++begins; }
 void set_blocking(bool value) { blocking=value; }
 bool writeable() { return ready; }
 size_t write(uint8_t value) { assert(ready && !blocking);output.push_back(char(value));return 1; }
} Serial;
uint32_t ticks=0;
uint32_t millis() { return ticks; }
uint32_t micros() { return ticks*1000; }
void delay(uint32_t ms) { ticks += ms; }
void sensorStart() {}
void sensorPoll() {}
'''

for name in ['log_service.h', 'log_service.cpp']:
    COMMON += re.sub(r'^#include .*\n', '', (ROOT / name).read_text(), flags=re.M)

transport = (ROOT / 'lorawan_service.cpp').read_text()
timer = transport.split('// Private LoRa rollover timing.\n', 1)[1].split('// LoRa transport and lifecycle.\n', 1)[0]
COMMON += 'static constexpr int16_t LW_ERR_TIMER_ROLLOVER = -31998;\n'
COMMON += re.sub(r'^#include .*\n', '', timer, flags=re.M)

JOURNAL_FAKE = r'''
#define RADIOLIB_LORAWAN_NONCES_BUF_SIZE 18
uint32_t __etext=0, __data_start__=0, __data_end__=0;
class FlashIAPBlockDevice {
public:
 std::vector<uint8_t> data;
 int cut=-1;
 bool readFailure=false, eraseFailure=false;
 int eraseBitMode=0;
 FlashIAPBlockDevice(uint32_t, uint32_t size): data(size,255) {}
 int init() { return 0; }
 uint32_t get_program_size() { return 4; }
 uint32_t get_erase_size() { return 8192; }
 int read(void* out, uint32_t offset, size_t size) {
   if(readFailure) return -1;
   assert(offset+size<=data.size()); memcpy(out, data.data()+offset,size); return 0;
 }
 int program(const void* input,uint32_t offset,size_t size) {
   const auto* bytes=static_cast<const uint8_t*>(input);
   for(size_t i=0;i<size;++i) {
     if(cut==0) return -1;
     if(cut>0) --cut;
     assert((data.at(offset+i)&bytes[i])==bytes[i]); data[offset+i]&=bytes[i];
   }
   return 0;
 }
 int erase(uint32_t offset,size_t size) {
   if(eraseBitMode==1) { data.at(offset+8)|=0x80; return -1; }
   if(eraseBitMode==2) {
     for(size_t i=0;i<size;i+=17) data.at(offset+i)|=uint8_t(1U<<((i/17)%8));
     return -1;
   }
   if(eraseFailure) { std::fill(data.begin()+offset,data.begin()+offset+size/2,255); return -1; }
   std::fill(data.begin()+offset,data.begin()+offset+size,255); return 0;
 }
};
'''

journal = (ROOT / 'storage_service.cpp').read_text().split('// Private nonce journal.\n',1)[1].split('// Private calibration record.\n',1)[0]

run('journal', COMMON + JOURNAL_FAKE + journal + r'''
int main() {
 assert(nonceStoreScan());
 uint8_t a[18]={1}, b[18]={2};
 assert(nonceStoreSave(a));
 auto baseline=gNonceFlash.data;
 // Reboot after every possible byte boundary, including a torn commit.
 for(int cut=0;cut<64;++cut) {
   gNonceFlash.data=baseline; gNonceFlash.cut=cut;
   assert(nonceStoreScan()); bool saved=nonceStoreSave(b);
   gNonceFlash.cut=-1; bool recovered=nonceStoreScan();
   if(saved) { assert(recovered && memcmp(gNonceSaved,b,18)==0); }
   // A valid prepared body conservatively reserves b even without commit.
   // Partial bodies halt. Never fall back to a after any bytes were written.
   if(recovered) { assert(memcmp(gNonceSaved,cut==0?a:b,18)==0); }
 }
 gNonceFlash.data=baseline; assert(nonceStoreScan());
 assert(nonceStoreSave(b));
 gNonceFlash.data[gNonceLatestOffset+offsetof(NonceRecord,committed)]^=1;
 assert(nonceStoreScan() && memcmp(gNonceSaved,b,18)==0);
 gNonceFlash.data=baseline; assert(nonceStoreScan());
 for(int i=0;i<400;++i) { b[0]=i&255; b[1]=i>>8; assert(nonceStoreSave(b)); }
 assert(nonceStoreScan()); assert(memcmp(gNonceSaved,b,18)==0);
 // A corrupt committed record must stop recovery rather than roll back.
 gNonceFlash.data[gNonceLatestOffset+8]^=1; assert(!nonceStoreScan());
 gNonceFlash.data=baseline; assert(nonceStoreScan());
 // Fill the current page; interrupted erase of the other page preserves it.
 while(gNonceLatestOffset<int32_t((NONCE_DATA_SLOTS-1)*NONCE_RECORD_BYTES)) { ++b[0]; assert(nonceStoreSave(b)); }
 gNonceFlash.eraseFailure=true; ++b[0]; assert(!nonceStoreSave(b));
 gNonceFlash.eraseFailure=false; assert(nonceStoreScan()); assert(gNonceSaved[0]==uint8_t(b[0]-1));
 gNonceFlash.readFailure=true; assert(!nonceStoreSave(a));
}
''', {'Arduino.h': '', 'RadioLib.h': '', 'FlashIAPBlockDevice.h': ''})

upstream = Path(os.environ.get('RADIOLIB_UPSTREAM',
    str(ROOT.parent / '.thermistor-tools/user/libraries/RadioLib')))

error_names = {
 'RADIOLIB_ERR_NONE','RADIOLIB_ERR_UNKNOWN','RADIOLIB_ERR_NULL_POINTER',
 'RADIOLIB_ERR_INVALID_PORT','RADIOLIB_ERR_INVALID_REVISION','RADIOLIB_ERR_INVALID_MODE',
 'RADIOLIB_ERR_INVALID_DATA_RATE','RADIOLIB_ERR_INVALID_FREQUENCY','RADIOLIB_ERR_NETWORK_NOT_JOINED',
 'RADIOLIB_ERR_UPLINK_UNAVAILABLE','RADIOLIB_ERR_PACKET_TOO_LONG','RADIOLIB_ERR_DWELL_TIME_EXCEEDED',
 'RADIOLIB_ERR_TX_TIMEOUT','RADIOLIB_ERR_RX_TIMEOUT','RADIOLIB_ERR_SPI_WRITE_FAILED',
 'RADIOLIB_ERR_SPI_CMD_TIMEOUT','RADIOLIB_ERR_SPI_CMD_INVALID','RADIOLIB_ERR_SPI_CMD_FAILED',
 'RADIOLIB_ERR_CHIP_NOT_FOUND','RADIOLIB_ERR_NO_JOIN_ACCEPT','RADIOLIB_ERR_NONCES_DISCARDED',
 'RADIOLIB_ERR_CHECKSUM_MISMATCH',
}

error_defines = '\n'.join(line for line in (upstream / 'src/TypeDef.h').read_text().splitlines()
                          if line.startswith('#define ') and line.split()[1] in error_names) + '\n'

assert len(error_defines.splitlines()) == len(error_names)

clock = (ROOT / 'core_service.cpp').read_text()

clock = clock[clock.index('static bool clockReadRtcHundredths'):clock.index('bool clockBegin()')]

run('clock_rate', COMMON + r'''
struct am_hal_rtc_time_t { uint32_t ui32ReadError,ui32Minute,ui32Second,ui32Hundredths; };
bool frozen=false, invalid=false;
uint32_t rtcStart=0;
uint32_t rate=3000;
void am_hal_rtc_osc_enable() {}
uint32_t am_hal_stimer_counter_get() { return ticks*rate; }
uint32_t am_hal_rtc_time_get(am_hal_rtc_time_t* t) {
 uint32_t h=(rtcStart+(frozen?0:ticks/10))%360000;
 t->ui32Minute=h/6000; t->ui32Second=(h/100)%60; t->ui32Hundredths=h%100;
 t->ui32ReadError=invalid; return invalid;
}
''' + clock + r'''
int main() {
 assert(clockVerifyRate()); rate=3030; assert(!clockVerifyRate());
 rate=3000; rtcStart=359900; ticks=0; assert(clockVerifyRate());
 frozen=true; assert(!clockVerifyRate()); frozen=false; invalid=true; assert(!clockVerifyRate());
}
''')

lw = (ROOT / 'lorawan_service.cpp').read_text()
lw = lw[lw.index('#if !defined(LORA_USE_OTAA)'):]

node_class = lw[lw.index('class PersistentLoRaWANNode'):lw.index('static PersistentLoRaWANNode')]

save = lw[lw.index('static bool lwSaveNonces()'):lw.index('int16_t lwRestore()')]

activate = lw[lw.index('bool lwActivate()'):lw.index('// ============================================================\n// SEND UPLINK')]

wrapper = lw[lw.index('int16_t lwSendReceive('):lw.index('// RadioLib owns region')]

NODE_FAKE = r'''
#define LORA_USE_OTAA 1
#define LORA_UPLINK_FPORT 1
#define RADIOLIB_LORAWAN_NONCES_BUF_SIZE 18
#define RADIOLIB_LORAWAN_NONCES_DEV_NONCE 10
#define RADIOLIB_LORAWAN_NONCES_SIGNATURE 16
constexpr int16_t RADIOLIB_ERR_NONE=0, RADIOLIB_ERR_NETWORK_NOT_JOINED=-7,
 RADIOLIB_LORAWAN_NEW_SESSION=-1118, RADIOLIB_LORAWAN_SESSION_RESTORED=-1117,
 LW_ERR_PERSISTENCE=-32000, LW_ERR_NONCE_EXHAUSTED=-31999, LW_ERR_INVALID_ARGUMENT=-100;
uint8_t durable[18]={};
bool saveWorks=true, joinWorks=false;
int16_t nonceRestoreResult=0;
int radioCalls=0, receiveResult=0;uint8_t downlinkPort=1;
struct LoRaWANEvent_t { uint32_t fCnt=0; float freq=0; uint8_t datarate=0,fPort=1; };
uint16_t durableLimit() { return durable[10] | (uint16_t(durable[11])<<8); }
class LoRaWANNode {
protected:
 uint16_t devNonce=0;
 bool active=false;
 uint8_t buffer[18]={};
 template<class T> static void hton(uint8_t* p,T val) { for(size_t i=0;i<sizeof(T);++i) p[i]=val>>(8*i); }
 static uint16_t checkSum16(const uint8_t* p,uint16_t len) {
   uint16_t c=0; for(size_t i=0;i<len;i+=2) c^=(uint16_t(p[i])<<8)|p[i+1]; return c;
 }
public:
 LoRaWANNode()=default;
 uint8_t* getBufferNonces() { hton<uint16_t>(buffer+10,devNonce); return buffer; }
 void scheduleTransmission(uint32_t) {}
 void restore(uint16_t next) { devNonce=next;active=false; }
 bool isActivated() { return active; }
 int16_t setBufferNonces(const uint8_t* p) {
  if(nonceRestoreResult)return nonceRestoreResult;
  memcpy(buffer,p,18);devNonce=p[10]|uint16_t(p[11])<<8;active=false;return 0;
 }
 int16_t activateOTAA() {
   // Model successful TX followed by either JoinAccept or no JoinAccept.
   assert(durableLimit()>devNonce); ++radioCalls; ++devNonce;
   active=joinWorks;return joinWorks?RADIOLIB_LORAWAN_NEW_SESSION:-42;
 }
 int16_t setDatarate(uint8_t) { return 0; }
 size_t getMaxPayloadLen() { return 53; }
 int16_t sendReceive(const uint8_t*,size_t,uint8_t,uint8_t*,size_t* len,bool,LoRaWANEvent_t*,LoRaWANEvent_t* eventDown) {
   eventDown->fPort=downlinkPort;
   if(receiveResult>0) *len=3;
   if(receiveResult<0) *len=255;
   return receiveResult;
 }
};
bool nonceStoreSave(const uint8_t* p) { if(!saveWorks) return false; memcpy(durable,p,18); return true; }
struct { void standby() {} } gRadio;
'''

run('nonce_reservation_and_downlink', COMMON + NODE_FAKE + node_class + r'''
static PersistentLoRaWANNode gNode;
static uint16_t gReservedNonceLimit=0;
static bool gPersistenceHealthy=true;
static int16_t gLastActivationState=0;
static uint8_t gLWnonces[18];
size_t lwMaxPayloadBytes() { return gNode.getMaxPayloadLen(); }
''' + save + activate + wrapper + r'''
int main() {
 assert(!lwActivate() && radioCalls==1 && durableLimit()==16);
 for(int i=0;i<15;++i) assert(!lwActivate());
 assert(radioCalls==16 && durableLimit()==16);
 assert(!lwActivate() && durableLimit()==32);
 // Reboot after failed requests: skip to the durable high-water mark.
 gNode.restore(durableLimit()); gReservedNonceLimit=durableLimit();
 assert(!lwActivate() && durableLimit()==48);
 saveWorks=false; gNode.restore(48); gReservedNonceLimit=48;
 int before=radioCalls; assert(!lwActivate() && radioCalls==before && !gPersistenceHealthy);
 saveWorks=true; gPersistenceHealthy=true; gNode.restore(65535);
 assert(!lwActivate() && gLastActivationState==LW_ERR_NONCE_EXHAUSTED && radioCalls==before);
 gNode.restore(100); gReservedNonceLimit=100; joinWorks=true;
 assert(lwActivate() && durableLimit()==116);
 uint8_t data[255]={}; size_t len=255;
 assert(lwSendReceive(data,11,data,&len)==0 && len==0);
 receiveResult=1; assert(lwSendReceive(data,11,data,&len)==1 && len==3);
 downlinkPort=2;assert(lwSendReceive(data,11,data,&len)==1 && len==0);downlinkPort=1;
 receiveResult=-42; assert(lwSendReceive(data,11,data,&len)==-42 && len==0);
 assert(lwSendReceive(nullptr,11,data,&len)==LW_ERR_INVALID_ARGUMENT);
 assert(lwSendReceive(data,11,nullptr,&len)==LW_ERR_INVALID_ARGUMENT);
}
''')

guard = lw[lw.index('#if !defined(LORA_USE_OTAA)'):lw.index('#endif',lw.index('#if !defined(LORA_USE_OTAA)'))+6]

for value in (None, 0, 1):
    defines = '' if value is None else '#define LORA_USE_OTAA ' + str(value) + '\n'
    result = subprocess.run(['g++','-x','c++','-fsyntax-only','-'], input=defines+guard,
                            capture_output=True, text=True)
    assert (result.returncode == 0) == (value == 1)
    if value != 1:
        assert 'ABP needs durable session and frame-counter storage' in result.stderr

print('PASS: OTAA-only compile guard')

run('stale_page_erase_recovery', COMMON + JOURNAL_FAKE + journal + r'''
int main() {
 assert(nonceStoreScan());
 uint8_t value[18]={};
 // Fill A, transition to B, then fill B. A now contains real stale records
 // and old transition metadata, unlike an interrupted first-ever blank erase.
 for(uint32_t i=1;i<=2*NONCE_DATA_SLOTS-1;++i) {
   value[0]=i&255; value[1]=i>>8; assert(nonceStoreSave(value));
 }
 assert(gNonceLatestOffset==int32_t(NONCE_PAGE_BYTES+(NONCE_DATA_SLOTS-1)*NONCE_RECORD_BYTES));
 auto full=gNonceFlash.data;
 uint8_t previous[18]; memcpy(previous,value,18);
 uint16_t next=(uint16_t(value[1])<<8 | value[0])+1;
 value[0]=next&255; value[1]=next>>8;
 for(int mode=1;mode<=2;++mode) {
   gNonceFlash.data=full; gNonceFlash.eraseBitMode=0; assert(nonceStoreScan());
   gNonceFlash.eraseBitMode=mode; assert(!nonceStoreSave(value));
   if(mode==1) {
     NonceRecord stale; memcpy(&stale,gNonceFlash.data.data(),sizeof(stale));
     assert(stale.committed==NONCE_COMMITTED && !nonceRecordBodyValid(stale));
   }
   // Retry even if power fails during the same stale-page erase again.
   assert(nonceStoreScan() && gNonceReclaimIntent>=0 && memcmp(gNonceSaved,previous,18)==0);
   assert(!nonceStoreSave(value)); assert(nonceStoreScan());
   gNonceFlash.eraseBitMode=0; assert(nonceStoreSave(value));
   assert(nonceStoreScan() && memcmp(gNonceSaved,value,18)==0);
   // Current page damage must never be treated as stale erase debris.
   gNonceFlash.data[gNonceLatestOffset+8]^=1; assert(!nonceStoreScan());
 }
 // Power loss at every program byte of intent, snapshot, activation and the
 // new reservation: recovery either retains the safe bound or stops; no rollback.
 for(int cut=0;cut<256;++cut) {
   gNonceFlash.data=full; gNonceFlash.cut=-1; gNonceFlash.eraseBitMode=0;
   assert(nonceStoreScan()); gNonceFlash.cut=cut;
   bool saved=nonceStoreSave(value); gNonceFlash.cut=-1;
   bool recovered=nonceStoreScan();
   if(saved) { assert(recovered && memcmp(gNonceSaved,value,18)==0); }
   if(recovered) {
     assert(memcmp(gNonceSaved,value,18)==0 || memcmp(gNonceSaved,previous,18)==0);
   }
   // An interrupted copy on the newly erased, not-yet-active page is safe to retry.
   if(cut>=64 && cut<128) {
     assert(recovered && gNonceReclaimIntent>=0);
     assert(nonceStoreSave(value)); assert(nonceStoreScan());
     assert(memcmp(gNonceSaved,value,18)==0);
   }
 }
}
''', {'Arduino.h':'','RadioLib.h':'','FlashIAPBlockDevice.h':''})

run('journal_transition_compatibility', COMMON + JOURNAL_FAKE + journal + r'''
int main() {
 uint8_t value[18]={};
 // Legacy records with unused final slots migrate without touching EEPROM.
 for(uint32_t slot=0;slot<NONCE_DATA_SLOTS;++slot) {
   value[0]=slot+1;
   assert(nonceWriteRecord(slot*NONCE_RECORD_BYTES,NONCE_MAGIC,slot+1,value));
 }
 assert(nonceStoreScan()); ++value[0]; assert(nonceStoreSave(value));
 assert(nonceStoreScan() && memcmp(gNonceSaved,value,18)==0);
 // A legacy page using the transition slots must stop without erasing it.
 std::fill(gNonceFlash.data.begin(),gNonceFlash.data.end(),255);
 for(uint32_t slot=0;slot<NONCE_PAGE_BYTES/NONCE_RECORD_BYTES;++slot) {
   value[0]=slot+1;
   assert(nonceWriteRecord(slot*NONCE_RECORD_BYTES,NONCE_MAGIC,slot+1,value));
 }
 assert(nonceStoreScan()); auto full=gNonceFlash.data;
 ++value[0]; assert(!nonceStoreSave(value)); assert(full==gNonceFlash.data);
 // Transition generations remain ordered through uint32_t sequence rollover.
 std::fill(gNonceFlash.data.begin(),gNonceFlash.data.end(),255);
 for(uint32_t slot=0;slot<NONCE_DATA_SLOTS;++slot) {
   value[0]=slot+1;
   assert(nonceWriteRecord(slot*NONCE_RECORD_BYTES,NONCE_MAGIC,
     UINT32_MAX-(NONCE_DATA_SLOTS-1)+slot,value));
 }
 assert(nonceStoreScan() && gNonceSequence==UINT32_MAX);
 ++value[0]; assert(nonceStoreSave(value));
 assert(nonceStoreScan() && gNonceSequence==3 && memcmp(gNonceSaved,value,18)==0);
 // Reclaim in the reverse direction too, with populated stale records.
 while(gNonceLatestOffset%NONCE_PAGE_BYTES<int32_t((NONCE_DATA_SLOTS-1)*NONCE_RECORD_BYTES)) {
   ++value[0]; assert(nonceStoreSave(value));
 }
 uint8_t before[18]; memcpy(before,value,18); ++value[0];
 gNonceFlash.eraseBitMode=2; assert(!nonceStoreSave(value));
 assert(nonceStoreScan() && memcmp(gNonceSaved,before,18)==0);
 gNonceFlash.eraseBitMode=0; assert(nonceStoreSave(value));
 assert(nonceStoreScan() && memcmp(gNonceSaved,value,18)==0);
}
''', {'Arduino.h':'','RadioLib.h':'','FlashIAPBlockDevice.h':''})

run('clock_transient_reads_and_sampling', COMMON + r'''
struct am_hal_rtc_time_t { uint32_t ui32ReadError,ui32Minute,ui32Second,ui32Hundredths; };
uint32_t epoch=0,rtcStart=0,rate=3000;
unsigned reads=0,failAt=0,failures=0,jitterAt=0;
bool invalid=false,frozen=false;
void am_hal_rtc_osc_enable() {}
uint32_t am_hal_stimer_counter_get() { return ticks*rate; }
uint32_t am_hal_rtc_time_get(am_hal_rtc_time_t* t) {
 ++reads;
 uint32_t h=(rtcStart+(frozen?0:uint32_t(ticks-epoch)/10))%360000;
 t->ui32Minute=h/6000;t->ui32Second=(h/100)%60;t->ui32Hundredths=h%100;
 bool error=invalid || (failAt && reads>=failAt && failures);
 if(failAt && reads>=failAt && failures) --failures;
 t->ui32ReadError=error;
 if(reads==jitterAt) ticks+=100; // preemption after RTC sampling, before STIMER
 return error;
}
''' + clock + r'''
void resetClock(uint32_t start=0) {
 ticks=start;epoch=start;rate=3000;rtcStart=0;reads=0;failAt=0;failures=0;jitterAt=0;invalid=false;frozen=false;
}
int main() {
 resetClock(); failAt=100;failures=1;assert(clockVerifyRate());
 resetClock(); failAt=1;failures=6;assert(clockVerifyRate()); // bounded window retry
 resetClock(); jitterAt=1;assert(clockVerifyRate());
 resetClock(); jitterAt=100;assert(clockVerifyRate());
 resetClock(UINT32_MAX-10000);rtcStart=359900;assert(clockVerifyRate());
 resetClock();invalid=true;assert(!clockVerifyRate() && reads<=15);
 resetClock();frozen=true;assert(!clockVerifyRate() && ticks<=90100);
 resetClock();rate=3030;assert(!clockVerifyRate());
}
''')

import importlib.util

import os

import shutil

spec = importlib.util.spec_from_file_location('verify_radiolib', ROOT / 'tools/verify_radiolib.py')

verifier = importlib.util.module_from_spec(spec)

spec.loader.exec_module(verifier)

upstream = Path(os.environ.get('RADIOLIB_UPSTREAM',
    str(ROOT.parent / '.thermistor-tools/user/libraries/RadioLib')))

verifier.verify(upstream)

with tempfile.TemporaryDirectory(prefix='thermistor-library-check-') as tmp:
    copy = Path(tmp)
    paths = ['library.properties', *verifier.UPSTREAM_HASHES]
    for relative in paths:
        destination = copy / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(upstream / relative, destination)
    before = {relative: (copy / relative).read_bytes() for relative in paths}
    verifier.verify(copy)
    assert before == {relative: (copy / relative).read_bytes() for relative in paths}
    for relative in paths:
        target = copy / relative
        target.write_bytes(before[relative] + b'\nmodified\n')
        if relative == 'library.properties':
            target.write_text('version=0.0.0\n')
        try:
            verifier.verify(copy)
        except ValueError:
            pass
        else:
            raise AssertionError('Modified dependency accepted: ' + relative)
        target.write_bytes(before[relative])

print('PASS: stock RadioLib verification (read only)', flush=True)

radio_source = (upstream / 'src/protocols/LoRaWAN/LoRaWAN.cpp').read_text()

radio_header = (upstream / 'src/protocols/LoRaWAN/LoRaWAN.h').read_text()

def radio_method(signature):
    start = radio_source.index(signature)
    return radio_source[start:radio_source.index('\n}\n', start) + 3]

schema_start = radio_header.index('#define RADIOLIB_LORAWAN_NONCES_VERSION_VAL')

schema = radio_header[schema_start:radio_header.index('enum LoRaWANSchemeSession_t', schema_start)]

templates = radio_header[radio_header.index('template<typename T>\nT LoRaWANNode::ntoh'):radio_header.rindex('#endif')]

stock_methods = ''.join(radio_method(signature) for signature in (
    'uint8_t* LoRaWANNode::getBufferNonces()',
    'int16_t LoRaWANNode::setBufferNonces(',
    'int16_t LoRaWANNode::checkBufferCommon(',
    'uint16_t LoRaWANNode::checkSum16(',
    'int16_t LoRaWANNode::activateOTAA(',
    'int16_t LoRaWANNode::transmitUplink(',
    'RadioLibTime_t LoRaWANNode::timeUntilUplink()',
))

recover = lw[lw.index('int16_t lwRecover()'):lw.index('bool lwActivate()')]

STOCK_FAKE = r'''
using RadioLibTime_t=uint32_t;
#define LORA_USE_OTAA 1
#define LORA_UPLINK_FPORT 1
#define LORA_SUB_BAND 2
#define RADIOLIB_LORAWAN_SESSION_RESTORED -1117
#define RADIOLIB_LORAWAN_NEW_SESSION -1118
#define RADIOLIB_LORAWAN_JOIN_REQUEST_LEN 23
#define RADIOLIB_LORAWAN_MODE_OTAA 1
#define RADIOLIB_LORAWAN_UPLINK 0
#define RADIOLIB_LORAWAN_DOWNLINK 1
#define RADIOLIB_LORAWAN_JOIN_ACCEPT_DELAY_1_MS 5000
#define RADIOLIB_LORAWAN_JOIN_ACCEPT_DELAY_2_MS 6000
#define RADIOLIB_ASSERT(s) do { if((s)!=0) return(s); } while(0)
#define RADIOLIB_DEBUG_PROTOCOL_PRINTLN(...) ((void)0)
#define RADIOLIB_DEBUG_PROTOCOL_HEXDUMP(...) ((void)0)
struct Hal { uint32_t millis() { return ticks; } void delay(uint32_t ms) { ticks+=ms; } };
struct Module { Hal* hal; };
struct LoRaWANChannel_t {};
struct Band { uint8_t bandNum=1; };
struct LoRaWANJoinEvent_t { bool newSession; uint16_t devNonce; uint32_t joinNonce; };
struct LoRaWANEvent_t { uint32_t fCnt=0;float freq=0;uint8_t datarate=0,fPort=1; };
uint16_t durableLimit();
struct Phy {
 Module* mod=nullptr;
 int16_t joinResult=0,uplinkResult=0;
 unsigned uplinkCalls=0;
 std::vector<uint16_t> attempted;
 Module* getMod() { return mod; }
 uint32_t getTimeOnAir(uint8_t) { return 1000; }
 int16_t transmit(uint8_t* data,uint8_t len) {
  if(len==23) {
   uint16_t nonce=data[0]|uint16_t(data[1])<<8;
   assert(nonce<durableLimit());attempted.push_back(nonce);return joinResult;
  }
  ++uplinkCalls;return uplinkResult;
 }
 void standby() {}
};
class LoRaWANNode {
public:
 Phy* phyLayer;Band* band;LoRaWANChannel_t channels[3];
 uint8_t bufferNonces[RADIOLIB_LORAWAN_NONCES_BUF_SIZE]={},txPowerMax=0,txPowerSteps=0;
 uint16_t devNonce=0,keyCheckSum=0x1234,lwMode=1,dwellTimeUp=0;
 uint8_t lwClass=0;
 uint32_t joinNonce=0,tUplink=0,rxDelayStart=0,rxDelayEnd=0,lastToA=0,dutyCycle=0;
 uint32_t rxDelays[3]={};bool isActive=false,dutyCycleEnabled=false;
 LoRaWANNode(Phy* phy=nullptr,Band* region=nullptr,uint8_t=0):phyLayer(phy),band(region) {}
 bool isActivated() { return isActive; }
 void createSession(uint8_t,uint8_t) { isActive=false; }
 void composeJoinRequest(uint8_t* data) { hton<uint16_t>(data,devNonce); }
 int16_t selectChannels() { return 0; }
 int16_t setPhyProperties(LoRaWANChannel_t*,uint8_t,int) { return 0; }
 int16_t receiveCommon(uint8_t,LoRaWANChannel_t*,uint32_t*,uint8_t,uint32_t) {
  ticks+=6000;rxDelayEnd=ticks;return joinReceiveResult;
 }
 int16_t processJoinAccept(LoRaWANJoinEvent_t*) {
  assert(uint16_t(devNonce-1)==phyLayer->attempted.back());
  ++joinNonce;hton<uint32_t>(bufferNonces+RADIOLIB_LORAWAN_NONCES_JOIN_NONCE,joinNonce,3);
  bufferNonces[RADIOLIB_LORAWAN_NONCES_ACTIVE]=1;isActive=true;return 0;
 }
 void scheduleTransmission(uint32_t when) { tUplink=when; }
 uint32_t dutyCycleInterval(uint32_t interval,uint32_t) { return interval; }
 int16_t setDatarate(uint8_t) { return 0; }
 uint8_t getMaxPayloadLen() { return 53; }
 int16_t sendReceive(const uint8_t* data,size_t len,uint8_t,uint8_t*,size_t*,bool,LoRaWANEvent_t*,LoRaWANEvent_t*) {
  if(!isActive)return RADIOLIB_ERR_NETWORK_NOT_JOINED;
  LoRaWANChannel_t channel;
  int16_t result=transmitUplink(&channel,const_cast<uint8_t*>(data),uint8_t(len),false);
  if(result==0) { ticks+=2000;rxDelayEnd=ticks; }
  return result;
 }
 static int joinReceiveResult;
 static uint16_t checkSum16(const uint8_t*,uint16_t);
 static int16_t checkBufferCommon(const uint8_t*,uint16_t);
 template<class T> static void hton(uint8_t*,T,size_t=0);
 template<class T> static T ntoh(const uint8_t*,size_t=0);
 uint8_t* getBufferNonces();
 int16_t setBufferNonces(const uint8_t*);
 int16_t activateOTAA(uint8_t=0,LoRaWANJoinEvent_t* =nullptr);
 int16_t transmitUplink(LoRaWANChannel_t*,uint8_t*,uint8_t,bool);
 RadioLibTime_t timeUntilUplink();
};
int LoRaWANNode::joinReceiveResult=1;
'''

FAKES = error_defines + r'''
constexpr int16_t LW_ERR_INVALID_ARGUMENT=-100, LW_ERR_PERSISTENCE=-32000, LW_ERR_NONCE_EXHAUSTED=-31999;
bool activated=false, joinWorks=true, persistence=true;
int joins=0, recoveries=0, sendResult=0, activationResult=-42;
bool recoveryWorks=true;
size_t limit=53;
uint32_t eligibleAt=0,forcedWait=0,waitAfterSend=0;
unsigned eligibilityQueries=0,capacityChanges=0;
bool capacityWorks=true,capacityImproves=true;
std::vector<uint32_t> sends;
std::vector<size_t> lengths;
bool clockBegin() { return true; }
bool eepromBootstrap() { return true; }
int16_t lwBegin() { return 0; }
int16_t lwRecover() { ++recoveries; if(!recoveryWorks)return RADIOLIB_ERR_SPI_CMD_TIMEOUT; activated=false;return 0; }
int16_t lwRestore() { return 0; }
bool lwPersistenceHealthy() { return persistence; }
int16_t lwLastActivationState() { return activationResult; }
bool lwIsActivated() { return activated; }
bool lwActivate() { ++joins; activated=joinWorks; return activated; }
size_t lwMaxPayloadBytes() { return limit; }
uint32_t lwTimeUntilUplink() { ++eligibilityQueries;return forcedWait?forcedWait:(int32_t(eligibleAt-ticks)>0?eligibleAt-ticks:0); }
bool lwTryCapacityDataRate(size_t,uint16_t& tried) {
 if(tried & 1) { return false; }
 tried|=1;++capacityChanges;
 if(!capacityWorks) { return false; }
 if(capacityImproves) { sendResult=0;limit=53; }
 return true;
}
int16_t lwSendReceive(const uint8_t* data,size_t size,uint8_t*,size_t* received) {
 const uint8_t expected[]={0x2f,0x09,0xdd,0x13,0xba,0xc3,0x8e,0,0,0x30,0x39};
 assert(activated); assert(*received==0); assert(size==0 || (size==24 && memcmp(data,expected,11)==0));
 sends.push_back(ticks); lengths.push_back(size);
 *received=0;
 if(waitAfterSend) { forcedWait=waitAfterSend; }
 if(sendResult==RADIOLIB_ERR_NETWORK_NOT_JOINED) activated=false;
 return sendResult;
}
'''

error_constants = FAKES[:FAKES.index('bool activated=')]

STOCK_STATE = r'''
Hal hal;Module mod;Phy gRadio;Band gRegion;
static PersistentLoRaWANNode gNode(&gRadio,&gRegion,LORA_SUB_BAND);
static uint16_t gReservedNonceLimit=0;
static bool gPersistenceHealthy=true;
static int16_t gLastActivationState=0;
static uint8_t gLWnonces[RADIOLIB_LORAWAN_NONCES_BUF_SIZE];
uint8_t durable[RADIOLIB_LORAWAN_NONCES_BUF_SIZE]={};
bool haveDurable=false,saveWorks=true,initWorks=true,loadWorks=true;
unsigned initializations=0;
uint16_t durableLimit() {
 return LoRaWANNode::ntoh<uint16_t>(durable+RADIOLIB_LORAWAN_NONCES_DEV_NONCE);
}
bool nonceStoreSave(const uint8_t* p) {
 if(!saveWorks) { return false; }
 memcpy(durable,p,sizeof(durable));haveDurable=true;return true;
}
int16_t lwBegin() {
 ++initializations;
 assert(!gNode.isActivated() && gNode.rxDelayStart==0 && gNode.rxDelayEnd==0);
 return initWorks?0:RADIOLIB_ERR_SPI_CMD_FAILED;
}
int16_t lwRestore() {
 if(!loadWorks) { gPersistenceHealthy=false;return LW_ERR_PERSISTENCE; }
 int16_t state=haveDurable?gNode.setBufferNonces(durable):0;
 gPersistenceHealthy=state==0;gReservedNonceLimit=gNode.nextDevNonce();return state;
}
bool clockBegin() { return true; }
bool eepromBootstrap() { return true; }
bool lwIsActivated() { return gNode.isActivated(); }
bool lwPersistenceHealthy() { return gPersistenceHealthy; }
int16_t lwLastActivationState() { return gLastActivationState; }
size_t lwMaxPayloadBytes() { return gNode.getMaxPayloadLen(); }
uint32_t lwTimeUntilUplink() { return gNode.timeUntilUplink(); }
bool lwTryCapacityDataRate(size_t,uint16_t&) { return false; }
'''

stock_integration = (COMMON + error_constants + schema + STOCK_FAKE + templates +
                     stock_methods + node_class + STOCK_STATE + save + recover + activate + wrapper)

restore_flash = JOURNAL_FAKE.replace('#define RADIOLIB_LORAWAN_NONCES_BUF_SIZE 18', '')

restore_journal = journal.replace('bool nonceStoreBegin()', 'bool hostUnusedNonceStoreBegin()')

restore_state = re.sub(r'bool nonceStoreSave\(const uint8_t\* p\) \{.*?\n\}', '', STOCK_STATE, flags=re.S)

restore_state = re.sub(r'int16_t lwRestore\(\) \{.*?\n\}', '', restore_state, flags=re.S)

restore_state = restore_state.replace('durable+RADIOLIB_LORAWAN_NONCES_DEV_NONCE',
                                      'gNonceSaved+RADIOLIB_LORAWAN_NONCES_DEV_NONCE')

restore_source = lw[lw.index('int16_t lwRestore()'):lw.index('// ============================================================\n// ACTIVATE')]

run('journal_only_restore_and_first_join', COMMON + error_constants + schema +
    restore_flash + restore_journal + r'''
#define EEPROM_LORAWAN_NONCES_BYTES RADIOLIB_LORAWAN_NONCES_BUF_SIZE
#define EEPROM_LORAWAN_FLAG_ADDR RADIOLIB_LORAWAN_NONCES_BUF_SIZE
#define EEPROM_LORAWAN_FLAG_VALUE 0xA5
struct Legacy : FlashIAPBlockDevice {Legacy():FlashIAPBlockDevice(0,8192){}} EEPROM;
bool nonceStoreBegin(){return nonceStoreScan();}
''' + STOCK_FAKE + templates + stock_methods + node_class + restore_state + save + restore_source + activate + r'''
void freshNode(){gNode=PersistentLoRaWANNode(&gRadio,&gRegion,LORA_SUB_BAND);}
void clearJournal(){std::fill(gNonceFlash.data.begin(),gNonceFlash.data.end(),255);gNonceFlash.readFailure=false;assert(nonceStoreScan());freshNode();}
int main(){
 mod.hal=&hal;gRadio.mod=&mod;auto blank=gNonceFlash.data;auto legacyBlank=EEPROM.data;
 assert(lwRestore()==0 && gPersistenceHealthy && !gNonceStoreHasRecord && gNode.nextDevNonce()==0);
 uint8_t history[RADIOLIB_LORAWAN_NONCES_BUF_SIZE],copied[RADIOLIB_LORAWAN_NONCES_BUF_SIZE];
 gNode.reservedNonces(history,32);assert(nonceStoreSave(history));
 assert(nonceStoreLoad(copied,sizeof(copied)) && memcmp(copied,history,sizeof(history))==0);
 assert(!nonceStoreLoad(nullptr,sizeof(copied)) && !nonceStoreLoad(copied,sizeof(copied)-1));
 // Existing journal history is preserved even when EEPROM cannot be read.
 EEPROM.readFailure=true;freshNode();assert(lwRestore()==0 && gNode.nextDevNonce()==32 && gReservedNonceLimit==32);
 assert(EEPROM.data==legacyBlank);
 auto saved=gNonceFlash.data;freshNode();gNode.keyCheckSum^=1;
 assert(lwRestore()==RADIOLIB_ERR_NONCES_DISCARDED && !gPersistenceHealthy && gNonceFlash.data==saved);
 clearJournal();EEPROM.readFailure=false;
 memcpy(EEPROM.data.data(),history,sizeof(history));EEPROM.data[EEPROM_LORAWAN_FLAG_ADDR]=0xA5;
 auto legacyHistory=EEPROM.data;
 // Even recognized legacy history is ignored, without changing EEPROM.
 assert(lwRestore()==0 && gNode.nextDevNonce()==0 && gReservedNonceLimit==0 && !gNonceStoreHasRecord);
 assert(EEPROM.data==legacyHistory && gNonceFlash.data==blank);
 // Malformed, unrecognized and unreadable legacy data cannot block startup.
 EEPROM.data[0]^=1;freshNode();assert(lwRestore()==0 && gNode.nextDevNonce()==0);
 EEPROM.data=legacyBlank;EEPROM.data[0]=0;freshNode();
 assert(lwRestore()==0 && gNode.nextDevNonce()==0 && gNonceFlash.data==blank);
 EEPROM.readFailure=true;freshNode();assert(lwRestore()==0);
 auto untouched=EEPROM.data;
 // A failed first reservation must prevent all RF, even after a fresh start.
 ticks=1000;gNonceFlash.cut=0;assert(!lwActivate());
 assert(!gPersistenceHealthy && gRadio.attempted.empty());
 gNonceFlash.cut=-1;freshNode();assert(lwRestore()==0);
 // The real activation wrapper verifies a reservation before transmitting.
 assert(lwActivate() && gRadio.attempted.size()==1 && gRadio.attempted[0]==0);
 assert(gNonceStoreHasRecord && durableLimit()==16 && EEPROM.data==untouched);
 freshNode();assert(lwRestore()==0 && gNode.nextDevNonce()==16 && gNode.joinNonce==1);
 ticks+=60000;assert(lwActivate() && gRadio.attempted.back()==16 && durableLimit()==32);
 assert(EEPROM.data==untouched);
 // Journal read errors and corruption remain failures; no fresh-start fallback.
 saved=gNonceFlash.data;gNonceFlash.readFailure=true;freshNode();
 assert(lwRestore()==LW_ERR_PERSISTENCE && !gPersistenceHealthy && gNonceFlash.data==saved);
 gNonceFlash.readFailure=false;gNonceFlash.data[0]^=1;saved=gNonceFlash.data;freshNode();
 assert(lwRestore()==LW_ERR_PERSISTENCE && !gPersistenceHealthy && gNonceFlash.data==saved);
}
''', {'Arduino.h':'', 'RadioLib.h':'', 'FlashIAPBlockDevice.h':''})

run('stock_join_nonce_skip_and_recovery', stock_integration + r'''
int main() {
 mod.hal=&hal;gRadio.mod=&mod;ticks=1000;
 gRadio.joinResult=-42;
 assert(!lwActivate() && gNode.nextDevNonce()==16 && durableLimit()==16);
 ticks+=60000;
 assert(!lwActivate() && gNode.nextDevNonce()==32 && durableLimit()==32);
 assert(gRadio.attempted[0]==0 && gRadio.attempted[1]==16);
 // A successful request TX without JoinAccept uses normal per-request batching.
 gRadio.joinResult=0;LoRaWANNode::joinReceiveResult=0;ticks+=60000;
 assert(!lwActivate() && gNode.nextDevNonce()==33 && durableLimit()==48);
 LoRaWANNode::joinReceiveResult=1;ticks+=60000;
 assert(lwActivate() && gNode.joinNonce==1 && durableLimit()==48);
 // Recreate our node and restore both the reserved bound and accepted JoinNonce.
 ticks+=60000;gNode.rxDelayStart=ticks;gNode.rxDelayEnd=ticks-1;
 assert(lwRecover()==0 && !lwIsActivated() && gNode.nextDevNonce()==48 && gNode.joinNonce==1);
 assert(gNode.rxDelayStart==0 && gNode.rxDelayEnd==0);
 assert(lwActivate() && gRadio.attempted.back()==48 && durableLimit()==64);
 // A bad reservation buffer must stop joining if uncertain-nonce restore fails.
 assert(lwRecover()==0);LoRaWANNode::joinReceiveResult=0;
 assert(!lwActivate() && gNode.nextDevNonce()==65 && durableLimit()==80);
 gLWnonces[RADIOLIB_LORAWAN_NONCES_SIGNATURE]^=1;gRadio.joinResult=-42;
 assert(!lwActivate() && !gPersistenceHealthy && gLastActivationState==RADIOLIB_ERR_CHECKSUM_MISMATCH);
 size_t attempts=gRadio.attempted.size();assert(!lwActivate() && gRadio.attempted.size()==attempts);
 // Neither an exhausted reservation nor a failed write may authorize RF.
 gPersistenceHealthy=true;gNode.isActive=false;gNode.devNonce=65534;gReservedNonceLimit=65535;
 gNode.reservedNonces(gLWnonces,65535);assert(nonceStoreSave(gLWnonces));
 assert(!lwActivate() && gNode.nextDevNonce()==65535);
 attempts=gRadio.attempted.size();assert(!lwActivate() && gLastActivationState==LW_ERR_NONCE_EXHAUSTED);
 assert(gRadio.attempted.size()==attempts);
 gNode.devNonce=100;gReservedNonceLimit=100;saveWorks=false;
 assert(!lwActivate() && !gPersistenceHealthy && gRadio.attempted.size()==attempts);
 gPersistenceHealthy=true;loadWorks=false;
 assert(lwRecover()!=0 && !gPersistenceHealthy && !lwIsActivated());
}
''')

gpio_failure_fixture = 'bool failGpioOnReceive=false,failGpioOnData=false;\n' + STOCK_FAKE.replace(
    'ticks+=6000;rxDelayEnd=ticks;return joinReceiveResult;',
    'ticks+=6000;rxDelayEnd=ticks;if(failGpioOnReceive)radioHalHealthy=false;return joinReceiveResult;').replace(
    'if(result==0) { ticks+=2000;rxDelayEnd=ticks; }',
    'if(result==0) { ticks+=2000;rxDelayEnd=ticks; }if(failGpioOnData)radioHalHealthy=false;')

gpio_failure_integration = (COMMON + error_constants + schema + gpio_failure_fixture + templates +
                            stock_methods + node_class + STOCK_STATE + save + recover + activate + wrapper)

run('radio_gpio_wrapper_failures', gpio_failure_integration + r'''
int main() {
 mod.hal=&hal;gRadio.mod=&mod;ticks=1000;
 radioHalHealthy=false;assert(!lwActivate() && lwLastActivationState()==LW_ERR_RADIO_GPIO);
 assert(gRadio.attempted.empty() && !haveDurable);
 radioHalHealthy=true;failGpioOnReceive=true;
 assert(!lwActivate() && lwLastActivationState()==LW_ERR_RADIO_GPIO && gNode.isActivated());
 assert(durableLimit()==16 && LoRaWANNode::ntoh<uint32_t>(durable+RADIOLIB_LORAWAN_NONCES_JOIN_NONCE,3)==1);
 uint8_t data[255]={};size_t down=255;
 assert(lwSendReceive(data,11,data,&down)==LW_ERR_RADIO_GPIO && down==0 && gRadio.uplinkCalls==0);
 radioHalHealthy=true;failGpioOnReceive=false;failGpioOnData=true;ticks+=60000;
 assert(lwSendReceive(data,11,data,&down)==LW_ERR_RADIO_GPIO && down==0 && gRadio.uplinkCalls==1);
}
''')

sx_source = (upstream / 'src/modules/SX126x/SX126x.cpp').read_text()

sx2_source = (upstream / 'src/modules/SX126x/SX1262.cpp').read_text()

sx2_header = (upstream / 'src/modules/SX126x/SX1262.h').read_text()

def sx_method(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]

sx_begin_declaration = re.search(r'    int16_t begin\(float freq = .*?;', sx2_header).group().strip()

assert 'tcxoVoltage = 1.6' in sx_begin_declaration

assert 'this->XTAL = false;' in sx_source

sx_constants = {
    'RADIOLIB_MODULE_SPI_WIDTH_ADDR', 'RADIOLIB_MODULE_SPI_WIDTH_CMD',
    'RADIOLIB_MODULE_SPI_COMMAND_READ', 'RADIOLIB_MODULE_SPI_COMMAND_WRITE',
    'RADIOLIB_MODULE_SPI_COMMAND_NOP', 'RADIOLIB_MODULE_SPI_COMMAND_STATUS',
    'RADIOLIB_SX126X_CMD_READ_REGISTER', 'RADIOLIB_SX126X_CMD_WRITE_REGISTER',
    'RADIOLIB_SX126X_CMD_NOP', 'RADIOLIB_SX126X_CMD_GET_STATUS',
    'RADIOLIB_SX126X_LORA_BW_500_0', 'RADIOLIB_SX126X_LORA_CR_4_7',
    'RADIOLIB_SX126X_LORA_CRC_ON', 'RADIOLIB_SX126X_LORA_HEADER_EXPLICIT',
    'RADIOLIB_SX126X_PACKET_TYPE_LORA', 'RADIOLIB_SX126X_SYNC_WORD_PRIVATE',
    'RADIOLIB_ERR_UNSUPPORTED',
}

sx_headers = '\n'.join((upstream / name).read_text() for name in (
    'src/Module.h', 'src/modules/SX126x/SX126x.h', 'src/TypeDef.h'))

sx_defines = '\n'.join(line for line in sx_headers.splitlines()
                       if line.startswith('#define ') and line.split()[1] in sx_constants) + '\n'

assert len(sx_defines.splitlines()) == len(sx_constants)

sx_methods = (sx_method(sx_source, 'int16_t SX126x::modSetup(') +
              sx_method(sx_source, 'int16_t SX126x::begin(') +
              sx_method(sx2_source, 'int16_t SX1262::begin('))

radio_begin = lw[lw.index('int16_t lwBegin()'):lw.index('bool lwIsActivated()')]

sx_fixture = r'''
#define LORA_USE_OTAA 1
#define LORA_VERSION 0
#define LORA_USE_SPI 1
#define LORA_UPLINK_FPORT 1
#define LORA_SUB_BAND 2
#define RADIOLIB_ASSERT(s) do { if((s)!=0)return(s); } while(0)
#define RADIOLIB_DEBUG_BASIC_PRINTLN(...) ((void)0)
unsigned tcxoCalls=0,rfSwitchCalls=0,failRfSwitchAt=0,nodeBegins=0,restores=0;
unsigned constructions=0;
float selectedTcxo=0;
bool chipFound=true,gPersistenceHealthy=true;
int16_t configResult=0,nodeBeginResult=0,restoreResult=0;
struct Hal { static const int GpioModeInput=0;void pinMode(int,int) {} };
struct Module {
 static const int BITS_16=16,BITS_8=8;
 struct Configuration {
  int widths[2]={},statusPos=0,cmds[4]={};bool stream=false;
  int16_t (*parseStatusCb)(uint8_t)=nullptr;
 } spiConfig;
 Hal* hal;
 void init() {}void term() {}
 int getIrq() { return 40; }int getGpio() { return 39; }
};
class SX126x {
public:
 bool XTAL=false;
 Module* mod;
 const char* chipType="SX1262";
 float bandwidthKhz=0;
 uint8_t spreadingFactor=0,bandwidth=0,codingRate=0,ldrOptimize=0,crcTypeLoRa=0,headerType=0,implicitLen=0;
 uint16_t preambleLengthLoRa=0;
 uint32_t tcxoDelay=0;
 SX126x(Module* m):mod(m) {}
 bool findChip(const char*) { return chipFound; }
 int16_t reset() { return 0; }int16_t standby() { return 0; }
 int16_t setTCXO(float voltage) { ++tcxoCalls;selectedTcxo=voltage;return 0; }
 int16_t config(uint8_t) { return configResult; }
 int16_t setRegulatorLDO() { return 0; }int16_t setRegulatorDCDC() { return 0; }
 int16_t setCodingRate(uint8_t) { return 0; }int16_t setSyncWord(uint8_t) { return 0; }
 int16_t setPreambleLength(uint16_t) { return 0; }int16_t setCurrentLimit(float) { return 0; }
 int16_t setDio2AsRfSwitch(bool=true) {
  return ++rfSwitchCalls==failRfSwitchAt?RADIOLIB_ERR_SPI_CMD_FAILED:0;
 }
 int16_t setCRC(uint8_t) { return 0; }int16_t invertIQ(bool) { return 0; }
 int16_t fixPaClamping() { return 0; }
 static int16_t SPIparseStatus(uint8_t) { return 0; }
 int16_t modSetup(float,bool,uint8_t);
 int16_t begin(uint8_t,uint8_t,uint16_t,float,bool);
};
class SX1262:public SX126x {
public:
 using SX126x::SX126x;
 int16_t setSpreadingFactor(uint8_t) { return 0; }int16_t setBandwidth(float) { return 0; }
 int16_t setFrequency(float) { return 0; }int16_t setOutputPower(int8_t) { return 0; }
 BEGIN_DECLARATION
};
Hal hal;Module mod;SX1262 gRadio(&mod);
struct Region {} gRegion;
class PersistentLoRaWANNode {
public:
 bool adr=true;
 void setADR(bool enabled) { adr=enabled; }
 PersistentLoRaWANNode(SX1262*,Region*,uint8_t) { ++constructions; }
 int16_t beginOTAA(uint64_t,uint64_t,const uint8_t*,const uint8_t*) {
  ++nodeBegins;return nodeBeginResult;
 }
} gNode(&gRadio,&gRegion,LORA_SUB_BAND);
uint64_t gJoinEUI=0,gDevEUI=0;
uint8_t gAppKey[16]={};
int16_t lwRestore() {
 ++restores;if(restoreResult!=0)gPersistenceHealthy=false;return restoreResult;
}
'''.replace('BEGIN_DECLARATION', sx_begin_declaration)

run('stock_radio_xtal_startup_and_recovery', COMMON + error_constants + sx_defines +
    sx_fixture + sx_methods + radio_begin + recover + r'''
void resetRadio() {
 gRadio.XTAL=false;tcxoCalls=0;selectedTcxo=0;rfSwitchCalls=0;failRfSwitchAt=0;
 chipFound=true;configResult=0;nodeBeginResult=0;restoreResult=0;gPersistenceHealthy=true;
}
int main() {
 mod.hal=&hal;
 // Positive control: the stock default really would request a 1.6-V TCXO.
 assert(gRadio.begin()==0 && tcxoCalls==1 && selectedTcxo==1.6f);
 resetRadio();assert(lwBegin()==0);
 assert(gRadio.XTAL && tcxoCalls==0 && nodeBegins==1 && rfSwitchCalls==2 && !gNode.adr);
 // Recovery must reapply the profile even if the flag was cleared beforehand.
 resetRadio();assert(lwRecover()==0);
 assert(gRadio.XTAL && tcxoCalls==0 && nodeBegins==2 && restores==1 && constructions==2 && !gNode.adr);
 unsigned previousBegins=nodeBegins,previousRestores=restores;
 resetRadio();chipFound=false;assert(lwRecover()==RADIOLIB_ERR_CHIP_NOT_FOUND);
 assert(gRadio.XTAL && tcxoCalls==0 && nodeBegins==previousBegins && restores==previousRestores);
 resetRadio();configResult=RADIOLIB_ERR_SPI_CMD_FAILED;assert(lwRecover()==RADIOLIB_ERR_SPI_CMD_FAILED);
 assert(tcxoCalls==0 && nodeBegins==previousBegins && restores==previousRestores);
 // Fail the explicit RF-switch setup after the driver's own successful setup.
 resetRadio();failRfSwitchAt=2;assert(lwRecover()==RADIOLIB_ERR_SPI_CMD_FAILED);
 assert(rfSwitchCalls==2 && nodeBegins==previousBegins && restores==previousRestores);
 resetRadio();nodeBeginResult=RADIOLIB_ERR_INVALID_MODE;assert(lwRecover()==RADIOLIB_ERR_INVALID_MODE);
 assert(nodeBegins==previousBegins+1 && restores==previousRestores);
 resetRadio();assert(lwRecover()==0 && tcxoCalls==0);
 assert(nodeBegins==previousBegins+2 && restores==previousRestores+1);
 resetRadio();restoreResult=LW_ERR_PERSISTENCE;assert(lwRecover()==LW_ERR_PERSISTENCE && !gPersistenceHealthy);
 unsigned before=constructions;assert(lwRecover()==LW_ERR_PERSISTENCE && constructions==before);
}
''')

# Absolute fake linker symbols exercise the production image guard without
# altering its arithmetic. All flash operations still target a host vector.
erase_fake = JOURNAL_FAKE.replace('uint32_t __etext=0, __data_start__=0, __data_end__=0;',
    'extern uint32_t __etext, __data_start__, __data_end__;')
erase_headers = {'Arduino.h':'','RadioLib.h':'','FlashIAPBlockDevice.h':''}
run('factory_erase_geometry_and_verification', COMMON + erase_fake + journal + r'''
int main(){
 assert(nonceStoreBegin());uint8_t data[18]={7},out[18]={};assert(nonceStoreSave(data));
 gNonceFlash.eraseFailure=true;assert(!nonceStoreErase());
 gNonceFlash.eraseFailure=false;assert(nonceStoreErase());assert(!nonceStoreLoad(out,18));
 for(uint8_t byte:gNonceFlash.data)assert(byte==255);
 assert(nonceStoreErase());assert(nonceStoreSave(data)&&nonceStoreLoad(out,18)&&out[0]==7);
}
''', erase_headers, ['-no-pie','-Wl,--defsym=__etext=0x10000',
    '-Wl,--defsym=__data_start__=0x20000000','-Wl,--defsym=__data_end__=0x20000000'])
run('factory_erase_rejects_firmware_overlap', COMMON + erase_fake + journal + r'''
int main(){gNonceFlash.data[0]=0;assert(!nonceStoreErase());assert(gNonceFlash.data[0]==0);}
''', erase_headers, ['-no-pie','-Wl,--defsym=__etext=0xFA001',
    '-Wl,--defsym=__data_start__=0x20000000','-Wl,--defsym=__data_end__=0x20000000'])
