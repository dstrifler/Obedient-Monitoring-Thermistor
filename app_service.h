#ifndef APP_SERVICE_H
#define APP_SERVICE_H

#include "app_settings.h"

// Main-thread lifecycle and command management. Sensor acquisition runs in its
// own worker; scheduler alone initiates the encoder -> LoRa delivery chain.
void appSetup();
void appLoop();
const AppSettings& appSettings();

#endif
