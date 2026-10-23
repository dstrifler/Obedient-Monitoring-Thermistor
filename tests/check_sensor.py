#!/usr/bin/env python3
"""Run the firmware with Bosch's unmodified C++ wrapper and a fake sensor API.

The actual wrapper (including getData's zero return in forced mode) is compiled.
The lower C sensor API/hardware is simulated; the board build also compiles the
real C implementation. These checks do not establish physical sensor operation.
"""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
LIB = Path(os.environ.get('BME68X_UPSTREAM',
    str(ROOT.parent / '.thermistor-tools/user/libraries/BME68x')))
RADIO = Path(os.environ.get('RADIOLIB_UPSTREAM',
    str(ROOT.parent / '.thermistor-tools/user/libraries/RadioLib')))

ARDUINO = r'''
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
WIRE = r'''
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
FIXTURE = r'''
#include "Arduino.h"
#include "Wire.h"
#include "bme68xLibrary.h"
#include "sensor_service.h"
#include "app_service.h"
#include "lorawan_service.h"
#include "log_service.h"
#include "payload_service.h"
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
bool active=false;unsigned joins=0;uint32_t joinedAt=0;std::vector<uint32_t> sends;
size_t capacity=11;bool macTest=false,stopSensor=false;
std::vector<std::vector<uint8_t>> packets;
bool clockBegin(){return true;}bool eepromBootstrap(){return true;}
bool lwBegin(){return true;}bool lwIsActivated(){return active;}
int16_t lwRestore(){return 0;}bool lwPersistenceHealthy(){return true;}
bool lwRecover(){active=false;return true;}
int16_t lwLastActivationState(){return 0;}
bool lwActivate(){
 inRadio=true;ticks+=6500;inRadio=false;++joins;active=true;joinedAt=ticks;return true;
}
size_t lwMaxPayloadBytes(){return capacity;}uint32_t lwTimeUntilUplink(){return 0;}
bool lwTryCapacityDataRate(size_t,uint16_t&){return false;}
int16_t lwSendReceive(const uint8_t* p,size_t n,uint8_t*,size_t* received){
 assert(active && (n==11 || (macTest && n==0)));
 if(n){
  assert((p[0]>>4)==1);
  if(failAt==1 || (stopSensor && sends.size()>=1)){
   assert(p[0]==0x10);for(size_t i=1;i<n;++i)assert(p[i]==0);
  }else{
   assert((p[0]&7)==7);
   int16_t temperature=int16_t((uint16_t(p[1])<<8)|p[2]);
   assert(temperature==lroundf(reading.temperature*100));
   assert(p[3]==0x13 && p[4]==0xba && p[5]==0xc3 && p[6]==0x8e);
   if(reading.status&BME68X_GASM_VALID_MSK)assert(p[0]==0x1f && p[9]==0x30 && p[10]==0x39);
   else {assert(p[0]==0x17);for(size_t i=7;i<n;++i)assert(p[i]==0);}
  }
 }
 assert(uint32_t(ticks-joinedAt)>=60000);sends.push_back(ticks);
 packets.emplace_back(p,p+n);
 if(macTest && sends.size()==2)capacity=11;
 if(stopSensor)fields=false;
 inRadio=true;ticks+=2100;inRadio=false;*received=0;return 0;
}
void drain(){for(unsigned i=0;i<100;++i)logPump();}
void advance(uint32_t span){uint32_t start=ticks;while(uint32_t(ticks-start)<span)appLoop();}
SensorData sentinel(){return SensorData{11,22,33333,44,true};}
void unchanged(const SensorData& data){
 assert(data.temperatureC==11 && data.humidityPct==22 && data.pressurePa==33333 && data.gasOhms==44 && data.gasValid);
}
void convert(){assert(sensorBegin() && sensorTrigger());ticks+=151;}
int main(int argc,char** argv){
 assert(argc==2);std::string test=argv[1];goodReading();logBegin(115200);
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
  fields=false;sensorStart();sensorPoll();assert(triggers==1);
  ticks=151;sensorPoll();assert(sensorPresent());
  ticks=1150;sensorPoll();assert(sensorPresent());
  ticks=1151;sensorPoll();assert(!sensorPresent());
  fields=true;ticks+=59999;sensorPoll();assert(initializations==1);
  ++ticks;sensorPoll();assert(initializations==2 && triggers==2 && Wire1.begins==1);
  ticks+=151;sensorPoll();assert(sensorPresent());drain();
#if FW_LOG_LEVEL >= FW_LOG_LEVEL_INFO
  assert(Serial.output.find("timed out without fresh data")!=std::string::npos);
  assert(Serial.output.find("T=25.25 C, RH=50.50 %, P=1001.23 hPa, gas=12345 ohm")!=std::string::npos);
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
   convert();SensorData out=sentinel();assert(!sensorFetch(&out) && !sensorPresent());unchanged(out);
  }
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
  ticks=UINT32_MAX-100;reading.temperature=-0.25f;sensorStart();sensorPoll();sensorStart();
  ticks+=151;sensorPoll();drain();assert(fetches==1);
#if FW_LOG_LEVEL >= FW_LOG_LEVEL_INFO
  assert(Serial.output.find("T=-0.25 C")!=std::string::npos);
#endif
  ticks+=59848;sensorPoll();assert(triggers==1);
  ++ticks;sensorPoll();assert(triggers==2);
  ticks+=151;sensorPoll();ticks+=600000;sensorPoll();assert(triggers==3);
  ticks+=151;sensorPoll();sensorPoll();assert(triggers==3);
 }else if(test=="sample_cache"){
  ticks=UINT32_MAX-100;convert();SensorData out{};assert(sensorFetch(&out));
  ticks+=65000;assert(sensorLatest(&out) && out.temperatureC==25.25f);
  ++ticks;out=sentinel();assert(!sensorLatest(&out));unchanged(out);
  convert();assert(sensorFetch(&out) && sensorLatest(&out));
  assert(sensorTrigger());ticks+=151;failAt=6;assert(!sensorFetch(&out));
  out=sentinel();assert(!sensorLatest(&out));unchanged(out);
 }else if(test=="old_conversion"){
  convert();ticks+=90000;SensorData out{};assert(sensorFetch(&out));
  assert(!sensorLatest(&out));
 }else if(test=="app_healthy" || test=="app_sensor_absent" || test=="app_uart_blocked" ||
          test=="app_mac_only" || test=="app_sensor_stops" || test=="app_gas_unavailable"){
  varyReading=true;
  if(test=="app_sensor_absent")failAt=1;
  if(test=="app_uart_blocked")Serial.ready=false;
  if(test=="app_mac_only"){macTest=true;capacity=0;}
  if(test=="app_sensor_stops")stopSensor=true;
  if(test=="app_gas_unavailable")reading.status&=~BME68X_GASM_VALID_MSK;
  appSetup();advance(310000);
  assert(joins==1 && sends.size()==5);
  assert(sends.front()-joinedAt>=60000 && sends.front()-joinedAt<=60100);
  for(size_t i=1;i<sends.size();++i)assert(sends[i]-sends[i-1]>=60000 && sends[i]-sends[i-1]<=60200);
  if(failAt)assert(triggers==0 && initializations>=5 && initializations<=6);
  else if(!stopSensor)assert(fetches>=5 && triggers>=5 && sensorPresent());
  if(macTest)assert(packets[0].empty() && packets[1].empty() && packets[2].size()==11);
  if(test=="app_healthy")assert(packets.front()!=packets.back());
 }else assert(false);
}
'''

