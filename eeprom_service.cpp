#include "eeprom_service.h"
#include <EEPROM.h>

bool eepromBootstrap()
{
  static bool isInitialized = false;
  if(isInitialized) {
    return true;
  }

  if(EEPROM.init() != 0) {
    return false;
  }
  EEPROM.setLength(EEPROM_TOTAL_BYTES);
  isInitialized = true;
  return true;
}
