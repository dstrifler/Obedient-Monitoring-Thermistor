#ifndef FIRMWARE_FEATURES_H
#define FIRMWARE_FEATURES_H


// Sensor acquisition feeds Serial and the RAM ring; scheduler selects v3 reports.
// Set to 0 for a radio-only build without the Bosch sensor dependency.
#ifndef SENSOR_SERVICE_ENABLED
#define SENSOR_SERVICE_ENABLED 1
#endif
static_assert(SENSOR_SERVICE_ENABLED == 0 || SENSOR_SERVICE_ENABLED == 1,
              "SENSOR_SERVICE_ENABLED must be 0 or 1");

// Set to 0 for the original forced-mode sensor backend without BSEC.
#ifndef SENSOR_BSEC_ENABLED
#define SENSOR_BSEC_ENABLED 1
#endif
static_assert(SENSOR_BSEC_ENABLED == 0 || SENSOR_BSEC_ENABLED == 1,
              "SENSOR_BSEC_ENABLED must be 0 or 1");

#endif
