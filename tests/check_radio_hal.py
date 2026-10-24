#!/usr/bin/env python3
"""Exercise firmware Apollo3 GPIO handling with stock RadioLib HAL and TX code."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = Path(os.environ.get('RADIOLIB_UPSTREAM', str(ROOT.parent / '.thermistor-tools/user/libraries/RadioLib')))
CORE = Path(os.environ.get('APOLLO3_CORE', str(ROOT.parent / '.thermistor-tools/user/hardware/SparkFun/apollo3')))


def function(source, signature):
    start = source.index(signature)
    pos = source.index('{', start) + 1
    depth = 1
    while depth:
        if source[pos] == '{':
            depth += 1
        elif source[pos] == '}':
            depth -= 1
        pos += 1
    return source[start:pos] + '\n'


# Compile the actual published interfaces and Arduino HAL implementation.
# Only build options and the underlying Arduino/SPI/MCU hardware are faked.
ARDUINO = r'''
#pragma once
#include <cstdint>
#include <cstddef>
#include <cassert>
constexpr unsigned INPUT=0,OUTPUT=1,LOW=0,HIGH=1,RISING=2,FALLING=3,CHANGE=4;
void pinMode(uint32_t,uint32_t);void digitalWrite(uint32_t,uint32_t);
uint32_t digitalRead(uint32_t);void attachInterrupt(uint32_t,void(*)(),uint32_t);
void detachInterrupt(uint32_t);void delay(unsigned long);void delayMicroseconds(unsigned long);
unsigned long millis();unsigned long micros();long pulseIn(uint32_t,uint32_t,unsigned long);
void yield();
#define digitalPinToInterrupt(pin) (pin)
'''
SPI = r'''
#pragma once
#include "Arduino.h"
constexpr unsigned MSBFIRST=1,SPI_MODE0=0;
struct SPISettings{unsigned clock,order,mode;SPISettings(unsigned c=2000000,unsigned o=MSBFIRST,unsigned m=SPI_MODE0):clock(c),order(o),mode(m){}};
struct SPIClass{
 unsigned begins=0,ends=0,transactions=0;SPISettings last;
 void begin(){++begins;}void end(){++ends;}
 void beginTransaction(SPISettings s){++transactions;last=s;}
 uint8_t transfer(uint8_t value){return value^0xff;}void endTransaction(){}
};
extern SPIClass SPI,SPI1;
'''
BUILD = r'''
#pragma once
#include "Arduino.h"
#include "TypeDef.h"
#define RADIOLIB_BUILD_ARDUINO
#define RADIOLIB_GODMODE 0
#define RADIOLIB_TONE_UNSUPPORTED
#define RADIOLIB_NC 0xffffffffU
#define RADIOLIB_DEFAULT_SPI SPI
#define RADIOLIB_DEFAULT_SPI_SETTINGS SPISettings(2000000,MSBFIRST,SPI_MODE0)
#define RADIOLIB_ARDUINOHAL_PIN_MODE_CAST
#define RADIOLIB_ARDUINOHAL_PIN_STATUS_CAST
#define RADIOLIB_ARDUINOHAL_INTERRUPT_MODE_CAST
'''
AMB = r'''
#pragma once
#include <cstdint>
constexpr uint32_t AM_HAL_STATUS_SUCCESS=0,AM_HAL_STATUS_INVALID_ARG=6,AM_HAL_GPIO_MAX_PADS=50;
struct am_hal_gpio_pincfg_t{bool input,readable;};
extern const am_hal_gpio_pincfg_t g_AM_HAL_GPIO_INPUT;
uint32_t am_hal_gpio_interrupt_disable(uint64_t);
uint32_t am_hal_gpio_interrupt_clear(uint64_t);
uint32_t am_hal_gpio_interrupt_enable(uint64_t);
uint32_t am_hal_gpio_pinconfig(uint32_t,am_hal_gpio_pincfg_t);
'''
CRITICAL = r'''
#pragma once
void core_util_critical_section_enter();void core_util_critical_section_exit();
'''

irq_source = (CORE / 'cores/mbed-os/targets/TARGET_Ambiq_Micro/TARGET_Apollo3/device/gpio_irq_api.c').read_text()
irq_enable = function(irq_source, 'uint32_t ap3_gpio_enable_interrupts(uint32_t ui32Pin, am_hal_gpio_intdir_e eIntDir)\n    {')
irq_set = function(irq_source, 'void gpio_irq_set(gpio_irq_t *obj, gpio_irq_event event, uint32_t enable)')
# Verify that the Arduino fake reproduces the real callback-removal sequence.
arduino_irq = (CORE / 'cores/arduino/mbed-bridge/core-implement/CommonInterrupt.cpp').read_text()
assert 'irq->rise(NULL, NULL);\n    irq->fall(NULL, NULL);' in arduino_irq
assert 'gpio_irq_set(&gpio_irq, IRQ_RISE, 0);' in arduino_irq
assert 'gpio_irq_set(&gpio_irq, IRQ_FALL, 0);' in arduino_irq
sx_source = (UPSTREAM / 'src/modules/SX126x/SX126x.cpp').read_text()
tx_methods = ''.join(function(sx_source, sig) for sig in [
    'int16_t SX126x::transmit(', 'int16_t SX126x::finishTransmit()',
    'void SX126x::setDio1Action(', 'void SX126x::clearDio1Action()',
    'void SX126x::setPacketReceivedAction(', 'void SX126x::clearPacketReceivedAction()',
])

HARNESS = r'''
#include "hal_fixture.inc"
#include "am_mcu_apollo.h"
#include "platform/mbed_critical.h"
#include <algorithm>
#include <iostream>
#include <vector>
SPIClass SPI,SPI1;
uint32_t regs[7]={};uint64_t enabled=0,pending=0;unsigned critical=0,callbacks=0;
uint32_t ticks=0,txStart=0;bool transmitting=false,physicalDio=false,rfSent=false;
unsigned unrelatedAttaches=0,unrelatedDetaches=0;uint64_t touched=0;bool irqWasCreated=false,expectDio1Masked=false;
uint32_t injectedStatus=0;unsigned configCalls=0;uint32_t lastPad=0;
void (*handlers[50])()={};bool pinInput[50]={};
struct Registers{uint32_t PADKEY=0;} gpio;
#define GPIO (&gpio)
#define AM_REGADDR(block,reg) 0x1000U
uint32_t& regval(uint32_t a){assert(a>=0x1000 && (a-0x1000)/4<7);return regs[(a-0x1000)/4];}
#define AM_REGVAL(a) regval(a)
#define AM_CRITICAL_BEGIN
#define AM_CRITICAL_END
#define GPIOCFG_FLD_INTD_S 3
#define GPIOCFG_FLD_INCFG_S 0
#define GPIO_PADKEY_PADKEY_Key 0x73
void am_hal_gpio_output_tristate_disable(uint32_t){}
using am_hal_gpio_intdir_e=uint32_t;  // Preserve the C enum's integer semantics.
enum {AM_HAL_GPIO_PIN_INTDIR_LO2HI=0,AM_HAL_GPIO_PIN_INTDIR_HI2LO=1,AM_HAL_GPIO_PIN_INTDIR_NONE=2,AM_HAL_GPIO_PIN_INTDIR_BOTH=3};
enum gpio_irq_event{IRQ_NONE=0,IRQ_RISE=1,IRQ_FALL=2};
struct Control{uint32_t pad=40;unsigned events=0;} control;
struct gpio_irq_t{Control* control;} irq{&control};
uint32_t ap3_gpio_enable_interrupts(uint32_t,am_hal_gpio_intdir_e);
''' + irq_enable + irq_set + r'''
bool readable(uint32_t pin){return !(regs[pin/8] & (1U<<((pin%8)*4)));}
void core_util_critical_section_enter(){++critical;}
void core_util_critical_section_exit(){assert(critical);--critical;}
const am_hal_gpio_pincfg_t g_AM_HAL_GPIO_INPUT{true,true};
uint32_t am_hal_gpio_interrupt_disable(uint64_t mask){assert(critical);touched|=mask;enabled&=~mask;return injectedStatus;}
uint32_t am_hal_gpio_interrupt_clear(uint64_t mask){assert(critical);touched|=mask;pending&=~mask;return injectedStatus;}
uint32_t am_hal_gpio_interrupt_enable(uint64_t mask){assert(critical);touched|=mask;enabled|=mask;return injectedStatus;}
uint32_t am_hal_gpio_pinconfig(uint32_t pin,am_hal_gpio_pincfg_t cfg){
 assert(critical && pin==40 && cfg.input && cfg.readable);++configCalls;lastPad=pin;
 if(injectedStatus)return injectedStatus;
 pinInput[pin]=true;regs[pin/8]&=~(15U<<((pin%8)*4));return 0;
}
void pinMode(uint32_t pin,uint32_t mode){assert(critical==0);if(pin<50)pinInput[pin]=mode==INPUT;}
void digitalWrite(uint32_t,uint32_t){}
uint32_t digitalRead(uint32_t pin){return pin<50 && readable(pin) && physicalDio;}
void attachInterrupt(uint32_t pin,void(*cb)(),uint32_t mode){
 assert(critical==0); // Real Arduino registration can allocate and lock RTOS mutexes.
 if(pin!=40){++unrelatedAttaches;return;}
 if(expectDio1Masked)assert(!(enabled & (uint64_t(1)<<40)));
 // Matches indexAttachInterruptParam's detach followed by rise/fall setup.
 detachInterrupt(pin);handlers[pin]=cb;
 if(!irqWasCreated){irqWasCreated=true;enabled|=uint64_t(1)<<pin;}
 if(mode==RISING){gpio_irq_set(&irq,IRQ_RISE,1);gpio_irq_set(&irq,IRQ_FALL,0);}
 else if(mode==FALLING){gpio_irq_set(&irq,IRQ_FALL,1);gpio_irq_set(&irq,IRQ_RISE,0);}
 else{gpio_irq_set(&irq,IRQ_RISE,1);gpio_irq_set(&irq,IRQ_FALL,1);}
}
void detachInterrupt(uint32_t pin){
 assert(critical==0); // Keep stock callback teardown outside global critical sections.
 if(pin!=40){++unrelatedDetaches;return;}
 if(expectDio1Masked)assert(!(enabled & (uint64_t(1)<<40)));
 handlers[pin]=nullptr;gpio_irq_set(&irq,IRQ_RISE,0);gpio_irq_set(&irq,IRQ_FALL,0);
}
void delay(unsigned long ms){ticks+=ms;}void delayMicroseconds(unsigned long us){ticks+=us/1000;}
unsigned long millis(){return ticks;}unsigned long micros(){return ticks*1000;}
long pulseIn(uint32_t,uint32_t,unsigned long){return 0;}
void yield(){++ticks;if(transmitting && (ticks-txStart)*1000>=370688){physicalDio=true;rfSent=true;}}
void fireReceive(){
 pending|=uint64_t(1)<<40;
 if(!critical && (enabled & (uint64_t(1)<<40)) && handlers[40]){pending&=~(uint64_t(1)<<40);handlers[40]();}
}
#define RADIOLIB_SX126X_MAX_PACKET_LENGTH 255
#define RADIOLIB_SX126X_PACKET_TYPE_LR_FHSS 3
#define RADIOLIB_SX126X_IRQ_TX_DONE 1
#define RADIOLIB_ASSERT(x) do{if((x)!=0)return(x);}while(0)
#define RADIOLIB_DEBUG_BASIC_PRINTLN(...) ((void)0)
struct Module{RadioLibHal* hal;uint32_t getIrq(){return 40;}};
struct SX126x{
 Module* mod;float dataRateMeasured=0;unsigned lrFhssHopNum=0;
 explicit SX126x(Module* m):mod(m){}
 int16_t standby(){transmitting=false;return 0;}
 uint32_t getTimeOnAir(size_t){return 370688;}
 int16_t startTransmit(const uint8_t*,size_t,uint8_t){txStart=ticks;physicalDio=false;rfSent=false;transmitting=true;return 0;}
 uint8_t getPacketType(){return 1;}uint16_t getIrqFlags(){return 1;}
 void setLRFHSSHop(unsigned){}int16_t clearIrqStatus(){physicalDio=false;return 0;}
 int16_t transmit(const uint8_t*,size_t,uint8_t=0);int16_t finishTransmit();
 void setDio1Action(void(*)());void clearDio1Action();
 void setPacketReceivedAction(void(*)());void clearPacketReceivedAction();
};
''' + tx_methods + r'''
void received(){++callbacks;}
int main(){
 uint8_t data[24]={};ArduinoHal plain(SPI1);Module module{&plain};SX126x radio(&module);
 plain.pinMode(40,INPUT);assert(radio.transmit(data,24)==0);
 radio.setPacketReceivedAction(received);fireReceive();assert(callbacks==1);
 radio.clearPacketReceivedAction();assert(!readable(40));
 assert(radio.transmit(data,24)==RADIOLIB_ERR_TX_TIMEOUT && rfSent);
 std::cout<<"PASS: negative control reproduces post-RX timeout with stock Arduino HAL\n";
 Apollo3RadioHal fixed(SPI1,40,40);module.hal=&fixed;
 irqWasCreated=false;expectDio1Masked=true; // Exercise first allocation through the adapter too.
 fixed.init();fixed.pinMode(40,INPUT);
 assert(fixed.status()==0 && readable(40) && !(enabled & (uint64_t(1)<<40)));
 for(unsigned cycle=0;cycle<100;++cycle){
  assert(radio.transmit(data,24)==0 && rfSent);
  radio.setPacketReceivedAction(received);
  assert(readable(40) && (enabled & (uint64_t(1)<<40)));
  unsigned count=callbacks;fireReceive();assert(callbacks==count+1);
  radio.clearPacketReceivedAction();assert(readable(40) && !handlers[40]);
  count=callbacks;fireReceive();assert(callbacks==count && !(enabled & (uint64_t(1)<<40)));
  if(cycle%5==0){fixed.init();fixed.pinMode(40,INPUT);}
  assert(fixed.status()==0 && critical==0);
 }
 std::cout<<"PASS: 100 TX/RX/detach cycles and repeated initialization retain readable DIO1 and working RX\n";
 assert(touched==(uint64_t(1)<<40) && lastPad==40);
 fixed.attachInterrupt(40,nullptr,RISING);assert(readable(40) && !handlers[40]);
 fixed.attachInterrupt(40,received,FALLING);unsigned count=callbacks;fireReceive();assert(callbacks==count+1);
 fixed.detachInterrupt(40);assert(readable(40));
 auto before=regs[5];unsigned configs=configCalls;
 fixed.pinMode(39,INPUT);fixed.attachInterrupt(39,received,RISING);fixed.detachInterrupt(39);
 fixed.pinMode(RADIOLIB_NC,INPUT);fixed.attachInterrupt(RADIOLIB_NC,received,RISING);fixed.detachInterrupt(RADIOLIB_NC);
 assert(regs[5]==before && configCalls==configs && unrelatedAttaches==1 && unrelatedDetaches==1);
 fixed.attachInterrupt(40,received,RISING);count=callbacks;
 fixed.init();fixed.pinMode(40,INPUT);fireReceive();assert(callbacks==count && !(enabled & (uint64_t(1)<<40)));
 fixed.attachInterrupt(40,received,RISING);fireReceive();assert(callbacks==count+1);fixed.detachInterrupt(40);
 std::cout<<"PASS: callback management runs outside critical sections with DIO1 masked; unrelated and NC pins preserved\n";
 fixed.spiBeginTransaction();uint8_t out[]={1,2},in[2]={};fixed.spiTransfer(out,2,in);fixed.spiEndTransaction();
 assert(SPI1.transactions==1 && SPI.transactions==0 && SPI1.last.clock==2000000 && SPI1.last.mode==SPI_MODE0);
 assert(in[0]==254 && in[1]==253);fixed.delay(7);assert(fixed.millis()==ticks && fixed.micros()==ticks*1000);
 assert(SPI1.begins==0); // Explicit SPI constructor retains stock initialization behavior.
 std::cout<<"PASS: stock SPI1 selection, bus settings and timing delegation retained\n";
 injectedStatus=17;fixed.pinMode(40,INPUT);assert(fixed.status()==17);
 injectedStatus=0;fixed.pinMode(40,INPUT);assert(fixed.status()==17);
 fixed.init();fixed.pinMode(40,INPUT);assert(fixed.status()==0);
 Apollo3RadioHal invalid(SPI1,40,64);invalid.init();invalid.pinMode(40,INPUT);assert(invalid.status()==AM_HAL_STATUS_INVALID_ARG);
 configs=configCalls;invalid.attachInterrupt(40,received,RISING);assert(configCalls==configs && critical==0);
 std::cout<<"PASS: GPIO failures latch until initialization and invalid pads cannot cause invalid shifts\n";
}
'''

with tempfile.TemporaryDirectory(prefix='thermistor-radio-hal-') as tmp:
    folder = Path(tmp)
    for name in ['src/TypeDef.h', 'src/Hal.h', 'src/Hal.cpp',
                 'src/hal/Arduino/ArduinoHal.h', 'src/hal/Arduino/ArduinoHal.cpp']:
        dest = folder / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes((UPSTREAM / name).read_bytes())
    files = {'Arduino.h': ARDUINO, 'SPI.h': SPI, 'src/BuildOpt.h': BUILD,
             'src/BuildOptUser.h': '', 'RadioLib.h': '#pragma once\n#include "src/TypeDef.h"\n',
             'am_mcu_apollo.h': AMB, 'platform/mbed_critical.h': CRITICAL,
             'hal_fixture.inc': '#include <RadioLib.h>\n#include <hal/Arduino/ArduinoHal.h>\n#include <am_mcu_apollo.h>\n#include <platform/mbed_critical.h>\n' + (ROOT/'lorawan_service.cpp').read_text().split('// Private Apollo3 radio HAL.\n',1)[1].split('// Private LoRa rollover timing.\n',1)[0],
             'main.cpp': HARNESS}
    for name, content in files.items():
        dest = folder / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(content)
    flags = ['-std=c++11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-variable',
             '-I', str(folder), '-I', str(folder / 'src')]
    subprocess.run(['g++', *flags, '-x', 'c++', '-fsyntax-only', '-'],
                   input='#include "hal_fixture.inc"\n', text=True, check=True)
    print('PASS: private HAL implementation compiles independently against stock RadioLib interfaces', flush=True)
    sources = ['src/Hal.cpp', 'src/hal/Arduino/ArduinoHal.cpp', 'main.cpp']
    subprocess.run(['g++', *flags, *[str(folder / p) for p in sources], '-o', str(folder / 'check')], check=True)
    subprocess.run([str(folder / 'check')], check=True)
