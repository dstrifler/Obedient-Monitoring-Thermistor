#!/usr/bin/env python3
"""Exercise the raw fallback against Bosch's real wrapper and fake hardware."""
from pathlib import Path
import os,shutil,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
LIB=Path(os.environ.get('BME68X_UPSTREAM',str(ROOT.parent/'.thermistor-tools/user/libraries/BME68x')))
ARDUINO=r'''
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <cassert>
using String=std::string;
enum {HIGH=1,LOW=0,OUTPUT=1};
extern uint32_t ticks;extern bool inRadio;
struct SerialFake {
 bool ready=true,blocking=true;std::string output;
 void begin(unsigned long){}
 void set_blocking(bool value){blocking=value;}
 bool writeable(){return ready;}
 size_t write(uint8_t c){assert(ready && !blocking && !inRadio);output+=char(c);return 1;}
};
extern SerialFake Serial;
unsigned long millis();unsigned long micros();
void delay(unsigned long);void delayMicroseconds(unsigned long);
inline void pinMode(unsigned,unsigned){}
inline void digitalWrite(unsigned,unsigned){}
'''
WIRE=r'''
#pragma once
#include "Arduino.h"
struct TwoWire {
 unsigned begins=0,transactions=0;uint8_t address=0;
 void begin(){assert(!inRadio);++begins;}
 void beginTransmission(uint8_t value){assert(!inRadio);address=value;++transactions;}
 size_t write(uint8_t){return 1;}
 uint8_t endTransmission(){return 0;}
 uint8_t requestFrom(int,int n){return n;}
 int available(){return 1;}int read(){return 0x61;}
};
extern TwoWire Wire,Wire1;
'''
FIXTURE=r'''
#include "Arduino.h"
#include "Wire.h"
#include "bme68xLibrary.h"
#include "sensor_service.h"
#include "storage_service.h"
#include "log_service.h"
#include <vector>
#include <cstring>
#include <limits>
#include <cmath>
uint32_t ticks=0;bool inRadio=false;SerialFake Serial;TwoWire Wire,Wire1;
unsigned long millis(){return ticks;}unsigned long micros(){return uint32_t(ticks*1000);}
void delay(unsigned long ms){ticks+=ms;}
void delayMicroseconds(unsigned long us){ticks+=(us+999)/1000;}
unsigned initializations=0,triggers=0,fetches=0;
int failAt=0;bool fields=true,varyReading=false;uint32_t triggeredAt=0;
bme68xData reading{};
void goodReading(){
 reading={};reading.temperature=25.25f;reading.humidity=50.5f;reading.pressure=100123;
 reading.gas_resistance=12345;reading.status=BME68X_NEW_DATA_MSK|BME68X_GASM_VALID_MSK|BME68X_HEAT_STAB_MSK;
}
extern "C" {
int8_t bme68x_init(bme68x_dev* dev){
 ++initializations;assert(!inRadio && dev->intf==BME68X_I2C_INTF);
 auto* comm=static_cast<bme68xScommT*>(dev->intf_ptr);
 assert(comm->i2c.wireobj==&Wire1 && comm->i2c.i2cAddr==0x77);
 uint8_t id=0;assert(dev->read(0xD0,&id,1,dev->intf_ptr)==BME68X_OK && id==0x61);
 return failAt==1?BME68X_E_COM_FAIL:BME68X_OK;
}
int8_t bme68x_get_conf(bme68x_conf* conf,bme68x_dev*){
 *conf={};return failAt==2?BME68X_E_COM_FAIL:BME68X_OK;
}
int8_t bme68x_set_conf(bme68x_conf* conf,bme68x_dev*){
 assert(conf->os_temp==BME68X_OS_2X && conf->os_pres==BME68X_OS_16X && conf->os_hum==BME68X_OS_1X);
 return failAt==3?BME68X_E_COM_FAIL:BME68X_OK;
}
int8_t bme68x_set_heatr_conf(uint8_t mode,const bme68x_heatr_conf* conf,bme68x_dev*){
 assert(mode==BME68X_FORCED_MODE && conf->enable && conf->heatr_temp==300 && conf->heatr_dur==100);
 return failAt==4?BME68X_E_COM_FAIL:BME68X_OK;
}
uint32_t bme68x_get_meas_dur(uint8_t mode,bme68x_conf*,bme68x_dev*){
 assert(mode==BME68X_FORCED_MODE);return 45001;
}
int8_t bme68x_set_op_mode(uint8_t mode,bme68x_dev*){
 assert(!inRadio && mode==BME68X_FORCED_MODE);++triggers;triggeredAt=ticks;
 return failAt==5?BME68X_E_COM_FAIL:BME68X_OK;
}
int8_t bme68x_get_data(uint8_t mode,bme68xData* out,uint8_t* count,bme68x_dev*){
 assert(!inRadio && mode==BME68X_FORCED_MODE && uint32_t(ticks-triggeredAt)>=151);++fetches;
 if(varyReading)reading.temperature=20.0f+float(fetches)/100.0f;
 *count=fields?1:0;*out=reading;
 return failAt==6?BME68X_E_COM_FAIL:(fields?BME68X_OK:BME68X_W_NO_NEW_DATA);
}
}
std::vector<SensorSnapshot> committed;
bool storageAppend(const SensorSnapshot& record,uint32_t){committed.push_back(record);return true;}
void drain(){for(unsigned i=0;i<100;++i)logPump();}

SensorData sentinel(){return SensorData{11,22,33333,44,true};}
void unchanged(const SensorData& data){
 assert(data.temperatureC==11 && data.humidityPct==22 && data.pressurePa==33333 && data.gasOhms==44 && data.gasValid);
}
void convert(){assert(sensorBegin() && sensorTrigger());ticks+=151;}
int main(int argc,char** argv){
 assert(argc==2);std::string test=argv[1];goodReading();logBegin(115200);
 AppSettings display=settingsDefaults();display.temperature=TemperatureUnit::Celsius;display.pressure=PressureUnit::Hpa;
 if(test=="forced_sample"){
  SensorData out=sentinel();assert(!sensorFetch(&out) && !sensorLatest(&out) && !sensorLatest(nullptr) && !sensorFetch(nullptr) && !sensorTrigger());unchanged(out);
  assert(sensorBegin() && sensorPresent() && ticks==0 && triggers==0);
  assert(Wire1.begins==1 && Wire.begins==0 && Wire1.address==0x77);
  assert(sensorTrigger() && !sensorTrigger() && ticks==0);
  ticks=150;assert(!sensorFetch(&out) && fetches==0);unchanged(out);
  ++ticks;assert(sensorFetch(&out) && fetches==1);
  assert(out.temperatureC==25.25f && out.humidityPct==50.5f && out.pressurePa==100123 && out.gasOhms==12345 && out.gasValid);
  SensorData latest{};assert(sensorLatest(&latest) && latest.temperatureC==25.25f);
  assert(!sensorFetch(&out) && sensorBegin() && Wire1.begins==1);
 }else if(test=="ready_timeout_retry"){
  fields=false;sensorStart();sensorPoll(display);assert(triggers==1);
  ticks=151;sensorPoll(display);assert(sensorPresent());
  ticks=1150;sensorPoll(display);assert(sensorPresent());
  ticks=1151;sensorPoll(display);assert(!sensorPresent());
  fields=true;ticks+=59999;sensorPoll(display);assert(initializations==1);
  ++ticks;sensorPoll(display);assert(initializations==2 && triggers==2 && Wire1.begins==1);
  ticks+=151;sensorPoll(display);assert(sensorPresent());drain();
#if FW_LOG_LEVEL >= FW_LOG_LEVEL_INFO
  assert(Serial.output.find("timed out without fresh data")!=std::string::npos);
  assert(Serial.output.find("T=25.25 C, RH=50.50 %RH, P=1001.23 mbar")!=std::string::npos);
#endif
 }else if(test=="init_errors"){
  for(int point=1;point<=4;++point){
   failAt=point;assert(!sensorBegin() && !sensorPresent() && !sensorTrigger());
   failAt=0;assert(sensorBegin());
  }
  failAt=5;assert(!sensorTrigger() && !sensorPresent());
  failAt=0;convert();failAt=6;SensorData out=sentinel();assert(!sensorFetch(&out) && !sensorPresent());unchanged(out);
  assert(Wire1.begins==1);
 }else if(test=="numeric_validation"){
  const float invalid[]={NAN,INFINITY,-41,86,-1,101,29999,110001};
  for(unsigned i=0;i<8;++i){
   goodReading();if(i<4)reading.temperature=invalid[i];else if(i<6)reading.humidity=invalid[i];else reading.pressure=invalid[i];
   convert();SensorData out=sentinel();assert(sensorFetch(&out) && sensorPresent());
   SensorSnapshot snapshot{};snapshot.environment=out;
   const uint8_t missing=i<4?1:i<6?2:4;
   assert(sensorValidMask(snapshot)==uint8_t(15 & ~missing));
   assert(out.gasValid && out.gasOhms==12345);
  }
 }else if(test=="partial_publication"){
  reading.pressure=NAN;sensorStart();
  while(ticks<12000){sensorPoll(display);ticks+=50;}
  assert(initializations==1 && committed.size()==4 && triggers==4);
  for(const auto& record:committed)assert(sensorValidMask(record)==0x0B);
  goodReading();sensorPoll(display);ticks+=200;sensorPoll(display);
  assert(initializations==1 && committed.size()==5 && sensorValidMask(committed.back())==0x0F);
 }else if(test=="gas_validity"){
  for(unsigned i=0;i<8;++i){
   goodReading();
   if(i==0)reading.status&=~BME68X_GASM_VALID_MSK;
   if(i==1)reading.status&=~BME68X_HEAT_STAB_MSK;
   if(i>=2){const float values[]={NAN,INFINITY,-1,0,4294967296.0f,12345};reading.gas_resistance=values[i-2];}
   convert();SensorData out{};assert(sensorFetch(&out) && sensorPresent());
   assert(out.temperatureC==25.25f && out.gasValid==(i==7));assert(out.gasOhms==(i==7?12345U:0U));
  }
 }else if(test=="stale_data"){
  reading.status=0;convert();SensorData out=sentinel();assert(!sensorFetch(&out) && sensorPresent());unchanged(out);
  ticks+=1000;assert(!sensorFetch(&out) && !sensorPresent());unchanged(out);
 }else if(test=="rollover_and_logs"){
  ticks=UINT32_MAX-100;reading.temperature=-0.25f;sensorStart();sensorPoll(display);sensorStart();
  ticks+=151;sensorPoll(display);drain();assert(fetches==1);
  SensorSnapshot snapshot{};assert(sensorLatestSnapshot(&snapshot) && snapshot.sampleUptimeSeconds>=4294967);
#if FW_LOG_LEVEL >= FW_LOG_LEVEL_INFO
  assert(Serial.output.find("T=-0.25 C")!=std::string::npos);
#endif
  ticks+=2848;sensorPoll(display);assert(triggers==1);
  ++ticks;sensorPoll(display);assert(triggers==2);
  ticks+=151;sensorPoll(display);ticks+=600000;sensorPoll(display);assert(triggers==3);
  ticks+=151;sensorPoll(display);sensorPoll(display);assert(triggers==3);
 }else if(test=="sample_cache"){
  ticks=UINT32_MAX-100;convert();SensorData out{};assert(sensorFetch(&out));
  ticks+=10000;assert(sensorLatest(&out) && out.temperatureC==25.25f);
  ++ticks;out=sentinel();assert(!sensorLatest(&out));unchanged(out);
  convert();assert(sensorFetch(&out) && sensorLatest(&out));
  assert(sensorTrigger());ticks+=151;failAt=6;assert(!sensorFetch(&out));
  out=sentinel();assert(!sensorLatest(&out));unchanged(out);
 }else if(test=="old_conversion"){
  convert();ticks+=90000;SensorData out{};assert(sensorFetch(&out));
  assert(!sensorLatest(&out));
 }else assert(false);
}
'''
with tempfile.TemporaryDirectory(prefix='raw-sensor-') as temp:
    folder=Path(temp)
    for name in ['sensor_service.cpp','sensor_service.h','common.cpp','common.h','storage_service.h','settings_defaults.h','log_service.h','log_service.cpp']:
        shutil.copyfile(ROOT/name,folder/name)
    shutil.copytree(LIB/'src',folder/'bosch')
    files={'Arduino.h':ARDUINO,'Wire.h':WIRE,
           'SPI.h':'#pragma once\n#include <cstdint>\nstruct SPIClass {uint8_t transfer(uint8_t v){return v;}};\n',
           'config.h':'#pragma once\n#define SENSOR_USE_WIRE1 1\n#define SENSOR_I2C_ADDR 0x77\n','fixture.cpp':FIXTURE}
    for name,text in files.items():(folder/name).write_text(text)
    cases=['forced_sample','ready_timeout_retry','init_errors','numeric_validation','partial_publication','gas_validity',
           'stale_data','rollover_and_logs','sample_cache','old_conversion']
    for level in [0,3,5]:
        exe=folder/'sensor'
        subprocess.run(['g++','-std=c++11','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-DSENSOR_BSEC_ENABLED=0','-DARDUINO=10819',
                        '-DFW_LOG_LEVEL='+str(level),'-I',str(folder),'-I',str(folder/'bosch'),
                        *[str(folder/name) for name in ['sensor_service.cpp','common.cpp','log_service.cpp','bosch/bme68xLibrary.cpp','fixture.cpp']],
                        '-o',str(exe)],check=True)
        for case in cases:subprocess.run([str(exe),case],check=True)
        print('PASS:',len(cases),'raw-sensor timing/validation/publication cases at log level',level,flush=True)
