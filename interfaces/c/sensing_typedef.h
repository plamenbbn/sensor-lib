#ifndef SENSING_TYPEDEF_H
#define SENSING_TYPEDEF_H

#include "action_typedef.h"
#include "bluetooth_typedef.h"
#include "wifi_typedef.h"

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

#define DEVICE_LOST_IF_NOT_SEEN_IN_N_MILLISEC 3000

#define MAX_BUF_LEN 1350

/**
 * Categories of connection technology. In theory these should be buckets over
 * connection characteristics (strength, throughput, range) to be used for routing
 * decisions. In practice in our code, blue = wifi, green = bluetooth.
 */
typedef enum {
    LINK_TYPE_BLUE = 0, // Wifi
    LINK_TYPE_GREEN,    // Bluetooth
    LINK_TYPE_ORANGE,   // Unused
} BratislavaLinkType;

typedef struct {
    char               linkID[20];
    char               devID[256];
    uint32_t           latency;     // Never set. Do not use.
    uint32_t           throughput;  // Never set. Do not use.
    uint32_t           messageLoss; // Never set. Do not use.
    BratislavaLinkType linkType;
} BratislavaLinkInfo;

/**
 * BratislavaLink owns its instrument-specific payload inline via an anonymous
 * union, selected by `instrumentType`. Reading a member other than the active
 * one is undefined; producers must always set instrumentType before populating
 * the payload, and consumers must always switch on instrumentType before
 * accessing it.
 */
typedef struct {
    char               linkID[20];
    char               devID[256];
    uint32_t           latency;     // Never set. Do not use.
    uint32_t           throughput;  // Never set. Do not use.
    uint32_t           messageLoss; // Never set. Do not use.
    BratislavaLinkType linkType;
    InstrumentType     instrumentType;
    int                socketFd;

    union {
        BluetoothDeviceInfoBase bluetoothDeviceInfo;
        WifiDeviceInfoBase      wifiDeviceInfo;
    };
} BratislavaLink;

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // SENSING_TYPEDEF_H
