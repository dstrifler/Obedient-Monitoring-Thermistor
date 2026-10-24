#ifndef BSEC_STATE_H
#define BSEC_STATE_H
#include <stddef.h>
#include <stdint.h>

static constexpr size_t BSEC_STATE_MAX_SIZE = 238;
// Application calibration lives at EEPROM offset 128, outside settings and
// ignored legacy bytes. LoRaWAN nonces remain in their independent journal.
uint16_t bsecStateLoad(uint8_t* state, size_t capacity, uint32_t profile);
bool bsecStateSave(const uint8_t* state, uint16_t size, uint32_t profile);
#endif
