#ifndef ENCODER_SERVICE_H
#define ENCODER_SERVICE_H

#include "common.h"
#include "lorawan_service.h"

// Codec functions are pure; the send entry point completes the scheduler's
// encoder -> LoRa handoff. Application downlinks are decoded after Class A ends.
bool encoderDecode(const uint8_t* bytes, size_t size, DecodedCommand* command);
size_t encoderSensorPacket(uint8_t* bytes, size_t capacity,
                          const SensorSnapshot& sample, const AppSettings& settings);
size_t encoderControlPacket(uint8_t* bytes, size_t capacity, const ControlReply& reply);
RadioTransaction encoderTransmit(ReportKind kind, const AppSettings& settings,
                                 const ControlReply* reply, const NodeStatus& status,
                                 bool requireComplete);

#endif
