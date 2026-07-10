#ifndef CALLBACK_TYPEDEF_H
#define CALLBACK_TYPEDEF_H

/**
 * @file callback_typedef.h
 * @brief C Typedefs for API callbacks
 */

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

#include "bluetooth_typedef.h"
#include "sensing_typedef.h"
#include "wifi_typedef.h"

typedef enum {
    DEVICE_DISCOVERED = 0,
    LINK_DISCOVERED,
    LINK_DROPPED,
} CallbackType;

// Callback types
typedef void (*BluetoothDeviceDiscoveredCallback)(const BluetoothDeviceInfoBase*);
typedef void (*BluetoothLinkDiscoveredCallback)(const BratislavaLink*);
typedef void (*WifiDeviceDiscoveredCallback)(const WifiDeviceInfoBase*);
typedef void (*WifiLinkDiscoveredCallback)(const BratislavaLink*);
typedef void (*LinkDiscoveredCallback)(const BratislavaLink*);
typedef void (*LinkDroppedCallback)(const BratislavaLinkInfo*);

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // CALLBACK_TYPEDEF_H
