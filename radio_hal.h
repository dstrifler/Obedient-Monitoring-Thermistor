#ifndef RADIO_HAL_H
#define RADIO_HAL_H

#include <RadioLib.h>
#include <hal/Arduino/ArduinoHal.h>

// Firmware-owned Apollo3 GPIO handling through RadioLib's public HAL interface.
// All SPI, timing and radio protocol operations remain in stock RadioLib.
class Apollo3RadioHal : public ArduinoHal {
public:
  Apollo3RadioHal(SPIClass& spi, uint32_t dio1Pin, uint32_t dio1Pad);
  void init() override;
  void pinMode(uint32_t pin, uint32_t mode) override;
  void attachInterrupt(uint32_t interruptNum, void (*callback)(void), uint32_t mode) override;
  void detachInterrupt(uint32_t interruptNum) override;
  uint32_t status() const;

private:
  void recordStatus(uint32_t result);
  bool maskDio1Interrupt();  // Masks just DIO1; releases the global critical section.
  void restoreDio1Polling();  // Caller holds the critical section.
  const uint32_t dio1Pin_;
  const uint32_t dio1Pad_;
  uint32_t status_ = 0;
};

#endif
