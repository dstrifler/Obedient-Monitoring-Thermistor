#ifndef FIRMWARE_FEATURES_H
#define FIRMWARE_FEATURES_H


// BME688 readings go to Serial and fixed 11-byte environmental uplinks.
// Set to 0 for a radio-only build without the Bosch sensor dependency.
#ifndef SENSOR_SERVICE_ENABLED
#define SENSOR_SERVICE_ENABLED 1
#endif
static_assert(SENSOR_SERVICE_ENABLED == 0 || SENSOR_SERVICE_ENABLED == 1,
              "SENSOR_SERVICE_ENABLED must be 0 or 1");

#endif
