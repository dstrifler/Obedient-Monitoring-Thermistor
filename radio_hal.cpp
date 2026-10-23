#include "radio_hal.h"
#include "am_mcu_apollo.h"
#include <platform/mbed_critical.h>

Apollo3RadioHal::Apollo3RadioHal(SPIClass& spi, uint32_t dio1Pin, uint32_t dio1Pad)
  : ArduinoHal(spi), dio1Pin_(dio1Pin), dio1Pad_(dio1Pad) {}

void Apollo3RadioHal::init() {
  status_ = AM_HAL_STATUS_SUCCESS;
  ArduinoHal::init();
}

void Apollo3RadioHal::recordStatus(uint32_t result) {
  if(status_ == AM_HAL_STATUS_SUCCESS && result != AM_HAL_STATUS_SUCCESS) {
    status_ = result;
  }
}

uint32_t Apollo3RadioHal::status() const { return status_; }

bool Apollo3RadioHal::maskDio1Interrupt() {
  if(dio1Pad_ >= AM_HAL_GPIO_MAX_PADS) {
    recordStatus(AM_HAL_STATUS_INVALID_ARG);
    return false;
  }
  const uint64_t mask = uint64_t(1) << dio1Pad_;
  core_util_critical_section_enter();
  recordStatus(am_hal_gpio_interrupt_disable(mask));
  recordStatus(am_hal_gpio_interrupt_clear(mask));
  core_util_critical_section_exit();
  return true;
}

void Apollo3RadioHal::restoreDio1Polling() {
  if(dio1Pad_ >= AM_HAL_GPIO_MAX_PADS) {
    recordStatus(AM_HAL_STATUS_INVALID_ARG);
    return;
  }
  const uint64_t mask = uint64_t(1) << dio1Pad_;
  recordStatus(am_hal_gpio_interrupt_disable(mask));
  // Apollo3 INTDIR_NONE forces input reads to zero. A complete input config
  // explicitly restores READPIN; ordinary Arduino pinMode(INPUT) need not.
  // Leave the hardware IRQ masked while no receive callback is registered.
  recordStatus(am_hal_gpio_pinconfig(dio1Pad_, g_AM_HAL_GPIO_INPUT));
  recordStatus(am_hal_gpio_interrupt_clear(mask));
}

void Apollo3RadioHal::pinMode(uint32_t pin, uint32_t mode) {
  ArduinoHal::pinMode(pin, mode);
  if(pin == dio1Pin_ && mode == GpioModeInput) {
    core_util_critical_section_enter();
    restoreDio1Polling();
    core_util_critical_section_exit();
  }
}

void Apollo3RadioHal::detachInterrupt(uint32_t interruptNum) {
  if(interruptNum != pinToInterrupt(dio1Pin_)) {
    ArduinoHal::detachInterrupt(interruptNum);
    return;
  }
  if(!maskDio1Interrupt()) { return; }
  // Arduino callback management may allocate or lock an RTOS mutex. Keep it
  // outside global critical sections, with just the DIO1 hardware IRQ masked.
  ArduinoHal::detachInterrupt(interruptNum);
  core_util_critical_section_enter();
  restoreDio1Polling();
  core_util_critical_section_exit();
}

void Apollo3RadioHal::attachInterrupt(uint32_t interruptNum, void (*callback)(void), uint32_t mode) {
  if(interruptNum != pinToInterrupt(dio1Pin_)) {
    ArduinoHal::attachInterrupt(interruptNum, callback, mode);
    return;
  }
  if(callback == nullptr) {
    detachInterrupt(interruptNum);
    return;
  }
  if(!maskDio1Interrupt()) { return; }
  // In particular, the first attach constructs InterruptIn and can allocate.
  ArduinoHal::attachInterrupt(interruptNum, callback, mode);
  const uint64_t mask = uint64_t(1) << dio1Pad_;
  core_util_critical_section_enter();
  recordStatus(am_hal_gpio_interrupt_clear(mask));
  if(status_ == AM_HAL_STATUS_SUCCESS) {
    recordStatus(am_hal_gpio_interrupt_enable(mask));
  }
  core_util_critical_section_exit();
}
