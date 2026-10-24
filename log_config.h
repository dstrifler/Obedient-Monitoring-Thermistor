#ifndef LOG_CONFIG_H
#define LOG_CONFIG_H

#define FW_LOG_LEVEL_OFF   0
#define FW_LOG_LEVEL_ERROR 1
#define FW_LOG_LEVEL_WARN  2
#define FW_LOG_LEVEL_INFO  3
#define FW_LOG_LEVEL_DEBUG 4
#define FW_LOG_LEVEL_TRACE 5

// Build-time ceiling; runtime verbosity is an OTA setting (factory INFO).
// Compile TRACE by default so every documented runtime level remains available.
#ifndef FW_LOG_LEVEL
#define FW_LOG_LEVEL FW_LOG_LEVEL_TRACE
#endif

#if FW_LOG_LEVEL < FW_LOG_LEVEL_OFF || FW_LOG_LEVEL > FW_LOG_LEVEL_TRACE
#error "FW_LOG_LEVEL must be OFF, ERROR, WARN, INFO, DEBUG or TRACE (0..5)"
#endif

#endif
