#include "instrument_api.h"

#include "bluetooth_typedef.h"
#include "gps_typedef.h"
#include "wifi_typedef.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char* const argv0) {
    fprintf(stderr, "Usage: %s bluetooth|wifi|gps\n", argv0);
}

static int run_bluetooth(const InstrumentAPI* const api) {
    BluetoothAdapterInfoBase adapters[BLUETOOTH_MAX_ADAPTERS];
    BluetoothDeviceInfoBase  devices[BLUETOOTH_MAX_DEVICES];
    uint32_t                 adapter_count = 0U;
    uint32_t                 device_count  = 0U;

    const instrument_api_status_t adapter_status = api->instrumentAction(
        INSTRUMENT_BLUETOOTH_GET_ADAPTER_INFO, NULL, 0U, adapters, &adapter_count);
    if (adapter_status != INSTRUMENT_API_SUCCESS) {
        fprintf(stderr, "Bluetooth adapter query failed: %d\n", adapter_status);
        return 1;
    }

    printf("Bluetooth adapters: %u\n", adapter_count);
    for (uint32_t i = 0; i < adapter_count; ++i) {
        printf("  [%u] id=%u mac=%s name=%s flags=0x%08X type=%u\n",
               i,
               (unsigned)adapters[i].id,
               adapters[i].mac_address,
               adapters[i].name,
               adapters[i].flags,
               adapters[i].type);
    }

    if (adapter_count == 0U) {
        return 0;
    }

    const instrument_api_status_t scan_status = api->instrumentAction(
        INSTRUMENT_BLUETOOTH_DISCOVER_DEVICES, &adapters[0], 1U, devices, &device_count);
    if (scan_status != INSTRUMENT_API_SUCCESS) {
        fprintf(stderr, "Bluetooth scan failed: %d\n", scan_status);
        return 1;
    }

    printf("Bluetooth discoveries: %u\n", device_count);
    for (uint32_t i = 0; i < device_count; ++i) {
        printf("  [%u] mac=%s name=%s class=%u/%u/%u major=%u\n",
               i,
               devices[i].mac_address,
               devices[i].name,
               devices[i].dev_class[0],
               devices[i].dev_class[1],
               devices[i].dev_class[2],
               devices[i].major);
    }

    return 0;
}

static int run_wifi(const InstrumentAPI* const api) {
    WifiDeviceInfoBase devices[WIFI_MAX_DEVICES];
    uint32_t           my_count     = 0U;
    uint32_t           device_count = 0U;

    const instrument_api_status_t my_status =
        api->instrumentAction(INSTRUMENT_WIFI_GET_MY_DEVICE, NULL, 0U, devices, &my_count);
    if (my_status != INSTRUMENT_API_SUCCESS) {
        fprintf(stderr, "Wi-Fi adapter query failed: %d\n", my_status);
        return 1;
    }

    printf("Wi-Fi adapters: %u\n", my_count);
    for (uint32_t i = 0; i < my_count; ++i) {
        printf("  [%u] mac=%s ssid=%s type=%u\n", i, devices[i].mac_address, devices[i].ssid, devices[i].type);
    }

    const instrument_api_status_t scan_status =
        api->instrumentAction(INSTRUMENT_WIFI_DISCOVER_DEVICES, NULL, 0U, devices, &device_count);
    if (scan_status != INSTRUMENT_API_SUCCESS) {
        fprintf(stderr, "Wi-Fi scan failed: %d\n", scan_status);
        return 1;
    }

    printf("Wi-Fi discoveries: %u\n", device_count);
    for (uint32_t i = 0; i < device_count; ++i) {
        printf("  [%u] mac=%s ssid=%s rssi_valid=%d rssi_dbm=%d\n",
               i,
               devices[i].mac_address,
               devices[i].ssid,
               devices[i].rssi.valid ? 1 : 0,
               devices[i].rssi.value_dbm);
    }

    return 0;
}

static int run_gps(const InstrumentAPI* const api) {
    C_PositionInfo position;
    uint32_t       count = 0U;

    const instrument_api_status_t status =
        api->instrumentAction(INSTRUMENT_GPS_GET_POSITION, NULL, 0U, &position, &count);
    if (status != INSTRUMENT_API_SUCCESS) {
        fprintf(stderr, "GPS poll failed: %d\n", status);
        return 1;
    }

    printf("GPS count: %u\n", count);
    printf("GPS mode: %d\n", (int)position.mode);
    printf("GPS timestamp: %lld\n", (long long)position.timestamp);
    printf("GPS lat/lon/alt: %.10Lf %.10Lf %.3Lf\n",
           position.position.lat,
           position.position.lon,
           position.position.alt);

    return 0;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        usage(argv[0]);
        return 1;
    }

    const InstrumentAPIConfig config = {0};
    InstrumentAPI* const api = createInstrumentAPI(config);
    if (api == NULL) {
        fprintf(stderr, "Failed to create InstrumentAPI\n");
        return 1;
    }

    int rc = 1;
    if (strcmp(argv[1], "bluetooth") == 0) {
        rc = run_bluetooth(api);
    } else if (strcmp(argv[1], "wifi") == 0) {
        rc = run_wifi(api);
    } else if (strcmp(argv[1], "gps") == 0) {
        rc = run_gps(api);
    } else {
        usage(argv[0]);
    }

    destroyInstrumentAPI(api);
    return rc;
}
