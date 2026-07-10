#ifndef WIFI_TYPEDEF_H
#define WIFI_TYPEDEF_H

/**
 * @file wifi_typedef.h
 * @brief C Typedefs for WiFi
 */

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

#define WIFI_MAX_DEVICES 150

#define WIFI_MAX_PACKET_SIZE 50

#include "rssi_typedef.h"

#include <stdint.h>

typedef enum {
    WIFI_UNKNOWN = 0,
    WIFI_ADAPTER,
    WIFI_ROUTER,
} WifiDeviceType;

typedef struct {
    char           ssid[256];
    char           mac_address[18];
    WifiDeviceType type;
    BratislavaRssi rssi;
} WifiDeviceInfoBase;

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // CELLULAR_TYPEDEF_H
