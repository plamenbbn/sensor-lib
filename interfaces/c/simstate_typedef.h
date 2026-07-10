#ifndef SIMSTATE_TYPEDEF_H
#define SIMSTATE_TYPEDEF_H

#include <stdbool.h>
#include <time.h>

/**
 * @file simstate_typedef.h
 * @brief C Typedefs for Simulation State.
 */

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

typedef struct {
    time_t startTime;
    time_t clockTime;
    float  playRate;
    bool   paused;
} C_SimState;

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // SIMSTATE_TYPEDEF_H