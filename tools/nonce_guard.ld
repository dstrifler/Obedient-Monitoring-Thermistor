/* Additional linker script for Arduino IDE builds. The core still supplies
 * the memory layout. Keep firmware below the two nonce pages and EEPROM. */
ASSERT(LOADADDR(.data) + SIZEOF(.data) <= 0xFA000,
       "Firmware overlaps nonce journal")
