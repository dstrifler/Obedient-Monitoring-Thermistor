#!/usr/bin/env python3
"""Exercise real management/storage/scheduler/codec against deterministic hardware."""
from pathlib import Path
import subprocess
import tempfile
from host_support import prepare, compile_host

FIXTURE = r'''
#include "Arduino.h"
#include "EEPROM.h"
#include "storage_service.h"
#include "scheduler_service.h"
#include "encoder_service.h"
#include "bsec_service.h"
#include "settings_defaults.h"
#include <cmath>
#include <vector>
#include <thread>
#include <atomic>
#include <cstring>
#include <string>
uint32_t ticks=0;thread_local bool inWorker=false;SerialFake Serial;EEPROMFake EEPROM;
bool clockOkay=true,clockChecked=false,radioReady=false,joinOkay=true,sensorOkay=true,sensorStarted=false;
bool stopped=false,stopNever=false,nonceEraseFails=false;unsigned erases=0,reboots=0,stops=0;
unsigned nonceSaves=0;uint8_t nonceBytes[18]={};bool noncePresent=false;
uint32_t acquired=0;uint64_t fullTime=0;SensorSnapshot source{};
int txResult=0;size_t radioCapacity=53;uint32_t eligibility=0;
struct Packet {uint32_t time;std::vector<uint8_t> bytes;};std::vector<Packet> packets;
uint32_t millis(){return ticks;}uint32_t micros(){return ticks*1000;}
void acquire(){
 if(!sensorStarted || !sensorOkay || stopped)return;
 if(uint32_t(ticks-acquired)>=3000){
  acquired=ticks;source.sampleUptimeSeconds=uint32_t(fullTime/1000);
  inWorker=true;storageAppend(source,ticks);inWorker=false;
 }
}
void delay(uint32_t duration){
 uint32_t start=ticks;
 while(uint32_t(ticks-start)<duration){
  uint32_t step=std::min(uint32_t(50),duration-uint32_t(ticks-start));ticks+=step;fullTime+=step;acquire();
 }
}
void NVIC_SystemReset(){++reboots;}
bool clockBegin(){clockChecked=true;return clockOkay;}
void bsecStart(){sensorStarted=true;acquired=ticks-3000;acquire();}
void bsecPoll(){acquire();}
bool bsecStop(){++stops;if(stopNever)return false;stopped=stops>=2;return stopped;}
bool bsecPresent(){return sensorOkay;}
bool bsecLatest(SensorSnapshot* out){return out&&storageLatestSnapshot(out);}
bool nonceStoreBegin(){return true;}
bool nonceStoreSave(const uint8_t* bytes){memcpy(nonceBytes,bytes,18);noncePresent=true;++nonceSaves;return true;}
bool nonceStoreLoad(uint8_t* bytes,size_t n){if(!noncePresent||n<18)return false;memcpy(bytes,nonceBytes,18);return true;}
bool nonceStoreErase(){assert(stopped);++erases;if(nonceEraseFails)return false;memset(nonceBytes,255,18);noncePresent=false;return true;}
RadioState fakeRadioState=RadioState::Stopped;
void lwServiceStart(){assert(clockChecked&&clockOkay&&EEPROM.inits==1);fakeRadioState=RadioState::Initializing;}
void lwServicePoll(){if(fakeRadioState!=RadioState::Stopped)fakeRadioState=joinOkay?RadioState::Ready:RadioState::Joining;radioReady=fakeRadioState==RadioState::Ready;}
void lwServiceStop(){fakeRadioState=RadioState::Stopped;radioReady=false;}
RadioState lwServiceState(){return fakeRadioState;}int16_t lwServiceError(){return txResult;}
bool lwServiceReady(){return radioReady;}
uint32_t lwServiceWait(){return int32_t(eligibility-ticks)>0?eligibility-ticks:0;}
uint32_t lwGenerateResetCode(){return 654321;}
RadioTransaction lwTransmit(const uint8_t* bytes,size_t n){
 RadioTransaction result={};result.result=txResult;result.macOnly=n>radioCapacity;
 result.applicationSent=txResult>=0&&!result.macOnly;
 packets.push_back({ticks,result.macOnly?std::vector<uint8_t>():std::vector<uint8_t>(bytes,bytes+n)});
 delay(2100);return result;
}
#include "app_service.cpp"
void normal(){source={};source.environment={20,50,99000,100000,true};source.airQuality={50,500,0.5f,7,0,false,false};}
void advance(uint32_t duration){const uint32_t start=ticks;while(uint32_t(ticks-start)<duration)appLoop();}
size_t sensors(){size_t n=0;for(const auto& p:packets)if(p.bytes.size()==25)++n;return n;}
size_t controls(){size_t n=0;for(const auto& p:packets)if(!p.bytes.empty()&&p.bytes[0]==0xA1)++n;return n;}
void command(std::initializer_list<uint8_t> data){std::vector<uint8_t> bytes(data);receiveCommand(bytes.data(),bytes.size());}
void warmed(){appSetup();advance(363000);assert(sensors()==1&&schedulerFirstSensorSent());}
void pendingReset(){command({193,5,0,50});assert(resetPending&&resetCode==654321);}
void confirmReset(){command({193,6,0,51,0,9,251,241});}
int main(int argc,char** argv){
 assert(argc==2);std::string test=argv[1];normal();
 if(test=="startup"){
  appSetup();advance(359000);assert(sensors()==0&&storageCount()>=119);
  advance(5000);assert(sensors()==1&&schedulerFirstSensorSent()&&schedulerLearningComplete());
  assert(packets.back().bytes[19]%4==0); // IAQ accuracy zero is accepted.
 }else if(test=="startup_missing"){
  source.airQuality.validMask=0;appSetup();advance(725000);
  assert(sensors()==0&&controls()==2&&!schedulerFirstSensorSent());
  source.airQuality.validMask=7;advance(61000);assert(sensors()==1&&schedulerFirstSensorSent());
 }else if(test=="fixed_missing"){
  AppSettings s=settingsDefaults();s.mode=ReportMode::Fixed;s.intervalMinutes=2;
  assert(storageSaveSettings(s));sensorOkay=false;appSetup();advance(365000);
  assert(sensors()==0&&controls()==3&&schedulerIntervalMs()==120000);
 }else if(test=="fixed_cadence"){
  AppSettings s=settingsDefaults();s.mode=ReportMode::Fixed;s.intervalMinutes=1;assert(storageSaveSettings(s));
  warmed();advance(190000);assert(sensors()==4);
  for(size_t i=1;i<packets.size();++i)assert(uint32_t(packets[i].time-packets[i-1].time)>=60000);
 }else if(test=="fixed_failure_recovery"){
  AppSettings s=settingsDefaults();s.mode=ReportMode::Fixed;s.intervalMinutes=2;assert(storageSaveSettings(s));
  warmed();sensorOkay=false;uint32_t due=packets.back().time+120000;
  advance(uint32_t(due-ticks)-100);assert(controls()==2);advance(500);assert(controls()==3);
  const size_t sent=sensors();sensorOkay=true;due=packets.back().time+120000;
  advance(uint32_t(due-ticks)-100);assert(sensors()==sent);advance(500);assert(sensors()==sent+1);
 }else if(test=="steady_and_ring"){
  warmed();advance(3601000);assert(storageCount()<=1200&&storageCount()>=1198);
  assert(schedulerIntervalMs()==3600000&&sensors()==2);
  StoredReading a{},b{};assert(storageRead(0,&a)&&storageRead(1190,&b));
  assert(uint32_t(ticks-b.acquiredMs)<3600000&&a.acquiredMs!=b.acquiredMs);
 }else if(test=="isolated_outlier"){
  warmed();advance(1800000);const uint32_t before=schedulerIntervalMs();
  source.environment.temperatureC=70;advance(3000);source.environment.temperatureC=20;advance(30000);
  assert(schedulerIntervalMs()+30000>=before);
 }else if(test=="sustained_change"){
  warmed();advance(1800000);const uint32_t before=schedulerIntervalMs();
  for(unsigned i=0;i<100;++i){source.environment.temperatureC=20+float(i)*0.05f;advance(3000);}
  assert(schedulerIntervalMs()<before/2&&schedulerIntervalMs()>=60000);
 }else if(test=="partial_and_total_failure"){
  warmed();source.airQuality.validMask=0;advance(500000);assert(sensors()>=2);
  assert((packets.back().bytes[19]&0x70)==0);
  sensorOkay=false;const size_t before=controls();advance(725000);assert(controls()>=before+2&&schedulerIntervalMs()==360000);
 }else if(test=="batch_and_fresh_interval"){
  warmed();const uint32_t accepted=ticks;
  command({193,1,0,1,3,1,1,2,2,3,0});
  assert(appSettings().mode==ReportMode::Fixed&&appSettings().intervalMinutes==2&&appSettings().temperature==TemperatureUnit::Celsius);
  assert(!schedulerFirstSensorSent()||schedulerLearningComplete());
  advance(119000);assert(sensors()==1&&controls()==1);advance(4000);assert(sensors()==2);
  assert(uint32_t(packets.back().time-accepted)>=120000);
 }else if(test=="batch_rejection_and_dedup"){
  warmed();const auto old=appSettings();unsigned writes=EEPROM.puts;
  command({193,1,0,1,2,3,0,2,0});assert(settingsEqual(old,appSettings())&&EEPROM.puts==writes);
  EEPROM.failWrite=true;command({193,1,0,2,1,3,0});assert(settingsEqual(old,appSettings()));
  EEPROM.failWrite=false;command({193,1,0,3,1,3,0});assert(appSettings().temperature==TemperatureUnit::Celsius);
  writes=EEPROM.puts;command({193,1,0,3,1,3,0});assert(EEPROM.puts==writes);
  command({193,1,0,3,1,3,1});assert(appSettings().temperature==TemperatureUnit::Celsius);
 }else if(test=="capacity_and_failures"){
  radioCapacity=0;appSetup();advance(490000);assert(sensors()==0&&!schedulerFirstSensorSent());
  radioCapacity=53;advance(61000);assert(sensors()==1);
  txResult=-42;command({193,3,0,5});advance(500000);
  for(size_t i=1;i<packets.size();++i)assert(uint32_t(packets[i].time-packets[i-1].time)>=60000);
 }else if(test=="reset_expiry_and_no_polling"){
  warmed();AppSettings s=appSettings();s.mode=ReportMode::Fixed;s.intervalMinutes=60;
  assert(storageSaveSettings(s));const auto old=active;active=s;schedulerSettingsChanged(old,active);
  pendingReset();const uint32_t began=resetStarted;advance(120000);
  assert(resetPending&&controls()==1&&sensors()==1); // Challenge only, no reset polling.
  command({193,5,0,50});assert(resetStarted==began); // Duplicate does not renew deadline.
  command({193,6,0,51,0,1,134,160});assert(resetPending&&erases==0); // Wrong code.
  advance(3600000);assert(!resetPending&&reboots==0&&erases==0);
  confirmReset();assert(erases==0&&reboots==0);
 }else if(test=="confirmed_reset"){
  warmed();uint8_t cal[238]={1};assert(storageSaveCalibration(cal,238,0x26103304));
  uint8_t nonces[18]={1};assert(storageNonceSave(nonces));pendingReset();confirmReset();advance(500);
  assert(reboots==1&&erases==1&&stopped&&storageCount()==0);
  for(uint8_t value:EEPROM.bytes)assert(value==255);
  assert(!noncePresent&&!resetPending);
 }else if(test=="erase_failure"){
  warmed();pendingReset();nonceEraseFails=true;confirmReset();advance(5000);
  assert(erases==3&&reboots==0&&lwServiceState()==RadioState::Stopped);
  assert(Serial.output.find("erase verification failed")!=std::string::npos);
 }else if(test=="stop_failure"){
  warmed();pendingReset();stopNever=true;confirmReset();advance(11000);
  assert(erases==0&&reboots==0&&lwServiceState()==RadioState::Stopped);
 }else if(test=="settings_and_calibration"){
  assert(storageBegin());AppSettings s=settingsDefaults();s.intervalMinutes=59;
  uint8_t cal[238]={7},out[238]={};assert(storageSaveCalibration(cal,238,0x26103304));
  assert(storageSaveSettings(s)&&storageLoadCalibration(out,238,0x26103304)==238&&out[0]==7);
  AppSettings loaded{};assert(storageLoadSettings(&loaded)&&settingsEqual(loaded,s));
  EEPROM.bytes[35]^=1;assert(!storageLoadSettings(&loaded));
  assert(!storageLoadCalibration(out,238,123));
 }else if(test=="legacy_settings"){
  uint8_t record[15]={};const uint32_t magic=0x47534150;
  for(unsigned i=0;i<4;++i)record[i]=uint8_t(magic>>(8*i));
  record[4]=2;for(unsigned i=5;i<=8;++i)record[i]=1;
  record[9]=0;record[10]=2;record[11]=0;record[12]=15;
  uint16_t sum=0;for(unsigned i=0;i<13;++i)sum+=record[i];record[13]=sum;record[14]=sum>>8;
  memcpy(EEPROM.bytes+32,record,15);appSetup();
  assert(appSettings().temperature==TemperatureUnit::Celsius&&appSettings().pressure==PressureUnit::Pa);
  assert(appSettings().mode==ReportMode::Dynamic&&appSettings().intervalMinutes==15&&EEPROM.puts==0);
 }else if(test=="concurrency"){
  assert(storageBegin());ticks=100000;
  std::atomic<bool> done(false);std::atomic<unsigned> commits(0);
  std::thread writer([&](){inWorker=true;for(unsigned i=0;i<20000;++i){
   SensorSnapshot s=source;s.environment.gasOhms=i+1;s.airQuality.co2EquivalentPpm=float(i+1);
   if(storageAppend(s,91000+(i%9000)))++commits;
  }done.store(true);});
  while(!done.load()) {StoredReading r{};if(storageRead(0,&r))assert(r.sample.environment.gasOhms==uint32_t(r.sample.airQuality.co2EquivalentPpm));}
  writer.join();assert(commits>19000&&storageCount()<=1200);
 }else if(test=="timer_wrap"){
  ticks=UINT32_MAX-500000;eligibility=ticks;fullTime=ticks;warmed();advance(400000);
  assert(schedulerLearningComplete()&&sensors()>=1&&storageCount()>200);
 }else if(test=="clock_fault_and_uart"){
  clockOkay=false;appSetup();advance(1000);assert(packets.empty()&&!sensorStarted);
 }else if(test=="uart_backpressure"){
  Serial.ready=false;warmed();advance(200000);assert(storageCount()>180&&sensors()>=1);
 }else assert(false);
}
'''
CASES = ['startup', 'startup_missing', 'fixed_missing', 'fixed_cadence', 'fixed_failure_recovery', 'steady_and_ring',
         'isolated_outlier', 'sustained_change', 'partial_and_total_failure', 'batch_and_fresh_interval',
         'batch_rejection_and_dedup', 'capacity_and_failures', 'reset_expiry_and_no_polling',
         'confirmed_reset', 'erase_failure', 'stop_failure', 'settings_and_calibration',
         'legacy_settings', 'concurrency', 'timer_wrap', 'clock_fault_and_uart', 'uart_backpressure']
with tempfile.TemporaryDirectory(prefix='monitor-system-') as temp:
    folder=Path(temp);prepare(folder);(folder/'fixture.cpp').write_text(FIXTURE)
    exe=compile_host(folder,['fixture.cpp','storage_service.cpp','eeprom_service.cpp','bsec_state.cpp',
                             'app_settings.cpp','sensor_types.cpp','sensor_service.cpp','scheduler_service.cpp',
                             'encoder_service.cpp','log_service.cpp'],'system')
    for case in CASES:
        subprocess.run([str(exe),case],check=True)
        print('PASS:',case,flush=True)
