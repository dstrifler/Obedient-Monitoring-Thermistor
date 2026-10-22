#!/usr/bin/env python3
"""Run actual firmware logic against host fakes; does not replace a board build."""
from pathlib import Path
import re
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(name, source, headers=None):
    with tempfile.TemporaryDirectory(prefix='thermistor-check-') as tmp:
        folder = Path(tmp)
        for path, contents in dict({'log_service.h': ''}, **(headers or {})).items():
            (folder / path).write_text(contents)
        cpp = folder / (name + '.cpp')
        cpp.write_text(source)
        level_flags = (['-DFW_LOG_LEVEL=' + os.environ['LOGIC_LOG_LEVEL']]
                       if 'LOGIC_LOG_LEVEL' in os.environ else [])
        subprocess.run(['g++', '-std=c++11', '-Wall', '-Wextra', '-Werror', *level_flags,
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
#include <cstdio>
#include <cstdarg>
#include <string>
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
'''
for name in ['log_config.h', 'log_service.h', 'log_service.cpp']:
    COMMON += re.sub(r'^#include .*\n', '', (ROOT / name).read_text(), flags=re.M)
COMMON += re.sub(r'^#include .*\n', '', (ROOT / 'lorawan_timer.h').read_text(), flags=re.M)

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
import os
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
FAKES = error_defines + r'''
constexpr int16_t LW_ERR_INVALID_ARGUMENT=-100, LW_ERR_PERSISTENCE=-32000, LW_ERR_NONCE_EXHAUSTED=-31999;
bool activated=false, joinWorks=true, persistence=true;
int joins=0, recoveries=0, sendResult=0, activationResult=-42;
bool recoveryWorks=true;
size_t limit=11;
uint32_t eligibleAt=0,forcedWait=0,waitAfterSend=0;
unsigned eligibilityQueries=0,capacityChanges=0;
bool capacityWorks=true,capacityImproves=true;
std::vector<uint32_t> sends;
std::vector<size_t> lengths;
bool clockBegin() { return true; }
bool eepromBootstrap() { return true; }
bool lwBegin() { return true; }
bool lwRecover() { ++recoveries; if(!recoveryWorks)return false; activated=false;return true; }
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
 if(capacityImproves) { sendResult=0;limit=11; }
 return true;
}
int16_t lwSendReceive(const uint8_t* data,size_t size,uint8_t*,size_t* received) {
 assert(activated); assert(*received==0); assert(size==0 || (size==11 && memcmp(data,"Hello World",11)==0));
 sends.push_back(ticks); lengths.push_back(size);
 *received=0;
 if(waitAfterSend) { forcedWait=waitAfterSend; }
 if(sendResult==RADIOLIB_ERR_NETWORK_NOT_JOINED) activated=false;
 return sendResult;
}
'''
run('scheduler', COMMON + FAKES + sketch + r'''
void advance(uint32_t ms) { uint32_t start=ticks; while(uint32_t(ticks-start)<ms) loop(); }
void reset() {
 ticks=0; activated=false; joinWorks=true; persistence=true; sendResult=0; limit=11;
 eligibleAt=0;forcedWait=0;waitAfterSend=0;eligibilityQueries=0;capacityChanges=0;capacityWorks=true;capacityImproves=true;joins=0;activationResult=-42;sends.clear();lengths.clear();
 gJoinHardwareFailures=0;gUplinkHardwareFailures=0;gCapacityFailures=0;gCapacityRatesTried=0;gRetryBackoffMs=5000; gUnavailableAttempts=0; gRecoveryAttempts=0; recoveries=0; recoveryWorks=true; gReportWaitMs=60000; setup();
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
 limit=11; advance(61000); assert(lengths.size()==2 && lengths[1]==11);
 reset(); eligibleAt=ticks+10000; advance(500); assert(sends.empty()); advance(10100); assert(sends.size()==1);
 reset(); sendResult=-42; advance(500); assert(sends.size()==1);
 advance(5500); assert(sends.size()==2); advance(10000); assert(sends.size()==3);
 assert(sends[2]-sends[1]>=10000);
 reset(); sendResult=RADIOLIB_ERR_INVALID_PORT; advance(1000); assert(gSchedulerState==SCHED_FAULT);
 advance(60000); assert(sends.size()==1);
 reset(); joinWorks=false; persistence=false; advance(500); assert(gSchedulerState==SCHED_FAULT && sends.empty());
 reset(); sendResult=RADIOLIB_ERR_NETWORK_NOT_JOINED; advance(200); sendResult=0; advance(1000); assert(joins==2);
 reset(); ticks=UINT32_MAX-32000; eligibleAt=ticks; gNextSchedulerTickMs=ticks; gLastJoinAttemptMs=ticks-60000;
 advance(500);assert(sends.empty() && gSchedulerState==SCHED_WAIT_TIMER_ROLLOVER);
 advance(93000);assert(sends.size()==1 && recoveries==1);
 // Persistent MAC responses continue at one-minute intervals until room returns.
 reset(); limit=10; advance(480000);
 assert(sends.size()==8 && gSchedulerState!=SCHED_FAULT);
 for(size_t i=0;i<lengths.size();++i) {
   assert(lengths[i]==0);
   if(i) assert(uint32_t(sends[i]-sends[i-1])>=60000);
 }
 limit=11;advance(61000);assert(lengths.back()==11 && recoveries==0);
 // A newly joined session gets the initial retry delay, even after a long fault.
 reset(); sendResult=-42;advance(400000);assert(gRetryBackoffMs==300000);
 activated=false;gSchedulerState=SCHED_WAIT_FOR_JOIN;gLastJoinAttemptMs=ticks-60000;
 assert(ensureJoined() && gRetryBackoffMs==5000);
 gSchedulerState=SCHED_SEND_UPLINK;loop();assert(gReportWaitMs==5000);
 // Unavailable with no airtime wait gets three retries, then a quiet cooldown.
 reset();sendResult=RADIOLIB_ERR_UPLINK_UNAVAILABLE;advance(12000);
 assert(sends.size()==3 && gSchedulerState==SCHED_RECOVER_RADIO && recoveries==0);
 uint32_t recoveryStart=gRecoveryStartedMs;
 advance(58000);assert(recoveries==0 && sends.size()==3);
 sendResult=0;advance(4000);
 assert(uint32_t(ticks-recoveryStart)>=60000 && recoveries==1 && joins==2 && sends.size()==4);
 // A real duty-cycle deferral does not consume the stuck-state counter.
 reset();sendResult=RADIOLIB_ERR_UPLINK_UNAVAILABLE;advance(1000);
 eligibleAt=ticks+10000;advance(6000);assert(gUnavailableAttempts==0 && recoveries==0);
 // Three failed recoveries stop; persistence errors stop on the first attempt.
 reset();recoveryWorks=false;scheduleRadioRecovery();advance(181000);
 assert(recoveries==3 && gSchedulerState==SCHED_FAULT);
 reset();recoveryWorks=false;persistence=false;scheduleRadioRecovery();advance(61000);
 assert(recoveries==1 && gSchedulerState==SCHED_FAULT);
 // Rollover adds a fresh cooldown after the boundary, then recovers once.
 reset();ticks=UINT32_MAX-30000;gNextSchedulerTickMs=ticks;scheduleRadioRecovery();
 advance(89000);assert(recoveries==0);advance(2000);assert(recoveries==1);

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
int16_t nonceRestoreResult=0;
int radioCalls=0, receiveResult=0;
struct LoRaWANEvent_t { uint32_t fCnt=0; float freq=0; uint8_t datarate=0; };
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
 size_t getMaxPayloadLen() { return 11; }
 int16_t sendReceive(const uint8_t*,size_t,uint8_t,uint8_t*,size_t* len,bool,LoRaWANEvent_t*,LoRaWANEvent_t*) {
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
 receiveResult=-42; assert(lwSendReceive(data,11,data,&len)==-42 && len==0);
 assert(lwSendReceive(nullptr,11,data,&len)==LW_ERR_INVALID_ARGUMENT);
 assert(lwSendReceive(data,11,nullptr,&len)==LW_ERR_INVALID_ARGUMENT);
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

# The build must accept pristine upstream files and reject the former patched
# files. Verification itself must not change the supplied dependency.
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

# Integrate the firmware with unchanged upstream methods and its actual nonce
# schema. Only the modem, cryptography/session setup, and flash I/O are faked.
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
recover = lw[lw.index('static bool lwRecover()'):lw.index('bool lwActivate()')]
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
struct LoRaWANEvent_t { uint32_t fCnt=0;float freq=0;uint8_t datarate=0; };
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
 uint8_t getMaxPayloadLen() { return 11; }
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
# Reuse the scheduler's error vocabulary, not its fake service functions.
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
bool lwBegin() {
 ++initializations;
 assert(!gNode.isActivated() && gNode.rxDelayStart==0 && gNode.rxDelayEnd==0);
 return initWorks;
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
 assert(lwRecover() && !lwIsActivated() && gNode.nextDevNonce()==48 && gNode.joinNonce==1);
 assert(gNode.rxDelayStart==0 && gNode.rxDelayEnd==0);
 assert(lwActivate() && gRadio.attempted.back()==48 && durableLimit()==64);
 // A bad reservation buffer must stop joining if uncertain-nonce restore fails.
 assert(lwRecover());LoRaWANNode::joinReceiveResult=0;
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
 assert(!lwRecover() && !gPersistenceHealthy && !lwIsActivated());
}
''')
run('stock_transmit_scheduler_recovery', stock_integration + sketch + r'''
void advance(uint32_t ms) { uint32_t start=ticks;while(uint32_t(ticks-start)<ms)loop(); }
int main() {
 mod.hal=&hal;gRadio.mod=&mod;setup();gRadio.uplinkResult=-42;
 advance(25000);
 assert(gSchedulerState==SCHED_RECOVER_RADIO && gRadio.uplinkCalls==1 && initializations==1);
 assert(gRadio.attempted.size()==1 && gRadio.attempted[0]==0);
 uint32_t recoveryStart=gRecoveryStartedMs;
 advance(50000);assert(initializations==1 && gRadio.uplinkCalls==1);
 gRadio.uplinkResult=0;advance(20000);
 assert(uint32_t(ticks-recoveryStart)>=60000);
 assert(initializations==2 && gRadio.uplinkCalls==2 && gSchedulerState!=SCHED_FAULT);
 assert(gRadio.attempted.size()==2 && gRadio.attempted[1]==16);
 assert(gRetryBackoffMs==5000 && gUnavailableAttempts==0);
 advance(61000);assert(gRadio.uplinkCalls==3);
}
''')

run('airtime_wait_validation_and_rechecks', COMMON + FAKES + sketch + r'''
void advance(uint32_t ms) { uint32_t start=ticks;while(uint32_t(ticks-start)<ms)loop(); }
int main() {
 setup();eligibleAt=ticks+180000;
 advance(125000);
 assert(sends.empty() && eligibilityQueries>=3 && recoveries==0 && gReportWaitMs<=60000);
 advance(60000);assert(sends.size()==1); // No RF before the real eligibility deadline.
 // Rechecks also notice when a long deferral is removed early.
 eligibleAt=ticks+3600000;gSchedulerState=SCHED_SEND_UPLINK;loop();
 assert(gReportWaitMs==60000);eligibleAt=ticks;advance(61000);assert(sends.size()==2);
 forcedWait=0xFFFFFFFFUL;gSchedulerState=SCHED_SEND_UPLINK;loop();
 assert(gSchedulerState==SCHED_RECOVER_RADIO && sends.size()==2);
 forcedWait=0;advance(59000);assert(recoveries==0);
 advance(2000);assert(recoveries==1);
 // Validate the result obtained after an UPLINK_UNAVAILABLE error too.
 sendResult=RADIOLIB_ERR_UPLINK_UNAVAILABLE;waitAfterSend=0x80000000UL;
 forcedWait=0;activated=true;gSchedulerState=SCHED_SEND_UPLINK;loop();
 assert(gSchedulerState==SCHED_RECOVER_RADIO);
 // Re-query scheduling also works at high uptime outside the rollover guard.
 forcedWait=0;waitAfterSend=0;sendResult=0;ticks=UINT32_MAX-1000000;
 gNextSchedulerTickMs=ticks;eligibleAt=ticks+180000;gSchedulerState=SCHED_SEND_UPLINK;
 size_t before=sends.size();advance(179000);assert(sends.size()==before);
 advance(2000);assert(sends.size()==before+1);
}
''')

run('join_hardware_failure_policy', COMMON + FAKES + sketch + r'''
void advance(uint32_t ms) { uint32_t start=ticks;while(uint32_t(ticks-start)<ms)loop(); }
int main() {
 setup();joinWorks=false;activationResult=RADIOLIB_ERR_SPI_CMD_TIMEOUT;
 advance(121000);assert(joins==3 && gSchedulerState==SCHED_RECOVER_RADIO && recoveries==0);
 advance(58000);assert(recoveries==0);
 joinWorks=true;advance(4000);assert(recoveries==1 && activated && gJoinHardwareFailures==0);
 // A no-JoinAccept result interrupts a streak of radio hardware errors.
 activated=false;joinWorks=false;gSchedulerState=SCHED_WAIT_FOR_JOIN;
 activationResult=RADIOLIB_ERR_TX_TIMEOUT;assert(!ensureJoined() && gJoinHardwareFailures==1);
 activationResult=RADIOLIB_ERR_NO_JOIN_ACCEPT;assert(!ensureJoined() && gJoinHardwareFailures==0);
 for(unsigned i=0;i<10;++i) { assert(!ensureJoined() && gSchedulerState==SCHED_WAIT_FOR_JOIN); }
 for(int16_t state : {RADIOLIB_ERR_TX_TIMEOUT,RADIOLIB_ERR_RX_TIMEOUT,
  RADIOLIB_ERR_SPI_WRITE_FAILED,RADIOLIB_ERR_SPI_CMD_TIMEOUT,RADIOLIB_ERR_SPI_CMD_INVALID,
  RADIOLIB_ERR_SPI_CMD_FAILED,RADIOLIB_ERR_CHIP_NOT_FOUND}) { assert(hardwareLoRaError(state)); }
 assert(!hardwareLoRaError(RADIOLIB_ERR_NO_JOIN_ACCEPT));
}
''')

run('capacity_retry_policy', COMMON + FAKES + sketch + r'''
void advance(uint32_t ms) { uint32_t start=ticks;while(uint32_t(ticks-start)<ms)loop(); }
int main() {
 setup();sendResult=RADIOLIB_ERR_PACKET_TOO_LONG;
 advance(121000);assert(sends.size()==3 && capacityChanges==1 && gSchedulerState!=SCHED_FAULT);
 advance(61000);assert(sends.size()==4 && lengths.back()==11 && gCapacityRatesTried==0);
 // A candidate that still fails is not selected again indefinitely.
 sendResult=RADIOLIB_ERR_DWELL_TIME_EXCEEDED;capacityImproves=false;
 advance(361000);assert(gSchedulerState==SCHED_FAULT);
 // Explicit no-capacity result pauses after the third failed transaction.
 gCapacityFailures=0;gCapacityRatesTried=0;capacityWorks=false;
 gSchedulerState=SCHED_SEND_UPLINK;gNextSchedulerTickMs=ticks;
 loop();advance(121000);assert(gSchedulerState==SCHED_FAULT);
}
''')

run('stock_join_hardware_recovery', stock_integration + sketch + r'''
void advance(uint32_t ms) { uint32_t start=ticks;while(uint32_t(ticks-start)<ms)loop(); }
int main() {
 mod.hal=&hal;gRadio.mod=&mod;setup();gRadio.joinResult=RADIOLIB_ERR_SPI_CMD_TIMEOUT;
 advance(121000);
 assert(gSchedulerState==SCHED_RECOVER_RADIO && gRadio.attempted.size()==3 && durableLimit()==48);
 advance(58000);assert(initializations==1);
 gRadio.joinResult=0;advance(12000);
 assert(initializations==2 && lwIsActivated() && gRadio.attempted.size()==4);
 assert(gRadio.attempted.back()==48 && durableLimit()==64 && gRadio.uplinkCalls==1);
}
''')

run('stock_rollover_wait_recovery', stock_integration + sketch + r'''
void advance(uint32_t ms) { uint32_t start=ticks;while(uint32_t(ticks-start)<ms)loop(); }
int main() {
 mod.hal=&hal;gRadio.mod=&mod;setup();advance(7000);
 assert(lwIsActivated() && gRadio.uplinkCalls==1);
 ticks=1000;gNode.rxDelayStart=UINT32_MAX-1000;gNode.rxDelayEnd=UINT32_MAX-500;
 gNode.dutyCycle=0;gNextSchedulerTickMs=ticks;gSchedulerState=SCHED_SEND_UPLINK;
 assert(lwTimeUntilUplink()>0x7FFFFFFFUL);loop();
 assert(gSchedulerState==SCHED_RECOVER_RADIO && gRadio.uplinkCalls==1);
 advance(59000);assert(initializations==1);
 advance(12000);assert(initializations==2 && lwIsActivated() && gRadio.uplinkCalls==2);
}
''')

# Use unchanged upstream payload arithmetic, validation and public setDatarate
# with the actual firmware capacity selector. Modem/MAC application is faked.
capacity_fixture = STOCK_FAKE.replace('struct LoRaWANChannel_t {};', r'''
struct LoRaWANChannel_t { bool enabled=false;uint8_t dr=0,drMin=0,drMax=1; };
''').replace('struct Band { uint8_t bandNum=1; };', r'''
struct Band {
 uint8_t bandNum=1,payloadLenMax[15]={11,53,125},dataRates[15]={};
 Band() { std::fill(dataRates+3,dataRates+15,255); }
};
''').replace('uint8_t getMaxPayloadLen() { return 11; }', r'''
 uint8_t fOptsUpLen=0;bool TS009=false,TS011=false,isMACPayload=false,phyRejectDataRate=false;
 LoRaWANChannel_t channelPlan[2][16];
 uint8_t getMaxPayloadLen();int16_t isValidUplink(uint8_t*,uint8_t);int16_t setDatarate(uint8_t);
 int16_t execMacCommand(uint8_t,uint8_t* options,uint8_t,uint8_t* ack) {
  *ack=phyRejectDataRate?0:2;
  if(*ack) { channels[0].dr=options[0]>>4; }
  return 0;
 }
''').replace('phyLayer(phy),band(region) {}', r'''
phyLayer(phy),band(region) { for(auto& channel:channelPlan[0]) { channel.enabled=true; } }
''').replace('  LoRaWANChannel_t channel;\n  int16_t result=', r'''
  uint8_t total=uint8_t(len)+fOptsUpLen;
  int16_t valid=isValidUplink(&total,1);if(valid)return valid;
  LoRaWANChannel_t channel;
  int16_t result=''')
capacity_defines = '\n'.join(line for line in radio_header.splitlines()
                            if line.startswith('#define RADIOLIB_LORAWAN_FPORT_') or
                            (line.startswith('#define ') and line.split()[1] in {
                                'RADIOLIB_LORAWAN_CHANNEL_NUM_DATARATES',
                                'RADIOLIB_LORAWAN_FHDR_FOPTS_MAX_LEN',
                                'RADIOLIB_LORAWAN_DATA_RATE_UNUSED',
                                'RADIOLIB_LORAWAN_NUM_AVAILABLE_CHANNELS',
                                'RADIOLIB_LORAWAN_MAC_LINK_ADR',
                            })) + '\n#define RADIOLIB_MIN(a,b) ((a)<(b)?(a):(b))\n'
capacity_methods = stock_methods + ''.join(radio_method(signature) for signature in (
    'uint8_t LoRaWANNode::getMaxPayloadLen()',
    'int16_t LoRaWANNode::isValidUplink(',
    'int16_t LoRaWANNode::setDatarate(',
))
capacity_selector = lw[lw.index('static bool lwTryCapacityDataRate('):lw.rindex('#endif')]
capacity_state = STOCK_STATE.replace('bool lwTryCapacityDataRate(size_t,uint16_t&) { return false; }', '')
capacity_integration = (COMMON + error_constants + schema + capacity_defines + capacity_fixture +
                        templates + capacity_methods + node_class + capacity_state + save + recover +
                        activate + wrapper + capacity_selector)
run('stock_payload_capacity_recovery', capacity_integration + sketch + r'''
void advance(uint32_t ms) { uint32_t start=ticks;while(uint32_t(ticks-start)<ms)loop(); }
int main() {
 mod.hal=&hal;gRadio.mod=&mod;setup();advance(7000);
 assert(lwIsActivated() && gRadio.uplinkCalls==1);
 gNode.fOptsUpLen=12;assert(lwMaxPayloadBytes()==255);
 advance(181000);
 assert(gNode.channels[0].dr==1 && gRadio.uplinkCalls==1 && initializations==1);
 advance(61000);
 assert(gRadio.uplinkCalls>=2 && gNode.fOptsUpLen==12 && gSchedulerState!=SCHED_FAULT);
 // Only the enabled uplink channel plan can authorize a candidate rate.
 uint16_t tried=0;
 for(auto& channel:gNode.channelPlan[0]) { channel.drMax=0; }
 assert(!lwTryCapacityDataRate(11,tried));uint16_t allTried=tried;
 assert(!lwTryCapacityDataRate(11,tried) && tried==allTried);
 // When all allowed PHY settings reject the change, pause rather than spin.
 for(auto& channel:gNode.channelPlan[0]) { channel.drMax=1; }
 gNode.channels[0].dr=0;gNode.phyRejectDataRate=true;
 gSchedulerState=SCHED_SEND_UPLINK;gCapacityFailures=0;gCapacityRatesTried=0;
 unsigned rfBefore=gRadio.uplinkCalls;advance(121000);
 assert(gSchedulerState==SCHED_FAULT && gRadio.uplinkCalls==rfBefore);
}
''')

run('stock_payload_capacity_boundary_units', capacity_integration + r'''
int main() {
 mod.hal=&hal;gRadio.mod=&mod;assert(lwActivate());
 gNode.fOptsUpLen=15;
 // These synthetic N-values straddle the bound discussed in PR #47.
 // They use the unchanged upstream setter, capacity and validation methods.
 for(uint8_t regionalN=25;regionalN<=39;++regionalN) {
  gRegion.payloadLenMax[1]=regionalN;uint16_t tried=0;
  bool selected=lwTryCapacityDataRate(11,tried);
  assert(selected==(regionalN>=26));
  if(selected) {
   assert(gNode.channels[0].dr==1 && gNode.getMaxPayloadLen()==regionalN-15);
   uint8_t total=11+15;assert(gNode.isValidUplink(&total,1)==RADIOLIB_ERR_NONE);
  }
 }
 gRegion.payloadLenMax[1]=26;uint16_t tried=0;
 assert(lwTryCapacityDataRate(11,tried) && gNode.getMaxPayloadLen()==11);
 uint8_t tooLong=12+15;
 assert(gNode.isValidUplink(&tooLong,1)==RADIOLIB_ERR_PACKET_TOO_LONG);
 assert(gNode.fOptsUpLen==15); // The firmware never alters the pending queue.
}
''')

# Exercise firmware hardware-error policy separately from the availability
# workaround: failures before PHY transmit never poison the RX timestamps.
run('uplink_hardware_failure_policy', COMMON + FAKES + sketch + r'''
void attempt(int16_t state) {
 sendResult=state;gSchedulerState=SCHED_SEND_UPLINK;gNextSchedulerTickMs=ticks;loop();
}
int main() {
 setup();activated=true;
 attempt(RADIOLIB_ERR_SPI_CMD_TIMEOUT);assert(gUplinkHardwareFailures==1);
 attempt(RADIOLIB_ERR_SPI_CMD_FAILED);assert(gUplinkHardwareFailures==2);
 attempt(RADIOLIB_ERR_RX_TIMEOUT);
 assert(gSchedulerState==SCHED_RECOVER_RADIO && recoveries==0);
 // Non-hardware failures and successful transactions interrupt the streak.
 activated=true;gSchedulerState=SCHED_WAIT_FOR_REPORT;
 attempt(RADIOLIB_ERR_TX_TIMEOUT);assert(gUplinkHardwareFailures==1);
 attempt(RADIOLIB_ERR_UNKNOWN);assert(gUplinkHardwareFailures==0 && gSchedulerState==SCHED_WAIT_FOR_REPORT);
 attempt(RADIOLIB_ERR_SPI_WRITE_FAILED);attempt(0);assert(gUplinkHardwareFailures==0);
 // Waiting for eligibility is not a failed transaction.
 attempt(RADIOLIB_ERR_CHIP_NOT_FOUND);forcedWait=60000;
 attempt(RADIOLIB_ERR_SPI_CMD_INVALID);assert(gUplinkHardwareFailures==1);
 forcedWait=0;assert(ensureJoined()); // Already joined; does not erase the streak.
 assert(gUplinkHardwareFailures==1);
 activated=false;assert(ensureJoined() && gUplinkHardwareFailures==0);
}
''')

hardware_fixture = STOCK_FAKE.replace(
    'int16_t setPhyProperties(LoRaWANChannel_t*,uint8_t,int) { return 0; }', r'''
 int16_t forcedPhyError=0;unsigned configFailures=0;
 int16_t setPhyProperties(LoRaWANChannel_t*,uint8_t,int) {
  if(forcedPhyError) { ++configFailures; }
  return forcedPhyError;
 }
''')
hardware_integration = (COMMON + error_constants + schema + hardware_fixture + templates +
                        stock_methods + node_class + STOCK_STATE + save + recover + activate + wrapper)
run('stock_uplink_spi_error_recovery', hardware_integration + sketch + r'''
void advance(uint32_t ms) { uint32_t start=ticks;while(uint32_t(ticks-start)<ms)loop(); }
int main() {
 mod.hal=&hal;gRadio.mod=&mod;setup();advance(7000);
 assert(lwIsActivated() && gRadio.uplinkCalls==1);
 uint32_t acceptedJoinNonce=gNode.joinNonce;uint16_t reserved=durableLimit();
 gNode.forcedPhyError=RADIOLIB_ERR_SPI_CMD_TIMEOUT;
 uint32_t started=ticks;
 while(gSchedulerState!=SCHED_RECOVER_RADIO && uint32_t(ticks-started)<120000) { loop(); }
 assert(gNode.configFailures==3 && initializations==1 && gRadio.uplinkCalls==1);
 assert(gSchedulerState==SCHED_RECOVER_RADIO);
 uint32_t recoveryStart=gRecoveryStartedMs;
 advance(58000);assert(initializations==1);
 // Node reconstruction clears the simulated fault. Restore skips the durable
 // nonce reservation, then obtain fresh keys/counters through OTAA before RF.
 advance(10000);
 assert(uint32_t(ticks-recoveryStart)>=60000 && initializations==2);
 assert(lwIsActivated() && gRadio.uplinkCalls==2 && gUplinkHardwareFailures==0);
 assert(gRadio.attempted.back()==reserved && gNode.joinNonce==acceptedJoinNonce+1);
 advance(61000);assert(gRadio.uplinkCalls==3);
}
''')

# Track all HAL delays and add realistic setup latency. Calls that started
# within 50 ms of the boundary used to request ~2^32 ms of internal sleep.
rollover_fixture = STOCK_FAKE.replace('struct Hal {', 'uint32_t maxHalDelay=0;\nstruct Hal {')
rollover_fixture = rollover_fixture.replace(
    'void delay(uint32_t ms) { ticks+=ms; }',
    'void delay(uint32_t ms) { assert(ms<60000);maxHalDelay=std::max(maxHalDelay,ms);ticks+=ms; }')
rollover_fixture = rollover_fixture.replace(
    'void createSession(uint8_t,uint8_t) { isActive=false; }', r'''
 uint32_t setupDelay=100,phyDelay=100;
 void createSession(uint8_t,uint8_t) { ticks+=setupDelay;isActive=false; }
''')
rollover_fixture = rollover_fixture.replace(
    'int16_t setPhyProperties(LoRaWANChannel_t*,uint8_t,int) { return 0; }',
    'int16_t setPhyProperties(LoRaWANChannel_t*,uint8_t,int) { ticks+=phyDelay;return 0; }')
rollover_integration = (COMMON + error_constants + schema + rollover_fixture + templates +
                        stock_methods + node_class + STOCK_STATE + save + recover + activate + wrapper)
run('stock_blocking_transaction_rollover_guard', rollover_integration + sketch + r'''
void advance(uint32_t ms) { uint32_t start=ticks;while(uint32_t(ticks-start)<ms)loop(); }
int main() {
 mod.hal=&hal;gRadio.mod=&mod;setup();
 const uint32_t boundary=UINT32_MAX-LW_TRANSACTION_GUARD_MS;
 ticks=boundary-1;assert(!lwTimerRolloverPending());
 ticks=boundary;assert(lwTimerRolloverPending());
 // Direct wrapper calls are guarded too, without consuming a nonce or doing RF.
 ticks=UINT32_MAX-50;
 assert(!lwActivate() && lwLastActivationState()==LW_ERR_TIMER_ROLLOVER);
 assert(gRadio.attempted.empty() && !haveDurable && maxHalDelay==0);
 uint8_t data[255]={};size_t down=255;
 assert(lwSendReceive(data,11,data,&down)==LW_ERR_TIMER_ROLLOVER && down==0);
 assert(gRadio.uplinkCalls==0 && maxHalDelay==0);
 // The join scheduler waits across wrap; a fresh node is initialized only
 // after the normal cooldown, then joins with the original durable history.
 gLastJoinAttemptMs=ticks-60000;gNextSchedulerTickMs=ticks;
 loop();assert(gSchedulerState==SCHED_WAIT_TIMER_ROLLOVER);
 advance(59000);assert(initializations==1 && gRadio.attempted.empty());
 advance(10000);assert(initializations==2 && lwIsActivated() && gRadio.uplinkCalls==1);
 assert(gRadio.attempted.back()==0);
 uint16_t reserved=durableLimit();uint32_t acceptedJoinNonce=gNode.joinNonce;
 // Do the same with a live session. No pre-wrap application or maintenance RF.
 ticks=boundary;gNextSchedulerTickMs=ticks;gSchedulerState=SCHED_SEND_UPLINK;
 loop();assert(gSchedulerState==SCHED_WAIT_TIMER_ROLLOVER && gRadio.uplinkCalls==1);
 advance(LW_TRANSACTION_GUARD_MS-1000);
 assert(initializations==2 && gRadio.uplinkCalls==1);
 advance(59000);assert(initializations==2); // Still within the post-wrap cooldown.
 advance(10000);
 assert(initializations==3 && lwIsActivated() && gRadio.uplinkCalls==2);
 assert(gRadio.attempted.back()==reserved && gNode.joinNonce==acceptedJoinNonce+1);
 assert(maxHalDelay<60000);
 // Detect a wrap missed while the scheduler was suspended, before consulting
 // stale library timestamps, and recover rather than waiting a whole cycle.
 gLastSchedulerTimeMs=UINT32_MAX-1000000;ticks=100;
 gNextSchedulerTickMs=ticks;gSchedulerState=SCHED_SEND_UPLINK;
 loop();assert(gSchedulerState==SCHED_RECOVER_RADIO && gRadio.uplinkCalls==2);
 advance(59000);assert(initializations==3);
 advance(10000);assert(initializations==4 && gRadio.uplinkCalls==3);
 // A wrapper return observed just after wrap must not wait another 49 days.
 ticks=100;gNextSchedulerTickMs=ticks;scheduleTimerRolloverWait();
 assert(gSchedulerState==SCHED_RECOVER_RADIO);
 // Timer maintenance never revives a permanent application fault.
 appFault(F("test"));ticks=boundary;gNextSchedulerTickMs=ticks;
 loop();assert(gSchedulerState==SCHED_FAULT);
}
''')

# Delayed flash I/O can enter the guard after the initial activation preflight.
late_guard_state = 'uint32_t flashDelay=100;\n' + STOCK_STATE.replace(
    'memcpy(durable,p,sizeof(durable));haveDurable=true;return true;',
    'ticks+=flashDelay;memcpy(durable,p,sizeof(durable));haveDurable=true;return true;')
late_guard_integration = (COMMON + error_constants + schema + rollover_fixture + templates +
                         stock_methods + node_class + late_guard_state + save + recover + activate + wrapper)
run('stock_rollover_guard_after_nonce_reservation', late_guard_integration + sketch + r'''
void advance(uint32_t ms) { uint32_t start=ticks;while(uint32_t(ticks-start)<ms)loop(); }
int main() {
 mod.hal=&hal;gRadio.mod=&mod;setup();ticks=UINT32_MAX-LW_TRANSACTION_GUARD_MS-50;
 assert(!lwActivate() && lwLastActivationState()==LW_ERR_TIMER_ROLLOVER);
 assert(gRadio.attempted.empty() && durableLimit()==16 && gNode.nextDevNonce()==0);
 // A durable reservation with no RF remains safe: recovery skips the batch.
 assert(!ensureJoined() && gSchedulerState==SCHED_WAIT_TIMER_ROLLOVER);
 gNextSchedulerTickMs=ticks;advance(LW_TRANSACTION_GUARD_MS+70000);
 assert(initializations==2 && lwIsActivated() && gRadio.attempted.back()==16);
 assert(gRadio.uplinkCalls==1 && maxHalDelay<60000);
 // Even a flash stall spanning the entire guard and wrap cannot authorize RF
 // with pre-wrap library state when the activation wrapper resumes.
 assert(lwRecover());size_t attempts=gRadio.attempted.size();
 ticks=UINT32_MAX-LW_TRANSACTION_GUARD_MS-50;flashDelay=LW_TRANSACTION_GUARD_MS+100;
 assert(!lwActivate() && lwLastActivationState()==LW_ERR_TIMER_ROLLOVER);
 assert(gRadio.attempted.size()==attempts && ticks<1000);
 scheduleTimerRolloverWait();assert(gSchedulerState==SCHED_RECOVER_RADIO);
}
''')

# Exercise the guard margin with 15 repetitions of the unchanged transmit
# method, maximum RX2 delay, simulated RF airtime and PHY setup latency.
budget_fixture = rollover_fixture.replace(
    '++uplinkCalls;return uplinkResult;', '++uplinkCalls;ticks+=1000;return uplinkResult;')
budget_fixture = budget_fixture.replace(r'''
  int16_t result=transmitUplink(&channel,const_cast<uint8_t*>(data),uint8_t(len),false);
  if(result==0) { ticks+=2000;rxDelayEnd=ticks; }
  return result;
''', r'''
  for(unsigned repetition=0;repetition<15;++repetition) {
   int16_t result=transmitUplink(&channel,const_cast<uint8_t*>(data),uint8_t(len),repetition>0);
   if(result!=0) { return result; }
   ticks+=16000;rxDelayEnd=ticks;
  }
  return 0;
''')
assert 'repetition<15' in budget_fixture
budget_integration = (COMMON + error_constants + schema + budget_fixture + templates +
                      stock_methods + node_class + STOCK_STATE + save + recover + activate + wrapper)
run('stock_rollover_guard_transaction_margin', budget_integration + sketch + r'''
int main() {
 mod.hal=&hal;gRadio.mod=&mod;setup();assert(lwActivate());
 const uint32_t boundary=UINT32_MAX-LW_TRANSACTION_GUARD_MS;
 ticks=boundary-1;gNextSchedulerTickMs=ticks;gSchedulerState=SCHED_SEND_UPLINK;
 loop();assert(gRadio.uplinkCalls==15 && ticks>boundary && ticks<UINT32_MAX);
 assert(maxHalDelay<60000 && uint32_t(ticks-(boundary-1))==256500);
 loop();assert(gSchedulerState==SCHED_WAIT_TIMER_ROLLOVER && gRadio.uplinkCalls==15);
}
''')

# Exercise real firmware startup/recovery and unchanged upstream radio init.
# The earlier scheduler fixtures stub lwBegin(), so they cannot check the
# NM180100 oscillator profile or RadioLib's default TCXO argument.
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
radio_begin = lw[lw.index('bool lwBegin()'):lw.index('bool lwIsActivated()')]
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
 resetRadio();assert(lwBegin());
 assert(gRadio.XTAL && tcxoCalls==0 && nodeBegins==1 && rfSwitchCalls==2);
 // Recovery must reapply the profile even if the flag was cleared beforehand.
 resetRadio();assert(lwRecover());
 assert(gRadio.XTAL && tcxoCalls==0 && nodeBegins==2 && restores==1 && constructions==2);
 unsigned previousBegins=nodeBegins,previousRestores=restores;
 resetRadio();chipFound=false;assert(!lwRecover());
 assert(gRadio.XTAL && tcxoCalls==0 && nodeBegins==previousBegins && restores==previousRestores);
 resetRadio();configResult=RADIOLIB_ERR_SPI_CMD_FAILED;assert(!lwRecover());
 assert(tcxoCalls==0 && nodeBegins==previousBegins && restores==previousRestores);
 // Fail the explicit RF-switch setup after the driver's own successful setup.
 resetRadio();failRfSwitchAt=2;assert(!lwRecover());
 assert(rfSwitchCalls==2 && nodeBegins==previousBegins && restores==previousRestores);
 resetRadio();nodeBeginResult=RADIOLIB_ERR_INVALID_MODE;assert(!lwRecover());
 assert(nodeBegins==previousBegins+1 && restores==previousRestores);
 resetRadio();assert(lwRecover() && tcxoCalls==0);
 assert(nodeBegins==previousBegins+2 && restores==previousRestores+1);
 resetRadio();restoreResult=LW_ERR_PERSISTENCE;assert(!lwRecover() && !gPersistenceHealthy);
 unsigned before=constructions;assert(!lwRecover() && constructions==before);
}
''')

run('serial_backpressure_and_fault_state', COMMON + FAKES + sketch + r'''
void advance(uint32_t ms) { uint32_t start=ticks;while(uint32_t(ticks-start)<ms)loop(); }
int main() {
 Serial.ready=false;setup();advance(185000);
 assert(joins==1 && sends.size()==4 && Serial.output.empty());
#if FW_LOG_LEVEL != FW_LOG_LEVEL_OFF
 for(unsigned i=0;i<20;++i) { LOG_INFO("Pressure", "Pending %u", i); }
 assert(logDroppedCount(LogLevel::INFO)>0);
#endif
 // Full/unwritable serial output must not delay faults or radio policy, and
 // critical messages must survive INFO pressure until the UART becomes ready.
 appFault("Injected failure");size_t before=sends.size();advance(31000);
 assert(gSchedulerState==SCHED_FAULT && sends.size()==before && Serial.output.empty());
 Serial.ready=true;advance(5000);
#if FW_LOG_LEVEL != FW_LOG_LEVEL_OFF
 assert(Serial.output.find("[ERROR][App] Injected failure\r\n")!=std::string::npos);
 assert(Serial.output.find("[ERROR][Fault] Radio work paused;")!=std::string::npos);
 assert(Serial.output.find("Dropped totals:")!=std::string::npos && sends.size()==before);
#else
 assert(Serial.output.empty() && sends.size()==before);
#endif
}
''')
