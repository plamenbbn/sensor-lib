#include "bluetooth_scanner.h"

#include "bluetooth_typedef.h"

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void copy_string(char* const dst, const size_t dst_size, const char* const src) {
    if ((dst == NULL) || (dst_size == 0U)) {
        return;
    }

    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    strncpy(dst, src, dst_size - 1U);
    dst[dst_size - 1U] = '\0';
}

static EBluetoothDeviceMajor bluetooth_major_from_class(const uint8_t dev_class[3]) {
    const uint32_t class_of_device =
        ((uint32_t)dev_class[2] << 16U) | ((uint32_t)dev_class[1] << 8U) | (uint32_t)dev_class[0];

    switch ((class_of_device >> 8U) & 0x1FU) {
    case 0x00:
        return MISCELLANEOUS;
    case 0x01:
        return COMPUTER;
    case 0x02:
        return PHONE;
    case 0x03:
        return LAN_ACCESS;
    case 0x04:
        return AUDIO_VIDEO;
    case 0x05:
        return PERIPHERAL;
    case 0x06:
        return IMAGING;
    case 0x07:
        return WEARABLE;
    case 0x08:
        return TOY;
    default:
        return UNCATEGORIZED;
    }
}

instrument_api_status_t bluetooth_discover_devices_backend(const InstrumentInputType input_data,
                                                           const uint32_t            input_len,
                                                           InstrumentOutputType      output_data,
                                                           uint32_t* const           output_len) {
    if ((output_data == NULL) || (output_len == NULL)) {
        return INSTRUMENT_API_ERROR;
    }

    int dev_id = hci_get_route(NULL);
    if ((input_data != NULL) && (input_len > 0U)) {
        dev_id = ((const BluetoothAdapterInfoBase*)input_data)->id;
    }
    if (dev_id < 0) {
        return INSTRUMENT_API_NOT_SUPPORTED;
    }

    inquiry_info* results = NULL;
    const int num_results = hci_inquiry(dev_id, 4, BLUETOOTH_MAX_DEVICES, NULL, &results, IREQ_CACHE_FLUSH);
    if (num_results < 0) {
        return INSTRUMENT_API_ERROR;
    }

    const int dev_fd = hci_open_dev(dev_id);
    BluetoothDeviceInfoBase* const devices = (BluetoothDeviceInfoBase*)output_data;
    uint32_t                       count   = 0U;

    for (int i = 0; (i < num_results) && (count < BLUETOOTH_MAX_DEVICES); ++i) {
        memset(&devices[count], 0, sizeof(devices[count]));
        devices[count].id = (uint16_t)i;
        ba2str(&results[i].bdaddr, devices[count].mac_address);
        memcpy(devices[count].dev_class, results[i].dev_class, sizeof(devices[count].dev_class));
        devices[count].major = bluetooth_major_from_class(results[i].dev_class);
        devices[count].rssi.valid = false;

        if (dev_fd >= 0) {
            char name[sizeof(devices[count].name)];
            memset(name, 0, sizeof(name));
            if (hci_read_remote_name(dev_fd, &results[i].bdaddr, sizeof(name), name, 0) == 0) {
                copy_string(devices[count].name, sizeof(devices[count].name), name);
            }
        }

        ++count;
    }

    if (dev_fd >= 0) {
        hci_close_dev(dev_fd);
    }
    free(results);

    *output_len = count;
    return INSTRUMENT_API_SUCCESS;
}
