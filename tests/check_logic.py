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

# Compile the actual sketch after replacing only its hardware includes.
sketch = re.sub(r'^#include .*\n', '', (ROOT / 'Obedient_Monitoring_Thermistor.ino').read_text(), flags=re.M)
FAKES = r'''
constexpr int16_t LW_ERR_INVALID_ARGUMENT=-100, LW_ERR_PERSISTENCE=-32000, LW_ERR_NONCE_EXHAUSTED=-31999;
constexpr int16_t RADIOLIB_ERR_NULL_POINTER=-1, RADIOLIB_ERR_INVALID_PORT=-2,
 RADIOLIB_ERR_INVALID_REVISION=-3, RADIOLIB_ERR_INVALID_MODE=-4,
 RADIOLIB_ERR_INVALID_DATA_RATE=-5, RADIOLIB_ERR_INVALID_FREQUENCY=-6,
 RADIOLIB_ERR_NETWORK_NOT_JOINED=-7, RADIOLIB_ERR_UPLINK_UNAVAILABLE=-8,
 RADIOLIB_ERR_PACKET_TOO_LONG=-9, RADIOLIB_ERR_DWELL_TIME_EXCEEDED=-10;
bool activated=false, joinWorks=true, persistence=true, macFits=true;
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
bool lwMacCommandsFit() { return macFits; }
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
 ticks=0; activated=false; joinWorks=true; persistence=true; macFits=true; sendResult=0; limit=11;
 eligibleAt=0; joins=0; sends.clear(); lengths.clear();
 gRetryBackoffMs=5000; gMacOnlyAttempts=0; gReportWaitMs=60000; setup();
}
int main() {
 // Waiting for a tick sleeps through the idle interval, including rollover.
 reset(); gNextSchedulerTickMs=ticks+40;
 uint32_t idleStart=ticks; loop(); assert(uint32_t(ticks-idleStart)==40);
 reset(); ticks=UINT32_MAX-19; gNextSchedulerTickMs=ticks+40;
 idleStart=ticks; loop(); assert(uint32_t(ticks-idleStart)==40);
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
 reset(); limit=8; sendResult=-42; advance(80000);
 assert(gMacOnlyAttempts==0 && sends.size()>=4 && gSchedulerState!=SCHED_FAULT);
 sendResult=0; advance(90000); assert(gMacOnlyAttempts>=1);
 limit=11; advance(7000); assert(gMacOnlyAttempts==0);
 gMacOnlyAttempts=3; activated=false; gSchedulerState=SCHED_WAIT_FOR_JOIN;
 gLastJoinAttemptMs=ticks-60000; advance(1000); assert(gMacOnlyAttempts==0);
 reset(); limit=0; macFits=false; advance(1000); assert(gSchedulerState==SCHED_FAULT && sends.empty());
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
 uint8_t fOptsUpLen=0;
 uint8_t buffer[18]={};
 template<class T> static void hton(uint8_t* p,T val) { for(size_t i=0;i<sizeof(T);++i) p[i]=val>>(8*i); }
 static uint16_t checkSum16(const uint8_t* p,uint16_t len) {
   uint16_t c=0; for(size_t i=0;i<len;i+=2) c^=(uint16_t(p[i])<<8)|p[i+1]; return c;
 }
public:
 LoRaWANNode()=default;
 uint8_t* getBufferNonces() { hton<uint16_t>(buffer+10,devNonce); return buffer; }
 void scheduleTransmission(uint32_t) {}
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

# Compile the actual patched RadioLib functions, rather than faking their math.
import importlib.util
import os
spec = importlib.util.spec_from_file_location('prepare_radiolib', ROOT / 'tools/prepare_radiolib.py')
patcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patcher)
upstream = Path(os.environ.get('RADIOLIB_UPSTREAM',
    str(ROOT.parent / '.thermistor-tools/user/libraries/RadioLib')))
patched = patcher.patch_source((upstream / 'src/protocols/LoRaWAN/LoRaWAN.cpp').read_text())
get_max = patched[patched.index('uint8_t LoRaWANNode::getMaxPayloadLen()'):
                  patched.index('int16_t LoRaWANNode::findDataRate')]
mac_fit = lw[lw.index('  bool macCommandsFit()'):lw.index('  uint16_t nextDevNonce()')]
run('real_library_payload_limits', COMMON + r'''
#define RADIOLIB_LORAWAN_UPLINK 0
#define RADIOLIB_MIN(a,b) ((a)<(b)?(a):(b))
struct Band { uint8_t payloadLenMax[1]={11}; };
struct Channel { uint8_t dr=0; };
struct Phy { uint32_t getTimeOnAir(uint8_t bytes) { return uint32_t(bytes)*1000; } };
class LoRaWANNode {
public:
 Band* band; Phy* phyLayer; Channel channels[1];
 uint8_t txPowerMax=0,txPowerSteps=0,fOptsUpLen=0;
 bool TS011=false; uint32_t dwellTimeUp=0;
 int setPhyProperties(Channel*,uint8_t,int) { return 0; }
 uint8_t getMaxPayloadLen();
''' + mac_fit + '};\n' + get_max + r'''
int main() {
 Band band; Phy phy; LoRaWANNode node; node.band=&band; node.phyLayer=&phy;
 assert(node.getMaxPayloadLen()==11);
 for(uint8_t pending=0;pending<=15;++pending) {
   node.fOptsUpLen=pending;
   assert(node.getMaxPayloadLen()==(pending<11?11-pending:0));
   assert(node.macCommandsFit()==(pending<=11));
   assert(node.fOptsUpLen==pending);
 }
 band.payloadLenMax[0]=242; node.fOptsUpLen=15; assert(node.getMaxPayloadLen()==227);
 node.dwellTimeUp=30; node.fOptsUpLen=4;
 assert(node.getMaxPayloadLen()<=13);
 node.fOptsUpLen=30; assert(node.getMaxPayloadLen()==0);
}
''')
time_until = patched[patched.index('RadioLibTime_t LoRaWANNode::timeUntilUplink()'):
                     patched.index('uint8_t LoRaWANNode::getMaxPayloadLen()')]
transmit = patched[patched.index('int16_t LoRaWANNode::transmitUplink('):
                   patched.index('// static callback') if '// static callback' in patched else patched.index('static volatile bool downlinkAction')]
# Bound the method by the following section instead of copying callback state.
transmit = transmit[:transmit.index('\n}\n')+3]
run('real_library_timer_rollover', COMMON + r'''
using RadioLibTime_t=uint32_t;
#define RADIOLIB_LORAWAN_UPLINK 0
#define RADIOLIB_ERR_UNKNOWN -99
#define RADIOLIB_ERR_NONE 0
#define RADIOLIB_ERR_UPLINK_UNAVAILABLE -8
#define RADIOLIB_ASSERT(state) do { if((state)!=0) return(state); } while(0)
#define RADIOLIB_DEBUG_PROTOCOL_PRINTLN(...) ((void)0)
struct Hal { uint32_t millis() { return ticks; } void delay(uint32_t ms) { assert(ms<10000); ticks+=ms; } };
struct Module { Hal* hal; };
struct LoRaWANChannel_t {};
struct Phy {
 Module* mod; int calls=0;
 Module* getMod() { return mod; }
 uint32_t getTimeOnAir(uint8_t) { return 1000; }
 int16_t transmit(uint8_t*,uint8_t) { ++calls; return 0; }
};
class LoRaWANNode {
public:
 Phy* phyLayer;
 uint32_t rxDelayStart=0,rxDelayEnd=0,tUplink=0,dutyCycle=0,lastToA=0;
 bool dutyCycleEnabled=false; uint8_t txPowerMax=0,txPowerSteps=0;
 uint32_t dutyCycleInterval(uint32_t interval,uint32_t) { return interval; }
 int16_t setPhyProperties(LoRaWANChannel_t*,uint8_t,int) { return 0; }
 RadioLibTime_t timeUntilUplink();
 int16_t transmitUplink(LoRaWANChannel_t*,uint8_t*,uint8_t,bool);
};
''' + time_until + transmit + r'''
int main() {
 Hal hal; Module mod; mod.hal=&hal; Phy phy; phy.mod=&mod;
 LoRaWANNode node; node.phyLayer=&phy;
 ticks=500; node.rxDelayStart=UINT32_MAX-1000; node.rxDelayEnd=20;
 node.tUplink=UINT32_MAX-500;
 assert(node.timeUntilUplink()==0);
 LoRaWANChannel_t channel; uint8_t data=0;
 assert(node.transmitUplink(&channel,&data,1,false)==0 && phy.calls==1 && ticks==500);
 node.dutyCycleEnabled=true; node.dutyCycle=2000;
 node.rxDelayStart=UINT32_MAX-1000; node.rxDelayEnd=20;
 assert(node.timeUntilUplink()==499);
 assert(node.transmitUplink(&channel,&data,1,false)==RADIOLIB_ERR_UPLINK_UNAVAILABLE);
 ticks+=499; assert(node.timeUntilUplink()==0);
 assert(node.transmitUplink(&channel,&data,1,false)==0 && phy.calls==2);
 // A future scheduled time can itself wrap to a smaller integer.
 node.dutyCycleEnabled=false; ticks=UINT32_MAX-99;
 node.rxDelayStart=ticks-500; node.rxDelayEnd=ticks-100; node.tUplink=100;
 assert(node.transmitUplink(&channel,&data,1,false)==0 && ticks==100);
}
''')

# Exercise the production compile guard for every supported activation value.
guard = lw[lw.index('#if !defined(LORA_USE_OTAA)'):lw.index('#endif',lw.index('#if !defined(LORA_USE_OTAA)'))+6]
for value in (None, 0, 1):
    defines = '' if value is None else '#define LORA_USE_OTAA ' + str(value) + '\n'
    result = subprocess.run(['g++','-x','c++','-fsyntax-only','-'], input=defines+guard,
                            capture_output=True, text=True)
    assert (result.returncode == 0) == (value == 1)
    if value != 1:
        assert 'ABP needs durable session and frame-counter storage' in result.stderr
print('PASS: OTAA-only compile guard')

# Both a changed upstream source and an unpatched firmware build must fail.
try:
    patcher.patch_source('unexpected source')
    raise AssertionError('Unsupported RadioLib source was accepted')
except ValueError:
    pass
patch_guard = lw[lw.index('#ifndef RADIOLIB_THERMISTOR_PROTOCOL_FIXES'):
                 lw.index('#endif',lw.index('#ifndef RADIOLIB_THERMISTOR_PROTOCOL_FIXES'))+6]
for prepared in (False, True):
    defines = '#define RADIOLIB_THERMISTOR_PROTOCOL_FIXES 1\n' if prepared else ''
    result = subprocess.run(['g++','-x','c++','-fsyntax-only','-'], input=defines+patch_guard,
                            capture_output=True,text=True)
    assert (result.returncode==0)==prepared
print('PASS: patched-library and source-version guards')

# Exercise the real receive-window entry guard around rollover. The rest of
# receiveCommon talks to the modem and is covered only by the board build.
start = patched.index('int16_t LoRaWANNode::receiveCommon(')
rx_gate = patched[start:patched.index('  // setup interrupt', start)]
rx_gate = rx_gate.replace('  int16_t state = RADIOLIB_ERR_UNKNOWN;\n', '')
rx_gate += '  (void)dir; (void)dlChannels; return 0;\n}\n'
run('real_library_receive_window_rollover', COMMON + r'''
using RadioLibTime_t=uint32_t;
#define RADIOLIB_ERR_NO_RX_WINDOW -9
struct Hal { uint32_t millis() { return ticks; } void delay(uint32_t ms) { assert(ms<3000); ticks+=ms; } };
struct Module { Hal* hal; };
struct LoRaWANChannel_t {};
struct Phy { Module* mod; Module* getMod() { return mod; } };
class LoRaWANNode {
public:
 Phy* phyLayer; uint32_t scanGuard=10,rxDelayEnd=0;
 int16_t receiveCommon(uint8_t,const LoRaWANChannel_t*,const RadioLibTime_t*,uint8_t,RadioLibTime_t);
};
''' + rx_gate + r'''
int main() {
 Hal hal; Module mod; mod.hal=&hal; Phy phy; phy.mod=&mod;
 LoRaWANNode node; node.phyLayer=&phy;
 uint32_t delays[3]={0,1000,2000}; LoRaWANChannel_t channels[3];
 uint32_t reference=UINT32_MAX-600;
 ticks=UINT32_MAX-500;
 assert(node.receiveCommon(0,channels,delays,2,reference)==0);
 ticks=1000;
 assert(node.receiveCommon(0,channels,delays,2,reference)==RADIOLIB_ERR_NO_RX_WINDOW);
 assert(ticks==1399 && node.rxDelayEnd==1399);
}
''')

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
