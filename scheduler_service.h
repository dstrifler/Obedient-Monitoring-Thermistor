#ifndef SCHEDULER_SERVICE_H
#define SCHEDULER_SERVICE_H

#include "common.h"
#include "lorawan_service.h"

static constexpr uint32_t SCHEDULER_MIN_MS = 60000UL;
static constexpr uint32_t SCHEDULER_BASE_MS = 360000UL;
static constexpr uint32_t SCHEDULER_MAX_MS = 3600000UL;
struct DeliveryEvent { ReportKind kind; RadioTransaction transaction; };

void schedulerStart();
// Lightweight storage notification, safe from the acquisition worker.
void schedulerNotifyRecord();
void schedulerSettingsChanged(const AppSettings& before, const AppSettings& after);
uint32_t schedulerIntervalMs();
bool schedulerLearningComplete();
bool schedulerFirstSensorSent();
// Only scheduler initiates encoding/transmission. Returns a completed event.
bool schedulerPoll(const AppSettings& settings, const ControlReply* control,
                   const NodeStatus& status, DeliveryEvent* event);

#endif
