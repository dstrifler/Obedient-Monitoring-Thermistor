#!/usr/bin/env python3
"""Exercise real firmware BSEC scheduling/persistence with public-API fakes.

The opaque Bosch ARM algorithm cannot execute on the host. Its public headers
are real; C calls and sensor/RTOS hardware are simulated. The board build links
the unmodified algorithm and actual Mbed/BME68x implementation.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
BSEC=Path(os.environ.get('BSEC_UPSTREAM',str(ROOT.parent/'work/Bosch-BSEC2-Library')))
BME=ROOT.parent/'.thermistor-tools/user/libraries/BME68x'
ARDUINO=r'''
#pragma once
#include <cstdint>
#include <cstddef>
#include <cassert>
extern uint32_t ticks;extern bool inWorker;extern unsigned heldLocks;
inline unsigned long millis(){return ticks;}
'''
WIRE=r'''
#pragma once
#include "Arduino.h"
struct TwoWire {unsigned begins=0;void begin(){assert(inWorker);++begins;}};
extern TwoWire Wire,Wire1;
'''
MBED=r'''
#pragma once
#include "Arduino.h"
#include <chrono>
const int osPriorityNormal=0,osOK=0;
extern unsigned threadStarts;
namespace mbed {inline void (*callback(void(*f)()))(){return f;}}
namespace rtos {
struct Mutex {bool held=false;void lock(){assert(!held);held=true;++heldLocks;}
 void unlock(){assert(held);held=false;--heldLocks;}};
struct Thread {Thread(int,size_t,unsigned char*,const char*){}
 int start(void(*)()){++threadStarts;return osOK;}};
namespace ThisThread {inline void sleep_for(std::chrono::milliseconds){}}
}
'''
SENSOR=r'''
#pragma once
#include "Wire.h"
#include <bme68x/bme68x.h>
#include <inc/bsec_interface.h>
using bme68xData=bme68x_data;
extern int sensorError;extern unsigned begins,triggers,fetches;
extern bme68xData reading;extern uint16_t heaterTemperature,heaterDuration;
extern bool haveFields;
class Bme68x {public:int8_t status=0;
 void begin(uint8_t address,TwoWire& wire){assert(inWorker && address==0x77 && &wire==&Wire1);++begins;status=sensorError;}
 void setTPH(uint8_t,uint8_t,uint8_t){assert(inWorker);status=sensorError;}
 void setHeaterProf(uint16_t temperature,uint16_t duration){assert(inWorker);heaterTemperature=temperature;heaterDuration=duration;status=sensorError;}
 uint32_t getMeasDur(uint8_t mode){assert(inWorker && mode==BME68X_FORCED_MODE);return 45001;}
 void setOpMode(uint8_t mode){assert(inWorker && mode==BME68X_FORCED_MODE);++triggers;status=sensorError;}
 uint8_t fetchData(){assert(inWorker);++fetches;status=sensorError;return haveFields?1:0;}
 uint8_t getData(bme68xData& out){out=reading;return 0;}
};
'''
EEPROM=r'''
#pragma once
#include "Arduino.h"
#include <cstring>
struct EEPROMFake {
 uint8_t bytes[512];unsigned puts=0;bool failWrite=false;
 EEPROMFake(){memset(bytes,255,sizeof(bytes));}
 uint8_t read(size_t index){assert(!inWorker && index<512);return bytes[index];}
 template<class T>void put(size_t index,const T& data){
  assert(!inWorker && heldLocks==0 && index+sizeof(T)<=512);++puts;
  if(!failWrite)memcpy(bytes+index,&data,sizeof(T));
 }
};extern EEPROMFake EEPROM;
'''
FIXTURE=r'''
#include "Arduino.h"
#include "Wire.h"
#include "EEPROM.h"
#include "bsec2.h"
#include "sensor_service.h"
#include "bsec_state.h"
#include "log_service.h"
#include <cstring>
#include <string>
#include <limits>
uint32_t ticks=0;bool inWorker=false;unsigned heldLocks=0,threadStarts=0,logs=0;
void logWrite(LogLevel,const char*,const char*,...){assert(!inWorker);++logs;}
TwoWire Wire,Wire1;EEPROMFake EEPROM;
bool nonceStoreBegin(){return true;}
bool nonceStoreSave(const uint8_t*){return true;}
bool nonceStoreLoad(uint8_t*,size_t){return false;}
bool nonceStoreErase(){return true;}
int sensorError=0;unsigned begins=0,triggers=0,fetches=0,initializations=0,steps=0,controls=0;
bme68xData reading{};uint16_t heaterTemperature=0,heaterDuration=0;bool haveFields=true;
int controlStatus=0,stepStatus=0;bool rejectState=false;unsigned sets=0,stateGets=0;
bool hideVoc=false,initializing=false;
bool eepromBootstrap(){return true;}
extern "C" {
bsec_library_return_t bsec_init(){assert(inWorker);++initializations;return BSEC_OK;}
bsec_library_return_t bsec_set_configuration(const uint8_t*,uint32_t n,uint8_t*,uint32_t w){
 assert(inWorker && n==1943 && w==4096);return BSEC_OK;
}
bsec_library_return_t bsec_set_state(const uint8_t*,uint32_t n,uint8_t*,uint32_t){
 assert(inWorker && n==238);++sets;return rejectState?BSEC_E_CONFIG_VERSIONMISMATCH:BSEC_OK;
}
bsec_library_return_t bsec_update_subscription(const bsec_sensor_configuration_t* requested,uint8_t n,
 bsec_sensor_configuration_t*,uint8_t* required){
 assert(inWorker && n==7 && *required==BSEC_MAX_PHYSICAL_SENSOR);
 for(unsigned i=0;i<n;++i)assert(requested[i].sample_rate==BSEC_SAMPLE_RATE_LP);
 assert(requested[0].sensor_id==BSEC_OUTPUT_IAQ && requested[1].sensor_id==BSEC_OUTPUT_CO2_EQUIVALENT && requested[2].sensor_id==BSEC_OUTPUT_BREATH_VOC_EQUIVALENT);
 *required=0;return BSEC_OK;
}
bsec_library_return_t bsec_sensor_control(int64_t time,bsec_bme_settings_t* out){
 assert(inWorker);++controls;*out={};out->next_call=time+3000000000LL;
 out->op_mode=BME68X_FORCED_MODE;out->trigger_measurement=1;
 out->temperature_oversampling=2;out->pressure_oversampling=4;out->humidity_oversampling=1;
 out->heater_temperature=320;out->heater_duration=150;
 out->process_data=BSEC_PROCESS_PRESSURE|BSEC_PROCESS_TEMPERATURE|BSEC_PROCESS_HUMIDITY|BSEC_PROCESS_GAS|BSEC_PROCESS_PROFILE_PART|(1UL<<(BSEC_INPUT_HEATSOURCE-1));
 return bsec_library_return_t(controlStatus);
}
bsec_library_return_t bsec_do_steps(const bsec_input_t* inputs,uint8_t n,bsec_output_t* out,uint8_t* count){
 assert(inWorker && n==6 && *count==BSEC_NUMBER_OUTPUTS);++steps;
 bool pressure=false;
 for(unsigned i=0;i<n;++i){
  if(inputs[i].sensor_id==BSEC_INPUT_PRESSURE){assert(inputs[i].signal==98765);pressure=true;}
  if(inputs[i].sensor_id==BSEC_INPUT_HEATSOURCE)assert(inputs[i].signal==0);
 }
 assert(pressure);*count=7;memset(out,0,sizeof(bsec_output_t)*7);
 const uint8_t ids[]={BSEC_OUTPUT_IAQ,BSEC_OUTPUT_CO2_EQUIVALENT,BSEC_OUTPUT_BREATH_VOC_EQUIVALENT,BSEC_OUTPUT_STABILIZATION_STATUS,BSEC_OUTPUT_RUN_IN_STATUS,BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE,BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY};
 const float values[]={42.25f,1234.5f,1.23456789f,1,1,19.25f,57.5f};
 for(unsigned i=0;i<7;++i){out[i].sensor_id=ids[i];out[i].signal=values[i];out[i].accuracy=initializing?0:3;}
 if(hideVoc)out[2].signal=std::numeric_limits<float>::quiet_NaN();
 return bsec_library_return_t(stepStatus);
}
bsec_library_return_t bsec_get_state(uint8_t,uint8_t* state,uint32_t capacity,uint8_t*,uint32_t,uint32_t* size){
 assert(inWorker && capacity==238);++stateGets;memset(state,0x42,238);*size=238;return BSEC_OK;
}
}
#include "bsec_service.cpp"
void workerTick(){assert(!inWorker);inWorker=true;workerStep();inWorker=false;}
void advance(uint32_t span){uint32_t start=ticks;while(uint32_t(ticks-start)<span){workerTick();ticks+=10;}}
void goodReading(){reading={};reading.temperature=20.25;reading.humidity=55.5;reading.pressure=98765;
 reading.gas_resistance=12345;reading.status=BME68X_NEW_DATA_MSK|BME68X_GASM_VALID_MSK|BME68X_HEAT_STAB_MSK;}
int main(int argc,char** argv){
 assert(argc==2);std::string test=argv[1];goodReading();
 if(test=="state"){
  uint8_t state[238],loaded[238];memset(state,0x42,238);memset(loaded,0xaa,238);
  for(unsigned i=0;i<128;++i)EEPROM.bytes[i]=uint8_t(i);
  assert(!bsecStateLoad(loaded,238,0x26103304) && loaded[0]==0xaa);
  assert(bsecStateSave(state,238,0x26103304) && EEPROM.puts==1);
  for(unsigned i=0;i<128;++i)assert(EEPROM.bytes[i]==uint8_t(i));
  assert(bsecStateLoad(loaded,238,0x26103304)==238 && memcmp(state,loaded,238)==0);
  assert(!bsecStateLoad(loaded,237,0x26103304) && !bsecStateSave(state,239,0x26103304) && !bsecStateSave(nullptr,1,0x26103304));
  for(unsigned i=128;i<382;++i){EEPROM.bytes[i]^=1;assert(!bsecStateLoad(loaded,238,0x26103304));EEPROM.bytes[i]^=1;}
  EEPROM.failWrite=true;state[0]^=1;assert(!bsecStateSave(state,238,0x26103304));
  return 0;
 }
 if(test=="restore" || test=="reject_restore"){
  uint8_t state[238]={};assert(bsecStateSave(state,238,0x26103304));rejectState=test=="reject_restore";
 }
 if(test=="rollover")ticks=UINT32_MAX-500;
 if(test=="retry")sensorError=BME68X_E_COM_FAIL;
 if(test=="gas_invalid")reading.status&=~BME68X_HEAT_STAB_MSK;
 sensorStart();sensorStart();assert(threadStarts==1 && Wire1.begins==0);
 advance(7000);
 SensorSnapshot sample{};
 if(test=="retry"){
  assert(!sensorPresent() && !sensorLatestSnapshot(&sample) && begins==1);
  sensorError=0;advance(50000);assert(begins==1);advance(5000);assert(sensorLatestSnapshot(&sample) && begins==2);
 }else{
  assert(sensorLatestSnapshot(&sample) && sensorPresent());
  assert(sample.environment.temperatureC==(test=="gas_invalid"?20.25:19.25) && sample.environment.pressurePa==98765);
  assert(Wire1.begins==1 && Wire.begins==0 && triggers==3 && fetches==3);
  assert(heaterTemperature==320 && heaterDuration==150); // BSEC owns the heater.
  if(test=="gas_invalid")assert(!sample.environment.gasValid && sample.airQuality.validMask==0 && steps==0);
  else{
   assert(sample.airQuality.validMask==7 && sample.airQuality.iaqAccuracy==3);
   assert(sample.airQuality.breathVocEquivalentPpm==1.23456789f);
   assert(sample.environment.gasValid && sample.airQuality.stabilized && sample.airQuality.runIn);
   assert(sample.temperatureHumidityCompensated && sample.environment.humidityPct==57.5);
  }
 }
 if(test=="direct_ring_and_stop"){
  assert(storageCount()==3);StoredReading latest{};assert(storageRead(0,&latest));
  assert(latest.sample.sampleUptimeSeconds==sample.sampleUptimeSeconds);
  assert(!sensorStop());workerLoop();assert(sensorStop());
 }
 if(test=="rollover")assert(sample.sampleUptimeSeconds>=4294967);
 if(test=="restore")assert(sets==1 && initializations==1);
 if(test=="reject_restore")assert(sets==1 && initializations==2);
 if(test=="healthy"){
  // Main can be blocked for join/RX while worker steps retain a 3-second cadence.
  unsigned before=triggers;advance(12000);assert(triggers==before+4 && EEPROM.puts==0);
  sensorPoll();assert(EEPROM.puts==1 && heldLocks==0);
  advance(10000);sensorPoll();assert(EEPROM.puts==1);
  uint32_t oldTime=sample.sampleUptimeSeconds;ticks+=15000;
  assert(!sensorLatestSnapshot(&sample) && sample.sampleUptimeSeconds==oldTime);
 }
 if(test=="missing_fields"){
  haveFields=false;advance(7000);assert(!sensorPresent() && !sensorLatestSnapshot(&sample));
 }
 if(test=="errors"){
  stepStatus=-1;advance(4000);assert(!sensorPresent() && !sensorLatestSnapshot(&sample));
  stepStatus=0;advance(65000);assert(sensorLatestSnapshot(&sample));
 }
 if(test=="warning"){
  controlStatus=100;advance(4000);assert(sensorLatestSnapshot(&sample));sensorPoll();assert(gDiagnostic==0);
 }
 if(test=="save_retry"){
  EEPROM.failWrite=true;sensorPoll();assert(gSavePending && EEPROM.puts==1);
  EEPROM.failWrite=false;advance(61000);sensorPoll();assert(!gSavePending && EEPROM.puts==2);
 }
 if(test=="save_interval"){
  sensorPoll();assert(stateGets==1 && EEPROM.puts==1);
  advance((5UL*60+59)*60000UL);sensorPoll();assert(stateGets==1 && EEPROM.puts==1);
  advance(61000);sensorPoll();assert(stateGets==2 && EEPROM.puts==2);
 }
 if(test=="partial_output"){
  hideVoc=true;advance(4000);assert(sensorLatestSnapshot(&sample) && sample.airQuality.validMask==3);
 }
 if(test=="initializing"){
  initializing=true;advance(4000);
  assert(sensorLatestSnapshot(&sample) && sample.airQuality.validMask==7 && sample.airQuality.iaqAccuracy==0);
 }
 if(test=="invalid_numeric"){
  reading.temperature=std::numeric_limits<float>::quiet_NaN();advance(4000);
  assert(!sensorPresent() && !sensorLatestSnapshot(&sample));
 }
 assert(!inWorker && heldLocks==0);
}
'''
with tempfile.TemporaryDirectory(prefix='bsec-service-') as tmp:
    folder=Path(tmp)
    for name in ['bsec_service.cpp','bsec_service.h','bsec_state.cpp','bsec_state.h',
                 'sensor_service.cpp','sensor_service.h','storage_service.cpp','storage_service.h','sensor_types.cpp','sensor_types.h','app_settings.cpp','app_settings.h','settings_defaults.h','nonce_store.h','firmware_features.h']:
        shutil.copyfile(ROOT/name,folder/name)
    files={'Arduino.h':ARDUINO,'Wire.h':WIRE,'mbed.h':MBED,
           'bsec2.h':SENSOR,'EEPROM.h':EEPROM,'fixture.cpp':FIXTURE,
           'config.h':'#pragma once\n#define SENSOR_I2C_ADDR 0x77\n#define SENSOR_USE_WIRE1 1\n',
           'eeprom_service.h':'#pragma once\n#define EEPROM_ADDR_SETTINGS_BASE 32\n#define EEPROM_ADDR_SETTINGS_END 96\n#define EEPROM_TOTAL_BYTES 512\nbool eepromBootstrap();\n'}
    for name,content in files.items():(folder/name).write_text(content)
    # Use production compile-time logging macros, including printf checking.
    for name in ['log_service.h','log_config.h']:
        shutil.copyfile(ROOT/name,folder/name)
    exe=folder/'bsec'
    cases=['healthy','gas_invalid','retry','rollover','restore','reject_restore','state',
           'missing_fields','errors','warning','save_retry','save_interval','partial_output','initializing','invalid_numeric','direct_ring_and_stop']
    for level in [0,3,5]:
        subprocess.run(['g++','-std=c++11','-Wall','-Wextra','-Werror','-DFW_LOG_LEVEL='+str(level),
                        '-I',str(folder),'-I',str(BSEC/'src'),'-I',str(BME/'src'),str(folder/'fixture.cpp'),
                        str(folder/'sensor_service.cpp'),str(folder/'storage_service.cpp'),str(folder/'sensor_types.cpp'),str(folder/'app_settings.cpp'),str(folder/'bsec_state.cpp'),'-o',str(exe)],check=True)
        for case in cases:
            subprocess.run([str(exe),case],check=True)
        print('PASS: 16 BSEC acquisition/publication/calibration/status cases at log level',level,flush=True)
