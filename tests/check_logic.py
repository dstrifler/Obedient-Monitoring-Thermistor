#!/usr/bin/env python3
"""Run actual firmware logic against host fakes; does not replace a board build."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(name, source, headers=None):
    with tempfile.TemporaryDirectory(prefix='thermistor-check-') as tmp:
        folder = Path(tmp)
        for path, contents in (headers or {}).items():
            (folder / path).write_text(contents)
        cpp = folder / (name + '.cpp')
        cpp.write_text(source)
        subprocess.run(['g++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', '-I', str(folder), str(cpp),
                        '-o', str(folder / name)], check=True)
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
using __FlashStringHelper = char;
#define F(x) x
#define HEX 16
#define SERIAL_BAUD_RATE 115200
struct SerialFake {
 void begin(int) {}
 template<class T> void print(T) {}
 template<class T> void print(T, int) {}
 template<class T> void println(T) {}
 void println() {}
} Serial;
uint32_t ticks=0;
uint32_t millis() { return ticks; }
void delay(uint32_t ms) { ticks += ms; }
'''

# Exercise the journal implementation itself with flash that can lose power
# during body programming, commit programming, or page erasure.
JOURNAL_FAKE = r'''
#define RADIOLIB_LORAWAN_NONCES_BUF_SIZE 18
uint32_t __etext=0, __data_start__=0, __data_end__=0;
class FlashIAPBlockDevice {
public:
 std::vector<uint8_t> data;
 int cut=-1;
 bool readFailure=false, eraseFailure=false;
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
   if(eraseFailure) { std::fill(data.begin()+offset,data.begin()+offset+size/2,255); return -1; }
   std::fill(data.begin()+offset,data.begin()+offset+size,255); return 0;
 }
};
'''
journal = (ROOT / 'nonce_store.h').read_text()
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
   gNonceFlash.cut=-1; assert(nonceStoreScan());
   assert(memcmp(gNonceSaved,saved?b:a,18)==0);
 }
 gNonceFlash.data=baseline; assert(nonceStoreScan());
 for(int i=0;i<400;++i) { b[0]=i&255; b[1]=i>>8; assert(nonceStoreSave(b)); }
 assert(nonceStoreScan()); assert(memcmp(gNonceSaved,b,18)==0);
 // A corrupt committed record must stop recovery rather than roll back.
 gNonceFlash.data[gNonceLatestOffset+8]^=1; assert(!nonceStoreScan());
 gNonceFlash.data=baseline; assert(nonceStoreScan());
 // Fill the current page; interrupted erase of the other page preserves it.
 while(gNonceLatestOffset<8128) { ++b[0]; assert(nonceStoreSave(b)); }
 gNonceFlash.eraseFailure=true; ++b[0]; assert(!nonceStoreSave(b));
 gNonceFlash.eraseFailure=false; assert(nonceStoreScan()); assert(gNonceSaved[0]==uint8_t(b[0]-1));
 gNonceFlash.readFailure=true; assert(!nonceStoreSave(a));
}
''', {'Arduino.h': '', 'RadioLib.h': '', 'FlashIAPBlockDevice.h': ''})

# Compile the actual sketch after replacing only its hardware includes.
sketch = re.sub(r'^#include .*\n', '', (ROOT / 'Obedient_Monitoring_Thermistor.ino').read_text(), flags=re.M)
FAKES = r'''
constexpr int16_t LW_ERR_INVALID_ARGUMENT=-100, LW_ERR_PERSISTENCE=-32000, LW_ERR_NONCE_EXHAUSTED=-31999;
constexpr int16_t RADIOLIB_ERR_NULL_POINTER=-1, RADIOLIB_ERR_INVALID_PORT=-2,
 RADIOLIB_ERR_INVALID_REVISION=-3, RADIOLIB_ERR_INVALID_MODE=-4,
 RADIOLIB_ERR_INVALID_DATA_RATE=-5, RADIOLIB_ERR_INVALID_FREQUENCY=-6,
 RADIOLIB_ERR_NETWORK_NOT_JOINED=-7, RADIOLIB_ERR_UPLINK_UNAVAILABLE=-8,
 RADIOLIB_ERR_PACKET_TOO_LONG=-9, RADIOLIB_ERR_DWELL_TIME_EXCEEDED=-10;
bool activated=false, joinWorks=true, persistence=true;
int joins=0, sendResult=0, activationResult=-42;
size_t limit=11;
uint32_t eligibleAt=0;
std::vector<uint32_t> sends;
std::vector<size_t> lengths;
bool clockBegin() { return true; }
bool eepromBootstrap() { return true; }
bool lwBegin() { return true; }
int16_t lwRestore() { return 0; }
bool lwPersistenceHealthy() { return persistence; }
int16_t lwLastActivationState() { return activationResult; }
bool lwIsActivated() { return activated; }
bool lwActivate() { ++joins; activated=joinWorks; return activated; }
size_t lwMaxPayloadBytes() { return limit; }
uint32_t lwTimeUntilUplink() { return int32_t(eligibleAt-ticks)>0?eligibleAt-ticks:0; }
int16_t lwSendReceive(const uint8_t* data,size_t size,uint8_t*,size_t* received) {
 assert(activated); assert(*received==0); assert(size==0 || (size==11 && memcmp(data,"Hello World",11)==0));
 sends.push_back(ticks); lengths.push_back(size);
 *received=0;
 if(sendResult==RADIOLIB_ERR_NETWORK_NOT_JOINED) activated=false;
 return sendResult;
}
'''
run('scheduler', COMMON + FAKES + sketch + r'''
void advance(uint32_t ms) { uint32_t start=ticks; while(uint32_t(ticks-start)<ms) loop(); }
void reset() {
 ticks=0; activated=false; joinWorks=true; persistence=true; sendResult=0; limit=11;
 eligibleAt=0; joins=0; sends.clear(); lengths.clear();
 gRetryBackoffMs=5000; gMacOnlyAttempts=0; gReportWaitMs=60000; setup();
}
int main() {
 reset(); advance(500); assert(sends.size()==1 && gPendingDownlinkLen==0);
 uint32_t first=sends[0]; advance(59000); assert(sends.size()==1);
 advance(1200); assert(sends.size()==2 && sends[1]-first>=60000 && sends[1]-first<=60100);
 reset(); joinWorks=false; advance(59000); assert(sends.empty() && joins==1);
 joinWorks=true; advance(1500); assert(sends.size()==1);
 reset(); limit=8; advance(500); assert(lengths.size()==1 && lengths[0]==0);
 limit=11; advance(5500); assert(lengths.size()==2 && lengths[1]==11);
 reset(); eligibleAt=ticks+10000; advance(500); assert(sends.empty()); advance(10100); assert(sends.size()==1);
 reset(); sendResult=-42; advance(500); assert(sends.size()==1);
 advance(5500); assert(sends.size()==2); advance(10000); assert(sends.size()==3);
 assert(sends[2]-sends[1]>=10000);
 reset(); sendResult=RADIOLIB_ERR_INVALID_PORT; advance(1000); assert(gSchedulerState==SCHED_FAULT);
 advance(60000); assert(sends.size()==1);
 reset(); joinWorks=false; persistence=false; advance(500); assert(gSchedulerState==SCHED_FAULT && sends.empty());
 reset(); sendResult=RADIOLIB_ERR_NETWORK_NOT_JOINED; advance(200); sendResult=0; advance(1000); assert(joins==2);
 reset(); ticks=UINT32_MAX-32000; eligibleAt=ticks; gNextSchedulerTickMs=ticks; gLastJoinAttemptMs=ticks-60000;
 advance(500); advance(61000); assert(sends.size()==2 && uint32_t(sends[1]-sends[0])>=60000);
 reset(); limit=0; advance(20000); assert(gSchedulerState==SCHED_FAULT && sends.size()==3);
}
''')

# Test the independent RTC measurement, including hour rollover and a stopped RTC.
clock = (ROOT / 'clock_service.h').read_text()
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

# Execute the production activation wrapper and reservation subclass. The fake
# radio refuses transmission unless a greater nonce is already durable.
lw = (ROOT / 'lorawan_service.h').read_text()
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
int radioCalls=0, receiveResult=0;
struct LoRaWANEvent_t { uint32_t fCnt=0; float freq=0; uint8_t datarate=0; };
uint16_t durableLimit() { return durable[10] | (uint16_t(durable[11])<<8); }
class LoRaWANNode {
protected:
 uint16_t devNonce=0;
 uint8_t buffer[18]={};
 template<class T> static void hton(uint8_t* p,T val) { for(size_t i=0;i<sizeof(T);++i) p[i]=val>>(8*i); }
 static uint16_t checkSum16(const uint8_t* p,uint16_t len) {
   uint16_t c=0; for(size_t i=0;i<len;i+=2) c^=(uint16_t(p[i])<<8)|p[i+1]; return c;
 }
public:
 LoRaWANNode()=default;
 uint8_t* getBufferNonces() { hton<uint16_t>(buffer+10,devNonce); return buffer; }
 void restore(uint16_t next) { devNonce=next; }
 int16_t activateOTAA() {
   assert(durableLimit()>devNonce); ++radioCalls; ++devNonce;
   return joinWorks?RADIOLIB_LORAWAN_NEW_SESSION:-42;
 }
 size_t getMaxPayloadLen() { return 11; }
 int16_t sendReceive(const uint8_t*,size_t,uint8_t,uint8_t*,size_t* len,bool,LoRaWANEvent_t*,LoRaWANEvent_t*) {
   if(receiveResult>0) *len=3;
   if(receiveResult<0) *len=255;
   return receiveResult;
 }
};
bool nonceStoreSave(const uint8_t* p) { if(!saveWorks) return false; memcpy(durable,p,18); return true; }
struct { void standby() {} } gRadio;
void clockLogTimestamp(const char*) {}
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
 receiveResult=-42; assert(lwSendReceive(data,11,data,&len)==-42 && len==0);
 assert(lwSendReceive(nullptr,11,data,&len)==LW_ERR_INVALID_ARGUMENT);
 assert(lwSendReceive(data,11,nullptr,&len)==LW_ERR_INVALID_ARGUMENT);
}
''')
