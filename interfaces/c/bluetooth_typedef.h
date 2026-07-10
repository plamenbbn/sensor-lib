#ifndef BLUETOOTH_TYPEDEF_H
#define BLUETOOTH_TYPEDEF_H

/**
 * @file bluetooth_typedef.h
 * @brief C Typedefs for Bluetooth
 */

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

#include "rssi_typedef.h"

#include <stdint.h>

#define BLUETOOTH_MAX_ADAPTERS 10

#define BLUETOOTH_MAX_DEVICES 150

#define BLUETOOTH_MAX_PACKET_SIZE 23

typedef enum {
    MISCELLANEOUS = 0,
    COMPUTER      = 1,
    PHONE         = 2,
    LAN_ACCESS    = 3,
    AUDIO_VIDEO   = 4,
    PERIPHERAL    = 5,
    IMAGING       = 6,
    WEARABLE      = 7,
    TOY           = 8,
    UNCATEGORIZED = 63,
} EBluetoothDeviceMajor;

typedef struct {
    uint16_t id;
    char     mac_address[18];
    char     name[256];
    uint32_t flags;
    uint8_t  type;
} BluetoothAdapterInfoBase;

typedef struct {
    uint16_t              id;
    char                  mac_address[18];
    char                  name[256];
    uint8_t               dev_class[3];
    EBluetoothDeviceMajor major;
    BratislavaRssi        rssi;
} BluetoothDeviceInfoBase;

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // BLUETOOTH_TYPEDEF_H
