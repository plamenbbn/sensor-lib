#include "BratislavaSocket.h"
#include "instrument_api.h"
#include "bluetooth_typedef.h"
#include "callback_typedef.h"
#include "comms_typedef.h"
#include "gps_typedef.h"
#include "link_callback_harness.h"
#include "sensing_typedef.h"
#include "wifi_typedef.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char* display_bt_name(const char* const name) {
    return ((name != NULL) && (name[0] != '\0')) ? name : "<NONE>";
}

static void usage(const char* const argv0) {
    fprintf(stderr,
            "Usage: %s bluetooth|wifi|gps|comms|link-callback [--wifi-mode client|hotspot]\n",
            argv0);
}

static bool parse_wifi_mode(int argc, char** argv, HarnessWifiMode* const wifi_mode) {
    *wifi_mode = HARNESS_WIFI_MODE_CLIENT;
    if (argc == 2) {
        return true;
    }

    if ((argc == 4) && (strcmp(argv[2], "--wifi-mode") == 0)) {
        if (strcmp(argv[3], "client") == 0) {
            *wifi_mode = HARNESS_WIFI_MODE_CLIENT;
            return true;
        }
        if ((strcmp(argv[3], "hotspot") == 0) || (strcmp(argv[3], "wap") == 0)) {
            *wifi_mode = HARNESS_WIFI_MODE_HOTSPOT;
            return true;
        }
    }

    return false;
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
               display_bt_name(adapters[i].name),
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
               display_bt_name(devices[i].name),
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

static void log_link_discovered(const BratislavaLink* link) {
    BratislavaSocket* const bsock = bratislavaSocket(*link);
    printf("Link discovered: id=%s dev=%s type=%u instrument=%u bsock=%p\n",
           link->linkID,
           link->devID,
           (unsigned)link->linkType,
           (unsigned)link->instrumentType,
           (void*)bsock);
}

static int run_comms(const InstrumentAPI* const api) {
    LinkDiscoveredCallback callback = log_link_discovered;
    instrument_api_status_t status =
        api->registerCallback(INSTRUMENT_COMMS, LINK_DISCOVERED, (InstrumentInputType)&callback);
    if (status != INSTRUMENT_API_SUCCESS) {
        fprintf(stderr, "Comms callback registration failed: %d\n", status);
        return 1;
    }

    printf("Waiting 10 seconds for Wi-Fi/Bluetooth link handshakes...\n");
    sleep(10);

    BratislavaLink* links[COMMS_MAX_LINKS];
    uint32_t link_count = 0U;
    status = api->instrumentAction(INSTRUMENT_COMMS_DISCOVER_BRATISLAVA_LINKS, NULL, 0U, links, &link_count);
    if (status != INSTRUMENT_API_SUCCESS) {
        fprintf(stderr, "Comms link discovery failed: %d\n", status);
        api->unregisterCallback(INSTRUMENT_COMMS, LINK_DISCOVERED, (InstrumentInputType)&callback);
        return 1;
    }

    printf("Active comms links: %" PRIu32 "\n", link_count);
    for (uint32_t i = 0U; i < link_count; ++i) {
        BratislavaSocket* const bsock = bratislavaSocket(*links[i]);
        printf("  [%u] id=%s dev=%s instrument=%u bsock=%p\n",
               i,
               links[i]->linkID,
               links[i]->devID,
               (unsigned)links[i]->instrumentType,
               (void*)bsock);
    }

    api->unregisterCallback(INSTRUMENT_COMMS, LINK_DISCOVERED, (InstrumentInputType)&callback);
    return 0;
}

int main(int argc, char** argv) {
    HarnessWifiMode wifi_mode;
    if (!parse_wifi_mode(argc, argv, &wifi_mode)) {
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
    } else if (strcmp(argv[1], "comms") == 0) {
        rc = run_comms(api);
    } else if (strcmp(argv[1], "link-callback") == 0) {
        rc = run_link_callback_harness(api, 2U, wifi_mode);
    } else {
        usage(argv[0]);
    }

    destroyInstrumentAPI(api);
    return rc;
}
