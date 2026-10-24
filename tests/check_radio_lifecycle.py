#!/usr/bin/env python3
"""Exercise the real radio lifecycle with deterministic low-level outcomes."""
from pathlib import Path
import subprocess
import tempfile
from host_support import ROOT, prepare, compile_host
transport=(ROOT/'lorawan_service.cpp').read_text()
timer=transport.split('// Private LoRa rollover timing.\n',1)[1].split('// LoRa transport and lifecycle.\n',1)[0]
source=transport.split('// Radio lifecycle is kept separate',1)[1]
CPP=r'''
#include "Arduino.h"
#include "lorawan_service.h"
#include "log_service.h"
#include <cstring>
#include <string>
uint32_t ticks=0;thread_local bool inWorker=false;SerialFake Serial;
uint32_t millis(){return ticks;}uint32_t micros(){return ticks*1000;}void delay(uint32_t n){ticks+=n;}
bool beginWorks=true,joinWorks=true,recoveryWorks=true,persistence=true,activeRadio=false;
int16_t restored=0,activation=-1116,transmission=0;unsigned begins=0,joins=0,recoveries=0,txs=0;
size_t capacity=53,lastBytes=0;uint32_t wait=0;bool capacityRestores=false;
bool lwBegin(){++begins;return beginWorks;}bool lwIsActivated(){return activeRadio;}
int16_t lwRestore(){return restored;}bool lwPersistenceHealthy(){return persistence;}
bool lwRecover(){++recoveries;activeRadio=false;return recoveryWorks;}
bool lwActivate(){++joins;activeRadio=joinWorks;return joinWorks;}
int16_t lwLastActivationState(){return activation;}
size_t lwMaxPayloadBytes(){return capacity;}uint32_t lwTimeUntilUplink(){return wait;}
bool lwTryCapacityDataRate(size_t,uint16_t& tried){if(tried)return false;tried=1;return capacityRestores;}
int16_t lwSendReceive(const uint8_t*,size_t n,uint8_t*,size_t* size){++txs;lastBytes=n;*size=0;return transmission;}
struct Entropy {uint8_t randomByte(){return 255;}}gRadio;
'''
MAIN=r'''
void start(){lwServiceStart();lwServicePoll();lwServicePoll();}
int main(int argc,char** argv){
 assert(argc==2);std::string test=argv[1];uint8_t data[25]={};
 if(test=="join_without_settling"){
  start();assert(lwServiceReady()&&begins==1&&joins==1&&ticks==0);
  assert(lwTransmit(data,25).applicationSent&&txs==1);
 }else if(test=="initialization_failure"){
  beginWorks=false;start();ticks+=1000;lwServicePoll();ticks+=2000;lwServicePoll();
  assert(begins==3&&joins==0&&lwServiceState()==RadioState::Fault);
 }else if(test=="persistence_failure"){
  restored=LW_ERR_PERSISTENCE;start();assert(lwServiceState()==RadioState::Fault&&joins==0);
 }else if(test=="network_retry"){
  joinWorks=false;start();assert(joins==1);ticks=59999;lwServicePoll();assert(joins==1);
  ticks=60000;lwServicePoll();assert(joins==2&&recoveries==0);
 }else if(test=="hardware_recovery"){
  start();transmission=RADIOLIB_ERR_TX_TIMEOUT;
  for(unsigned i=0;i<3;++i)lwTransmit(data,25);
  assert(lwServiceState()==RadioState::Cooldown);ticks=59999;lwServicePoll();assert(recoveries==0);
  ticks=60000;lwServicePoll();assert(recoveries==1);lwServicePoll();assert(lwServiceReady());
 }else if(test=="mixed_errors"){
  start();transmission=RADIOLIB_ERR_TX_TIMEOUT;lwTransmit(data,25);lwTransmit(data,25);
  transmission=RADIOLIB_ERR_UPLINK_UNAVAILABLE;lwTransmit(data,25);
  transmission=RADIOLIB_ERR_TX_TIMEOUT;lwTransmit(data,25);
  assert(lwServiceState()==RadioState::Ready);
 }else if(test=="bounded_recovery"){
  start();transmission=RADIOLIB_ERR_SPI_CMD_TIMEOUT;for(unsigned i=0;i<3;++i)lwTransmit(data,25);
  recoveryWorks=false;for(unsigned i=0;i<3;++i){ticks+=60000;lwServicePoll();}
  assert(recoveries==3&&lwServiceState()==RadioState::Fault);
 }else if(test=="capacity_and_mac"){
  start();capacity=0;auto t=lwTransmit(data,25);assert(t.macOnly&&!t.applicationSent&&lastBytes==0);
  capacity=53;transmission=RADIOLIB_ERR_PACKET_TOO_LONG;
  for(unsigned i=0;i<3;++i)lwTransmit(data,25);
  assert(lwServiceState()==RadioState::Fault);
 }else if(test=="rollover"){
  start();ticks=UINT32_MAX-1000;lwServicePoll();assert(lwServiceState()==RadioState::Rollover);
  ticks=10;lwServicePoll();assert(lwServiceState()==RadioState::Cooldown);
  ticks+=60000;lwServicePoll();assert(recoveries==1);lwServicePoll();assert(lwServiceReady());
 }else if(test=="eligibility_and_stop"){
  start();wait=180000;assert(lwServiceWait()==180000&&txs==0);
  wait=0x80000000;assert(lwServiceWait()==UINT32_MAX&&lwServiceState()==RadioState::Cooldown);
  lwServiceStop();assert(!lwTransmit(data,25).applicationSent&&txs==0);
 }else if(test=="entropy"){
  start();const auto code=lwGenerateResetCode();assert(code>=100000&&code<=999999);
 }else assert(false);
}
'''
with tempfile.TemporaryDirectory(prefix='radio-lifecycle-') as temp:
    folder=Path(temp);prepare(folder);(folder/'fixture.cpp').write_text(CPP+timer+'// Radio lifecycle is kept separate'+source+MAIN)
    exe=compile_host(folder,['fixture.cpp','log_service.cpp'],'radio')
    for case in ['join_without_settling','initialization_failure','persistence_failure','network_retry',
                 'hardware_recovery','mixed_errors','bounded_recovery','capacity_and_mac','rollover','eligibility_and_stop','entropy']:
        subprocess.run([str(exe),case],check=True)
        print('PASS:',case,flush=True)
