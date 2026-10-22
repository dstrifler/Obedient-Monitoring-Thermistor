#ifndef LOG_CONFIG_H
#define LOG_CONFIG_H

#define FW_LOG_LEVEL_OFF   0
#define FW_LOG_LEVEL_ERROR 1
#define FW_LOG_LEVEL_WARN  2
#define FW_LOG_LEVEL_INFO  3
#define FW_LOG_LEVEL_DEBUG 4
#define FW_LOG_LEVEL_TRACE 5

// Serial output only. Change this default or override FW_LOG_LEVEL at build
// time. A level includes all more severe messages; OFF disables serial output.
#ifndef FW_LOG_LEVEL
#define FW_LOG_LEVEL FW_LOG_LEVEL_INFO
#endif

#if FW_LOG_LEVEL < FW_LOG_LEVEL_OFF || FW_LOG_LEVEL > FW_LOG_LEVEL_TRACE
#error "FW_LOG_LEVEL must be OFF, ERROR, WARN, INFO, DEBUG or TRACE (0..5)"
#endif

#endif
