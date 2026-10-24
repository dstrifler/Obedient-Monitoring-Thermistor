#!/usr/bin/env python3
"""Exercise real management/storage/scheduler/codec against deterministic hardware."""
from pathlib import Path
import subprocess
import tempfile
from host_support import ROOT, prepare, compile_host

FIXTURE = r'''
#include "Arduino.h"
#include "EEPROM.h"
#include "storage_service.h"
#include "scheduler_service.h"
#include "encoder_service.h"
#include "sensor_service.h"
#include "FlashIAPBlockDevice.h"
#include "settings_defaults.h"
#include <cmath>
#include <vector>
#include <thread>
#include <atomic>
#include <cstring>
#include <string>
uint32_t ticks=0;thread_local bool inWorker=false;SerialFake Serial;EEPROMFake EEPROM;
bool clockOkay=true,clockChecked=false,radioReady=false,joinOkay=true,sensorOkay=true,sensorStarted=false;
bool stopped=false,stopNever=false;unsigned reboots=0,stops=0;
uint32_t acquired=0;uint64_t fullTime=0;SensorSnapshot source{};
int txResult=0;size_t radioCapacity=53;uint32_t eligibility=0;
uint32_t txDelay=2100;bool injectStatus=false;
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
void sensorStart(){sensorStarted=true;acquired=ticks-3000;acquire();}
void sensorPoll(const AppSettings&){acquire();}
bool sensorStop(){++stops;if(stopNever)return false;stopped=stops>=2;return stopped;}
bool sensorPresent(){return sensorOkay;}
bool sensorLatestSnapshot(SensorSnapshot* out){return out&&storageLatestSnapshot(out);}
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
 delay(txDelay);
 if(injectStatus){
  const uint8_t request[]={193,3,0,100};
  memcpy(result.downlink,request,sizeof(request));result.downlinkSize=sizeof(request);
  result.result=1;injectStatus=false;
 }
 return result;
}
#include "core_service.h"
#include "lorawan_service.h"
#include "log_service.h"
#include "config.h"
#include <mbed.h>
#include "core_management.inc"
void normal(){source={};source.environment={20,50,99000,100000,true};source.airQuality={50,500,0.5f,7,0,false,false};}
void advance(uint32_t duration){const uint32_t start=ticks;while(uint32_t(ticks-start)<duration)appLoop();}
size_t sensors(){size_t n=0;for(const auto& p:packets)if(p.bytes.size()==25)++n;return n;}
size_t controls(){size_t n=0;for(const auto& p:packets)if(!p.bytes.empty()&&p.bytes[0]==0xA1)++n;return n;}
void command(std::initializer_list<uint8_t> data){std::vector<uint8_t> bytes(data);receiveCommand(bytes.data(),bytes.size());}
void warmed(){appSetup();advance(363000);assert(sensors()==1&&schedulerFirstSensorSent());}
void pendingReset(){command({193,5,0,50});assert(resetPending&&resetCode==654321);}
void confirmReset(){command({193,6,0,51,0,9,251,241});}
void pendingReboot(){command({193,7,0,60});assert(resetPending&&resetAction==ResetAction::Reboot&&resetCode==654321);}
void confirmReboot(){command({193,8,0,61,0,9,251,241});}
int main(int argc,char** argv){
 assert(argc==2);std::string test=argv[1];normal();flashState().acquisitionStopped=&stopped;
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
 }else if(test=="availability_recovery"){
  warmed();source.airQuality.validMask=3;advance(4000000);
  assert(schedulerIntervalMs()==SCHEDULER_MAX_MS);
  const size_t before=sensors();source.airQuality.validMask=7;
  advance(5000);assert(schedulerIntervalMs()==SCHEDULER_MAX_MS); // Fewer than three matching readings.
  advance(6000);assert(schedulerIntervalMs()<=SCHEDULER_BASE_MS || sensors()>before);
  advance(SCHEDULER_BASE_MS+5000);assert(sensors()>before && (packets.back().bytes[19]&0x40));
  advance(90000);assert(schedulerIntervalMs()>SCHEDULER_BASE_MS); // Delivered event does not remain pending.
 }else if(test=="availability_loss_during_rf"){
  warmed();advance(4000000);assert(schedulerIntervalMs()==SCHEDULER_MAX_MS);
  const size_t before=sensors();source.airQuality.validMask=3;
  delay(180000); // Worker commits continue while main is blocked by RF work.
  advance(5000);assert(schedulerIntervalMs()<=SCHEDULER_BASE_MS || sensors()>before);
  advance(SCHEDULER_BASE_MS+5000);
  assert(sensors()>before && (packets.back().bytes[19]&0x40)==0);
 }else if(test=="transient_availability"){
  warmed();advance(4000000);const size_t before=sensors();
  source.airQuality.validMask=3;advance(3000);source.airQuality.validMask=7;advance(45000);
  assert(schedulerIntervalMs()==SCHEDULER_MAX_MS && sensors()==before);
 }else if(test=="fixed_partial_availability"){
  AppSettings s=settingsDefaults();s.mode=ReportMode::Fixed;s.intervalMinutes=2;assert(storageSaveSettings(s));
  warmed();const size_t before=sensors();const uint32_t due=packets.back().time+120000;
  source.airQuality.validMask=3;advance(15000);source.airQuality.validMask=7;
  advance(uint32_t(due-ticks)-100);assert(sensors()==before && schedulerIntervalMs()==120000);
  advance(500);assert(sensors()==before+1);
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
  command({193,6,0,51,0,1,134,160});assert(resetPending&&flashState().erases==0); // Wrong code.
  advance(3600000);assert(resetPending&&reboots==0&&flashState().erases==0);
  advance(3600000);assert(!resetPending&&reboots==0&&flashState().erases==0);
  confirmReset();assert(flashState().erases==0&&reboots==0);
 }else if(test=="factory_confirmation_after_one_hour"){
  warmed();pendingReset();delay(5400000); // A confirmation at 90 minutes is still valid.
  confirmReset();advance(500);
  assert(reboots==1&&flashState().erases==2&&stopped);
 }else if(test=="reboot_preserves_persistence"){
  AppSettings saved=settingsDefaults();saved.intervalMinutes=60;
  saved.temperature=TemperatureUnit::Celsius;assert(storageSaveSettings(saved));warmed();
  uint8_t cal[238]={7},nonces[18]={1};
  assert(storageSaveCalibration(cal,238,0x26103304));assert(storageNonceBegin()&&storageNonceSave(nonces));
  const std::vector<uint8_t> eepromBefore(EEPROM.bytes,EEPROM.bytes+sizeof(EEPROM.bytes));
  const std::vector<uint8_t> journalBefore(flashState().bytes,flashState().bytes+sizeof(flashState().bytes));
  const unsigned writes=EEPROM.puts;pendingReboot();
  assert(nodeStatus().managerState==65&&replies[replyHead].kind==ReplyKind::RebootChallenge);
  delay(5400000);confirmReboot();assert(lwServiceState()==RadioState::Stopped&&reboots==0);
  advance(500);assert(reboots==1&&stopped&&stops==2&&flashState().erases==0);
  assert(EEPROM.puts==writes&&std::equal(eepromBefore.begin(),eepromBefore.end(),EEPROM.bytes));
  assert(std::equal(journalBefore.begin(),journalBefore.end(),flashState().bytes));
  AppSettings loaded{};uint8_t calAfter[238]={},noncesAfter[18]={};
  assert(storageLoadSettings(&loaded)&&settingsEqual(loaded,saved));
  assert(storageLoadCalibration(calAfter,238,0x26103304)==238&&!memcmp(cal,calAfter,238));
  assert(storageNonceLoad(noncesAfter,18)&&!memcmp(nonces,noncesAfter,18));
 }else if(test=="confirmation_operation_binding"){
  warmed();pendingReboot();const uint32_t began=resetStarted;
  command({193,5,0,70});assert(replies[(replyHead+replyCount-1)%QUEUE_SIZE].result==CommandResult::Busy);
  command({193,7,0,60});assert(resetStarted==began); // Cached request cannot renew the deadline.
  confirmReset();assert(replies[(replyHead+replyCount-1)%QUEUE_SIZE].result==CommandResult::Conflict);
  command({193,8,0,71,0,1,134,160});
  assert(replies[(replyHead+replyCount-1)%QUEUE_SIZE].result==CommandResult::WrongCode);
  assert(resetPending&&state==ManagerState::Running&&flashState().erases==0&&reboots==0);
  confirmReboot();advance(500);assert(reboots==1&&flashState().erases==0);
 }else if(test=="factory_rejects_reboot_confirmation"){
  warmed();pendingReset();confirmReboot();
  assert(replies[(replyHead+replyCount-1)%QUEUE_SIZE].result==CommandResult::Conflict);
  command({193,7,0,70});assert(replies[(replyHead+replyCount-1)%QUEUE_SIZE].result==CommandResult::Busy);
  assert(resetPending&&state==ManagerState::Running&&flashState().erases==0&&reboots==0);
  confirmReset();advance(500);assert(reboots==1&&flashState().erases==2);
 }else if(test=="reboot_expiry_and_no_polling"){
  warmed();AppSettings saved=appSettings();saved.mode=ReportMode::Fixed;saved.intervalMinutes=60;
  assert(storageSaveSettings(saved));const auto before=active;active=saved;schedulerSettingsChanged(before,active);
  pendingReboot();const uint32_t began=resetStarted;
  advance(120000);assert(resetPending&&controls()==1&&sensors()==1);
  advance(3600000);assert(resetPending&&sensors()==2&&flashState().erases==0&&reboots==0);
  assert(resetStarted==began);advance(3600000);
  assert(!resetPending&&reboots==0&&flashState().erases==0&&state==ManagerState::Running);
  command({193,7,0,60});
  assert(replies[(replyHead+replyCount-1)%QUEUE_SIZE].result==CommandResult::Expired);
  confirmReboot();assert(replies[(replyHead+replyCount-1)%QUEUE_SIZE].result==CommandResult::Expired);
  assert(reboots==0&&flashState().erases==0);
 }else if(test=="challenge_expiry_boundary" || test=="challenge_expiry_wrap"){
  if(test=="challenge_expiry_wrap"){ticks=UINT32_MAX-60000;eligibility=ticks;fullTime=ticks;}
  appSetup();pendingReboot();delay(7200000-1);expireReset();
  assert(resetPending&&replies[replyHead].kind==ReplyKind::RebootChallenge);
  delay(1);expireReset();assert(!resetPending&&resetCode==0);
  assert(replies[replyHead].kind==ReplyKind::Result&&replies[replyHead].result==CommandResult::Expired);
  assert(replies[replyHead].value==0&&reboots==0&&flashState().erases==0);
 }else if(test=="reboot_stop_failure"){
  warmed();pendingReboot();stopNever=true;confirmReboot();advance(11000);
  assert(flashState().erases==0&&reboots==0&&lwServiceState()==RadioState::Stopped);
  assert(state==ManagerState::Fault);
 }else if(test=="confirmed_reset"){
  warmed();uint8_t cal[238]={1};assert(storageSaveCalibration(cal,238,0x26103304));
  uint8_t nonces[18]={1};assert(storageNonceBegin() && storageNonceSave(nonces));pendingReset();confirmReset();advance(500);
  assert(reboots==1&&flashState().erases==2&&stopped&&storageCount()==0);
  for(uint8_t value:EEPROM.bytes)assert(value==255);
  uint8_t noncesAfter[18]={};assert(!storageNonceLoad(noncesAfter,18)&&!resetPending);
 }else if(test=="erase_failure"){
  warmed();pendingReset();flashState().eraseFails=true;confirmReset();advance(5000);
  assert(flashState().erases==3&&reboots==0&&lwServiceState()==RadioState::Stopped);
  assert(Serial.output.find("erase verification failed")!=std::string::npos);
 }else if(test=="stop_failure"){
  warmed();pendingReset();stopNever=true;confirmReset();advance(11000);
  assert(flashState().erases==0&&reboots==0&&lwServiceState()==RadioState::Stopped);
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
 }else if(test=="log_ceiling"){
  AppSettings saved=settingsDefaults();saved.serialLevel=5;assert(storageSaveSettings(saved));appSetup();
  assert(appSettings().serialLevel==5 && unsigned(logGetLevel())==unsigned(FW_LOG_LEVEL));
  command({193,1,0,1,1,3,0}); // Unrelated temperature change with a higher saved verbosity.
  assert(appSettings().temperature==TemperatureUnit::Celsius && appSettings().serialLevel==5);
  assert(replies[0].result==CommandResult::Accepted && replies[0].kind==ReplyKind::Configuration);
  AppSettings loaded{};assert(storageLoadSettings(&loaded) && settingsEqual(loaded,appSettings()));
  command({193,4,0,2}); // Factory INFO preference is valid even in OFF/ERROR builds.
  assert(settingsEqual(appSettings(),settingsDefaults()) && replies[1].result==CommandResult::Accepted);
  assert(unsigned(logGetLevel())==unsigned(FW_LOG_LEVEL<3?FW_LOG_LEVEL:3));
  command({193,1,0,3,1,7,5});assert(appSettings().serialLevel==5 && replies[2].result==CommandResult::Accepted);
  assert(unsigned(logGetLevel())==unsigned(FW_LOG_LEVEL));
  const auto before=appSettings();const unsigned writes=EEPROM.puts;
  command({193,1,0,4,1,7,6});assert(settingsEqual(before,appSettings()) && EEPROM.puts==writes);
  EEPROM.failWrite=true;command({193,1,0,5,2,3,0,7,0});
  assert(settingsEqual(before,appSettings()) && replies[4].result==CommandResult::SaveFailed);
  assert(unsigned(logGetLevel())==unsigned(FW_LOG_LEVEL));
 }else if(test=="concurrency"){
  assert(storageBegin());ticks=100000;
  std::atomic<bool> done(false);std::atomic<unsigned> commits(0);
  std::thread writer([&](){inWorker=true;for(unsigned i=0;i<20000;++i){
   SensorSnapshot s=source;s.environment.gasOhms=i+1;s.airQuality.co2EquivalentPpm=float(i+1);
   if(storageAppend(s,91000+(i%9000)))++commits;
  }done.store(true);});
  while(!done.load()) {StoredReading r{};if(storageRead(0,&r))assert(r.sample.environment.gasOhms==uint32_t(r.sample.airQuality.co2EquivalentPpm));}
  writer.join();assert(commits>19000&&storageCount()<=1200);
 }else if(test=="status_after_blocking_radio" || test=="status_after_radio_wrap"){
  if(test=="status_after_radio_wrap"){ticks=UINT32_MAX-500000;eligibility=ticks;fullTime=ticks;}
  warmed();command({193,2,0,90});injectStatus=true;txDelay=180000;
  advance(61000); // GetStatus arrives in the receive window after a long transaction.
  bool found=false;
  for(unsigned i=0;i<replyCount;++i){
   const auto& reply=replies[(replyHead+i)%QUEUE_SIZE];
   if(reply.requestId==100){
    found=true;assert(reply.kind==ReplyKind::Status);
    assert(reply.status.uptimeSeconds==uint32_t(fullTime/1000));
   }
  }
  assert(found&&!injectStatus);
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
         'availability_recovery', 'availability_loss_during_rf', 'transient_availability', 'fixed_partial_availability',
         'batch_rejection_and_dedup', 'capacity_and_failures', 'reset_expiry_and_no_polling',
         'factory_confirmation_after_one_hour', 'reboot_preserves_persistence',
         'confirmation_operation_binding', 'factory_rejects_reboot_confirmation',
         'reboot_expiry_and_no_polling', 'challenge_expiry_boundary', 'challenge_expiry_wrap', 'reboot_stop_failure',
         'confirmed_reset', 'erase_failure', 'stop_failure', 'settings_and_calibration',
         'legacy_settings', 'log_ceiling', 'concurrency', 'timer_wrap', 'status_after_blocking_radio',
         'status_after_radio_wrap', 'clock_fault_and_uart', 'uart_backpressure']
with tempfile.TemporaryDirectory(prefix='monitor-system-') as temp:
    folder=Path(temp);prepare(folder)
    (folder/'core_management.inc').write_text((ROOT/'core_service.cpp').read_text().split('// Core management.\n',1)[1])
    (folder/'fixture.cpp').write_text(FIXTURE)
    exe=compile_host(folder,['fixture.cpp','storage_service.cpp',
                             'common.cpp','scheduler_service.cpp',
                             'encoder_service.cpp','log_service.cpp'],'system')
    for case in CASES:
        subprocess.run([str(exe),case],check=True)
        print('PASS:',case,flush=True)
    # The supported compile-time ceiling caps output without changing the
    # validity or persistence of application preferences and OTA batches.
    for level in range(5):
        exe=compile_host(folder,['fixture.cpp','storage_service.cpp',
                                 'common.cpp','scheduler_service.cpp',
                                 'encoder_service.cpp','log_service.cpp'],'system-level-'+str(level),
                         flags=['-DFW_LOG_LEVEL='+str(level)])
        subprocess.run([str(exe),'log_ceiling'],check=True)
        print('PASS: settings with compile-time log ceiling',level,flush=True)
