#ifndef GPS_TYPEDEF_H
#define GPS_TYPEDEF_H

#include <time.h>

/**
 * @file gps_typedef.h
 * @brief C Typedefs for GPS.
 */

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

typedef enum {
    MODE_NOT_SEEN = 0,
    MODE_NO_FIX   = 1,
    MODE_2D       = 2,
    MODE_3D       = 3,
} gps_mode_t;

typedef struct {
    long double lat;
    long double lon;
    long double alt;
} C_LLA;

typedef struct {
    C_LLA      position;
    time_t     timestamp;
    gps_mode_t mode;
} C_PositionInfo;

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // GPS_TYPEDEF_H