assert (LIB / 'src/bme68xLibrary.cpp').is_file(), 'Install Bosch BME68x Sensor library (1.3.40408) or set BME68X_UPSTREAM'
with tempfile.TemporaryDirectory(prefix='thermistor-sensor-') as tmp:
    folder = Path(tmp)
    for source in [*ROOT.glob('*.h'), *ROOT.glob('*.cpp')]:
        if source.name != 'config.h':
            shutil.copyfile(source, folder / source.name)
    # Copy dependency sources without edits. Never include live provisioning.
    shutil.copytree(LIB / 'src', folder / 'bosch')
    constants = '\n'.join(line for line in (RADIO / 'src/TypeDef.h').read_text().splitlines()
                         if re.match(r'#define RADIOLIB_ERR_\w+\s+\(-?\d+\)', line))
    files = {'Arduino.h': ARDUINO, 'Wire.h': WIRE,
             'SPI.h': '#pragma once\n#include <cstdint>\nstruct SPIClass {uint8_t transfer(uint8_t v){return v;}};\n',
             'RadioLib.h': '#pragma once\n' + constants,
             'config.h': '#pragma once\n#define SENSOR_USE_WIRE1 1\n#define SENSOR_I2C_ADDR 0x77\n#define SERIAL_BAUD_RATE 115200\n',
             'fixture.cpp': FIXTURE}
    for name, text in files.items():
        (folder / name).write_text(text)
    program = folder / 'sensor'
    cases = ['forced_sample', 'ready_timeout_retry', 'init_errors', 'numeric_validation',
             'gas_validity', 'stale_data', 'rollover_and_logs',
             'sample_cache', 'old_conversion', 'app_healthy', 'app_sensor_absent', 'app_uart_blocked',
             'app_mac_only', 'app_sensor_stops', 'app_gas_unavailable']
    for level in (0, 3, 5):
        subprocess.run(['g++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                        '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                        '-DARDUINO=10819', '-DFW_LOG_LEVEL=' + str(level),
                        '-I', str(folder), '-I', str(folder / 'bosch'),
                        *[str(folder / name) for name in ['sensor_service.cpp', 'app_service.cpp',
                          'lorawan_timer.cpp', 'log_service.cpp', 'payload_service.cpp', 'bosch/bme68xLibrary.cpp', 'fixture.cpp']],
                        '-o', str(program)], check=True)
        for case in cases:
            subprocess.run([str(program), case], check=True)
        print('PASS:', len(cases), 'sensor/real-Bosch-wrapper and app integration scenarios at log level', level, flush=True)
