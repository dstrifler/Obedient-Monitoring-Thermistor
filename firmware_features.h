#ifndef FIRMWARE_FEATURES_H
#define FIRMWARE_FEATURES_H


// The radio beta does not acquire sensor readings. Keep the retained I2C
// backend out of the build until sensor development resumes.
#ifndef SENSOR_SERVICE_ENABLED
#define SENSOR_SERVICE_ENABLED 0
#endif
static_assert(SENSOR_SERVICE_ENABLED == 0 || SENSOR_SERVICE_ENABLED == 1,
              "SENSOR_SERVICE_ENABLED must be 0 or 1");

#endif
