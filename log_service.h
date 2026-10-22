#ifndef LOG_SERVICE_H
#define LOG_SERVICE_H

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>
#include "log_config.h"

enum class LogLevel : uint8_t { OFF, ERROR, WARN, INFO, DEBUG, TRACE };

// Main-thread service: no logging/pumping from interrupts or other threads.
// This service exclusively owns Serial. Messages live only in bounded RAM.
void logBegin(unsigned long baud);
void logPump();
void logServiceDelay(uint32_t waitMs);
uint32_t logDroppedCount(LogLevel level);
void logWrite(LogLevel level, const char* module, const char* format, ...)
  __attribute__((format(printf, 3, 4)));
void logHex(LogLevel level, const char* module, const uint8_t* data, size_t len);

#if FW_LOG_LEVEL >= FW_LOG_LEVEL_ERROR
#define LOG_ERROR(module, ...) do { logWrite(LogLevel::ERROR, module, __VA_ARGS__); } while(0)
#else
#define LOG_ERROR(...) do {} while(0)
#endif
#if FW_LOG_LEVEL >= FW_LOG_LEVEL_WARN
#define LOG_WARN(module, ...) do { logWrite(LogLevel::WARN, module, __VA_ARGS__); } while(0)
#else
#define LOG_WARN(...) do {} while(0)
#endif
#if FW_LOG_LEVEL >= FW_LOG_LEVEL_INFO
#define LOG_INFO(module, ...) do { logWrite(LogLevel::INFO, module, __VA_ARGS__); } while(0)
#else
#define LOG_INFO(...) do {} while(0)
#endif
#if FW_LOG_LEVEL >= FW_LOG_LEVEL_DEBUG
#define LOG_DEBUG(module, ...) do { logWrite(LogLevel::DEBUG, module, __VA_ARGS__); } while(0)
#else
#define LOG_DEBUG(...) do {} while(0)
#endif
#if FW_LOG_LEVEL >= FW_LOG_LEVEL_TRACE
#define LOG_TRACE(module, ...) do { logWrite(LogLevel::TRACE, module, __VA_ARGS__); } while(0)
#define LOG_TRACE_HEX(module, data, len) do { logHex(LogLevel::TRACE, module, data, len); } while(0)
#else
#define LOG_TRACE(...) do {} while(0)
#define LOG_TRACE_HEX(...) do {} while(0)
#endif

#endif
