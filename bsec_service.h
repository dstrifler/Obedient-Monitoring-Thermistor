#ifndef BSEC_SERVICE_H
#define BSEC_SERVICE_H

#include "sensor_service.h"

// Start/poll run on the application thread. The worker owns all sensor/BSEC
// calls; snapshots cross threads under a mutex. Serial and flash stay on main.
void bsecStart();
void bsecPoll();
bool bsecLatest(SensorSnapshot* data);
bool bsecPresent();

#endif
