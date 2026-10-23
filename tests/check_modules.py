#!/usr/bin/env python3
"""Compile real modules separately and exercise their public interfaces."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = Path(os.environ.get('RADIOLIB_UPSTREAM',
    str(ROOT.parent / '.thermistor-tools/user/libraries/RadioLib')))
ARDUINO = r'''
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <cassert>
extern uint32_t fakeMs;
struct SerialFake {
 bool ready=true,blocking=true;unsigned begins=0;std::string output;
 void begin(unsigned long){++begins;}
 void set_blocking(bool value){blocking=value;}
 bool writeable(){return ready;}
 size_t write(uint8_t value){assert(ready && !blocking);output.push_back(char(value));return 1;}
};
extern SerialFake Serial;
unsigned long millis();unsigned long micros();void delay(unsigned long);
'''
EEPROM = r'''
#pragma once
#include <cstdint>
#include <cstring>
#include <cassert>
struct EEPROMFake {
 unsigned initCalls=0,length=0;uint8_t data[512];
 EEPROMFake(){memset(data,255,sizeof(data));}
 int init(){++initCalls;return 0;}
 void setLength(unsigned size){length=size;}
 uint8_t read(int index){assert(index>=0 && index<512);return data[index];}
 void write(int index,uint8_t value){assert(index>=0 && index<512);data[index]=value;}
};
extern EEPROMFake EEPROM;
'''
FLASH = r'''
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <algorithm>
#include <cassert>
extern unsigned flashInstances;
// Preinitialized fake flash; image-address/initialization guards are covered
// by the board build and focused journal tests, rather than host addresses.
class FlashIAPBlockDevice {
 std::vector<uint8_t> data;
 public:
 FlashIAPBlockDevice(uint32_t,uint32_t size):data(size,255){++flashInstances;}
 int init(){return 0;}unsigned get_program_size(){return 4;}unsigned get_erase_size(){return 8192;}
 int read(void* out,uint32_t offset,size_t size){assert(offset+size<=data.size());memcpy(out,data.data()+offset,size);return 0;}
 int program(const void* source,uint32_t offset,size_t size){
  assert(offset+size<=data.size());const auto* bytes=static_cast<const uint8_t*>(source);
  for(size_t i=0;i<size;++i){assert((data[offset+i]&bytes[i])==bytes[i]);data[offset+i]&=bytes[i];}return 0;
 }
 int erase(uint32_t offset,size_t size){assert(offset+size<=data.size());std::fill(data.begin()+offset,data.begin()+offset+size,255);return 0;}
};
'''
HAL = r'''
#pragma once
#include <cstdint>
#define APOLLO3_B0 1
enum {AM_HAL_STATUS_SUCCESS=0,AM_HAL_CLKGEN_STATUS_RTCOSC_XTAL=1,
 CLKGEN_HFADJ_HFADJEN_EN=1,CLKGEN_HFADJ_HFADJGAIN_Gain_of_1_in_2=2,
 CLKGEN_HFADJ_HFADJCK_4SEC=4,CTIMER_STCFG_CLKSEL_HFRC_DIV16=16,
 AM_HAL_CLKGEN_CONTROL_XTAL_START=1,AM_HAL_CLKGEN_CONTROL_RTC_SEL_XTAL=2,
 AM_HAL_CLKGEN_CONTROL_HFADJ_ENABLE=3};
struct ClockRegisters {struct {uint32_t HFADJEN=0,HFXTADJ=0,HFADJGAIN=0,HFADJCK=0;} HFADJ_b;};
struct TimerRegisters {struct {uint32_t CLKSEL=16;} STCFG_b;};
extern ClockRegisters fakeClock;extern TimerRegisters fakeTimer;
#define CLKGEN (&fakeClock)
#define CTIMER (&fakeTimer)
struct am_hal_clkgen_status_t {uint32_t eRTCOSC=0;bool bXtalFailure=false;};
struct am_hal_rtc_time_t {uint32_t ui32ReadError=0,ui32Minute=0,ui32Second=0,ui32Hundredths=0;};
uint32_t am_hal_clkgen_control(uint32_t,void*);
uint32_t am_hal_clkgen_status_get(am_hal_clkgen_status_t*);
void am_hal_rtc_osc_enable();
uint32_t am_hal_stimer_counter_get();
uint32_t am_hal_rtc_time_get(am_hal_rtc_time_t*);
'''
BACKENDS = r'''
#include "Arduino.h"
#include "EEPROM.h"
#include "am_mcu_apollo.h"
#include "lorawan_service.h"
#include <vector>
#include <cstring>
uint32_t fakeMs=0;SerialFake Serial;EEPROMFake EEPROM;unsigned flashInstances=0;
uint32_t __etext=0,__data_start__=0,__data_end__=0;
unsigned long millis(){return fakeMs;}unsigned long micros(){return uint32_t(fakeMs*1000);}
void delay(unsigned long value){fakeMs+=value;}
ClockRegisters fakeClock;TimerRegisters fakeTimer;
bool xtal=false,rtc=false;
uint32_t am_hal_clkgen_control(uint32_t command,void*){
 if(command==2)xtal=true;
 if(command==3){fakeClock.HFADJ_b.HFADJEN=1;fakeClock.HFADJ_b.HFXTADJ=0x5b8;
  fakeClock.HFADJ_b.HFADJGAIN=2;fakeClock.HFADJ_b.HFADJCK=4;}
 return 0;
}
uint32_t am_hal_clkgen_status_get(am_hal_clkgen_status_t* out){out->eRTCOSC=xtal?1:0;out->bXtalFailure=false;return 0;}
void am_hal_rtc_osc_enable(){rtc=true;}
uint32_t am_hal_stimer_counter_get(){return fakeMs*3000;}
uint32_t am_hal_rtc_time_get(am_hal_rtc_time_t* out){
 uint32_t h=fakeMs/10;*out=am_hal_rtc_time_t{};
 out->ui32Minute=(h/6000)%60;out->ui32Second=(h/100)%60;out->ui32Hundredths=h%100;return rtc?0:1;
}
bool active=false;unsigned joins=0;int16_t sendResult=0;
std::vector<uint32_t> sends;
bool lwBegin(){return true;}bool lwIsActivated(){return active;}
int16_t lwRestore(){return 0;}bool lwRecover(){active=false;return true;}
bool lwActivate(){++joins;active=true;return true;}
int16_t lwLastActivationState(){return 0;}bool lwPersistenceHealthy(){return true;}
size_t lwMaxPayloadBytes(){return 11;}uint32_t lwTimeUntilUplink(){return 0;}
bool lwTryCapacityDataRate(size_t,uint16_t&){return false;}
int16_t lwSendReceive(const uint8_t* data,size_t size,uint8_t*,size_t* received){
 assert(active && size==11 && memcmp(data,"Hello World",11)==0);*received=0;sends.push_back(fakeMs);return sendResult;
}
'''
HEADERS = ['app_service.h', 'clock_service.h', 'eeprom_service.h', 'firmware_features.h',
           'log_config.h', 'log_service.h', 'lorawan_service.h', 'lorawan_timer.h',
           'nonce_store.h', 'payload_service.h', 'sensor_service.h', 'settings_service.h']
INCLUDES = ''.join('#include "' + name + '"\n' for name in HEADERS) * 2
OTHER = INCLUDES + r'''
bool bootstrapOther(){return eepromBootstrap();}
AppSettings* settingsOther(){return settingsGet();}
void changeOther(){settingsSetHumidityEnabled(false);LOG_WARN("Other","Shared modules");}
bool loadOther(uint8_t* out,size_t size){return nonceStoreLoad(out,size);}
'''
MAIN = INCLUDES + r'''
#include "Arduino.h"
#include "EEPROM.h"
#include <vector>
#include <cstring>
extern unsigned flashInstances,joins;extern bool active;extern int16_t sendResult;
extern std::vector<uint32_t> sends;
bool bootstrapOther();AppSettings* settingsOther();void changeOther();bool loadOther(uint8_t*,size_t);
void setup();void loop();
void advance(uint32_t ms){uint32_t start=fakeMs;while(uint32_t(fakeMs-start)<ms)loop();}
int main(){
 assert(flashInstances==1);
 assert(eepromBootstrap() && bootstrapOther() && EEPROM.initCalls==1 && EEPROM.length==512);
 settingsResetDefaults();assert(settingsGet()==settingsOther());
 assert(settingsGet()->humidityEnabled && settingsSetReportInterval(30));
 changeOther();assert(!settingsGet()->humidityEnabled);
 settingsSave();settingsSetReportInterval(60);settingsLoad();assert(settingsGet()->reportIntervalMin==30);
 settingsResetDefaults();SensorData data{25.0f,50.0f,100000.0f,1234};
 assert(!sensorBegin() && !sensorTrigger() && !sensorFetch(&data) && !sensorPresent());
 assert(data.temperatureC==25.0f && data.gasOhms==1234);
 uint8_t payload[256];assert(payloadEncodeUplink(payload,sizeof(payload),&data,settingsGet())==14);
 assert(payload[0]==1 && payload[1]==15 && payload[2]==0x09 && payload[3]==0xc4);
 assert(payloadEncodeUplink(payload,13,&data,settingsGet())==0);
 uint8_t command[]={CMD_SET_REPORT_INTERVAL,30};assert(payloadDecodeDownlink(command,2).valid);
 command[1]=31;assert(!payloadDecodeDownlink(command,2).valid);
 uint8_t nonces[18]={1},loaded[18]={};
 assert(!nonceStoreLoad(loaded,sizeof(loaded)));
 assert(nonceStoreSave(nonces) && loadOther(loaded,sizeof(loaded)) && memcmp(nonces,loaded,18)==0);
 assert(!nonceStoreLoad(nullptr,18) && !nonceStoreLoad(loaded,17));
 fakeMs=UINT32_MAX-LW_TRANSACTION_GUARD_MS-1;assert(!lwTimerRolloverPending());
 ++fakeMs;assert(lwTimerRolloverPending());fakeMs=0;assert(!lwTimerRolloverPending());
 setup();assert(fakeMs>=41000 && fakeMs<=41100 && EEPROM.initCalls==1);
 assert(Serial.begins==1);
 advance(245000);assert(joins==1 && sends.size()==4);
 for(size_t i=1;i<sends.size();++i)assert(sends[i]-sends[i-1]>=60000);
 fakeMs+=0x90000000UL;uint32_t resumed=fakeMs;
 loop();loop();assert(sends.size()==5 && uint32_t(fakeMs-resumed)<=100);
 // Serial congestion cannot stop the application from entering its fault.
 Serial.ready=false;sendResult=RADIOLIB_ERR_INVALID_PORT;advance(61000);
 size_t before=sends.size();advance(60000);assert(sends.size()==before);
 Serial.ready=true;advance(5000);
 assert(Serial.output.find("[ERROR][App] Permanent uplink configuration/API error.")!=std::string::npos);
 assert(Serial.output.find("[WARN][Other] Shared modules")!=std::string::npos);
}
'''


def compile_program(folder, sources, output, flags=()):
    sanitizer = (['-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                  '-fno-sanitize-recover=all'] if os.environ.get('MODULE_SANITIZERS') == '1' else [])
    subprocess.run(['g++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                    *sanitizer, *flags, '-I', str(folder), *[str(folder / p) for p in sources],
                    '-o', str(folder / output)], check=True)
    subprocess.run([str(folder / output)], check=True)


with tempfile.TemporaryDirectory(prefix='thermistor-modules-') as tmp:
    folder = Path(tmp)
    # Stage sources with a non-secret config; quoted includes must never select
    # the real provisioning header in these host checks.
    for source in [*ROOT.glob('*.h'), *ROOT.glob('*.cpp')]:
        if source.name != 'config.h':
            (folder / source.name).write_text(source.read_text())
    errors = '\n'.join(line for line in (UPSTREAM / 'src/TypeDef.h').read_text().splitlines()
                       if re.match(r'#define RADIOLIB_ERR_\w+\s+\(-?\d+\)', line))
    files = {'Arduino.h': ARDUINO, 'EEPROM.h': EEPROM, 'FlashIAPBlockDevice.h': FLASH,
             'RadioLib.h': '#pragma once\n#define RADIOLIB_LORAWAN_NONCES_BUF_SIZE 18\n' + errors,
             'am_mcu_apollo.h': HAL, 'backends.cpp': BACKENDS, 'other.cpp': OTHER,
             'main.cpp': MAIN, 'config.h': '#pragma once\n#define SERIAL_BAUD_RATE 115200\n'
             '#define CLOCK_XTAL_STABILIZATION_MS 2000UL\n#define SENSOR_USE_WIRE1 1\n'
             '#define SENSOR_I2C_ADDR 0x77\n#define SENSOR_FORCED_DELAY_MS 250\n',
             'sketch.cpp': (ROOT / 'Obedient_Monitoring_Thermistor.ino').read_text()}
    for name, contents in files.items():
        (folder / name).write_text(contents)
    for name in HEADERS:
        subprocess.run(['g++', '-std=c++11', '-Wall', '-Wextra', '-Werror', '-I', str(folder),
                        '-x', 'c++', '-fsyntax-only', '-'],
                       input=('#include "' + name + '"\n') * 2, text=True, check=True)
    print('PASS: independent public headers and repeated inclusion', flush=True)
    sources = ['app_service.cpp', 'clock_service.cpp', 'eeprom_service.cpp', 'lorawan_timer.cpp',
               'nonce_store.cpp', 'payload_service.cpp', 'sensor_service.cpp', 'settings_service.cpp',
               'log_service.cpp', 'sketch.cpp', 'backends.cpp', 'other.cpp', 'main.cpp']
    compile_program(folder, sources, 'modules')
    print('PASS: separate module linking, shared state, persistence API and Arduino wrapper behavior', flush=True)

    # Check the retained backend's interface behind its explicit opt-in, using
    # a fake BME library; this does not validate the real sensor or its wiring.
    (folder / 'Wire.h').write_text(r'''
#pragma once
struct WireFake {unsigned begins=0;void begin(){++begins;}};
extern WireFake Wire,Wire1;
''')
    (folder / 'bme68xLibrary.h').write_text(r'''
#pragma once
#include <cstdint>
#include "Wire.h"
enum {BME68X_FORCED_MODE=1};
struct bme68xData {float temperature=25,humidity=50,pressure=100000;uint32_t gas_resistance=1234;};
extern bool fetchWorks,readWorks;extern unsigned bindings;
struct Bme68x {
 void begin(uint8_t,WireFake&){++bindings;}
 void setTPH(){}void setHeaterProf(int,int){}void setOpMode(int){}
 bool fetchData(){return fetchWorks;}
 bool getData(bme68xData& out){out=bme68xData{};return readWorks;}
};
''')
    (folder / 'sensor_main.cpp').write_text(r'''
#include "sensor_service.h"
#include "Arduino.h"
#include "Wire.h"
bool fetchWorks=true,readWorks=true;unsigned bindings=0;WireFake Wire,Wire1;
uint32_t fakeMs=0;unsigned long millis(){return fakeMs;}unsigned long micros(){return uint32_t(fakeMs*1000);}
void delay(unsigned long value){fakeMs+=value;}
int main(){
 assert(!sensorPresent() && sensorBegin() && sensorPresent());
 assert(Wire1.begins==1 && Wire.begins==0 && bindings==1 && fakeMs==250);
 SensorData data{};assert(sensorTrigger() && sensorFetch(&data));
 assert(data.temperatureC==25 && data.gasOhms==1234);
 fetchWorks=false;assert(!sensorFetch(&data) && !sensorPresent());
 fetchWorks=true;assert(sensorTrigger() && bindings==2);
 readWorks=false;assert(!sensorFetch(&data) && !sensorPresent());
 assert(!sensorFetch(nullptr));
}
''')
    compile_program(folder, ['sensor_service.cpp', 'sensor_main.cpp'], 'sensor_backend',
                    ['-DSENSOR_SERVICE_ENABLED=1'])
    print('PASS: retained sensor backend opt-in and failure propagation', flush=True)
