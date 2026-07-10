#ifndef LOGGER_TYPEDEF_H
#define LOGGER_TYPEDEF_H

/**
 * @file logger_typedef.h
 * @brief Minimal logger typedefs used by the C Instrument API.
 */

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

typedef enum {
    ERROR = 0,
    WARNING,
    INFO,
    DEBUG,
} ELoggerLevel;

typedef enum {
    LOG_STDERR = 0,
    LOG_STDOUT,
    LOG_FILE,
} ELoggerOutput;

typedef struct {
    ELoggerLevel  level;
    ELoggerOutput output;
    const char*   destination;
} LoggerConfig;

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // LOGGER_TYPEDEF_H
