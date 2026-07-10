#ifndef RSSI_TYPEDEF_H
#define RSSI_TYPEDEF_H

/**
 * @file rssi_typedef.h
 * @brief Standalone RSSI typedef shared by every transport-layer device-info
 *        struct (BluetoothDeviceInfoBase, WifiDeviceInfoBase, ...).
 *
 * Lives on its own to break a header dependency cycle: BratislavaLink in
 * sensing_typedef.h owns the device-info structs by value (via an inline
 * union), and those structs need BratislavaRssi -- so neither side can
 * include the other.
 */

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

#include <stdbool.h>
#include <stdint.h>

/**
 * RSSI measurement in dBm, with an explicit validity flag.
 * Physical values range from -120 to -30 dBm.
 */
typedef struct {
    int16_t value_dbm;
    bool    valid;
} BratislavaRssi;

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // RSSI_TYPEDEF_H
