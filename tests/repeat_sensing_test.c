#include "instrument_api.h"

#include "bluetooth_typedef.h"
#include "gps_typedef.h"
#include "wifi_typedef.h"

#include <errno.h>
#include <inttypes.h>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_ITERATIONS 20U
#define ITERATION_DELAY_SECONDS 2U
#define LEAK_ALLOWANCE_BYTES 1024U

typedef struct {
    bool     supported;
    uint64_t bytes_in_use;
} LeakSnapshot;

typedef struct {
    char   mac_address[18];
    char   name[256];
    time_t first_seen;
} BluetoothDiscoveryRecord;

typedef struct {
    char   mac_address[18];
    char   ssid[256];
    time_t first_seen;
} WifiDiscoveryRecord;

typedef struct {
    BluetoothDiscoveryRecord bluetooth[BLUETOOTH_MAX_DEVICES];
    uint32_t                 bluetooth_count;
    WifiDiscoveryRecord      wifi[WIFI_MAX_DEVICES];
    uint32_t                 wifi_count;
} DiscoverySummary;

static void usage(const char* const argv0) {
    fprintf(stderr, "Usage: %s [iterations]\n", argv0);
}

static bool parse_iterations(const char* const arg, uint32_t* const iterations_out) {
    if ((arg == NULL) || (iterations_out == NULL)) {
        return false;
    }

    errno = 0;
    char* end = NULL;
    const unsigned long parsed = strtoul(arg, &end, 10);
    if ((errno != 0) || (end == arg) || (*end != '\0') || (parsed == 0UL) || (parsed > UINT32_MAX)) {
        return false;
    }

    *iterations_out = (uint32_t)parsed;
    return true;
}

static void sleep_between_iterations(void) {
    const struct timespec delay = {
        .tv_sec = ITERATION_DELAY_SECONDS,
        .tv_nsec = 0L,
    };

    while (nanosleep(&delay, NULL) != 0) {
        if (errno != EINTR) {
            break;
        }
    }
}

static LeakSnapshot capture_leak_snapshot(void) {
    LeakSnapshot snapshot = {0};

#if defined(__GLIBC__)
    malloc_trim(0);
    const struct mallinfo2 info = mallinfo2();
    snapshot.supported = true;
    snapshot.bytes_in_use = (uint64_t)info.uordblks;
#endif

    return snapshot;
}

static int report_unexpected_status(const char* const label, const instrument_api_status_t status) {
    fprintf(stderr, "%s failed with unexpected status %d\n", label, status);
    return 1;
}

static void format_timestamp(const time_t timestamp, char* const buffer, const size_t buffer_size) {
    if ((buffer == NULL) || (buffer_size == 0U)) {
        return;
    }

    struct tm tm_value;
    if (localtime_r(&timestamp, &tm_value) == NULL) {
        snprintf(buffer, buffer_size, "%lld", (long long)timestamp);
        return;
    }

    if (strftime(buffer, buffer_size, "%Y-%m-%d %H:%M:%S %Z", &tm_value) == 0U) {
        snprintf(buffer, buffer_size, "%lld", (long long)timestamp);
    }
}

static bool strings_equal(const char* const lhs, const char* const rhs) {
    if ((lhs == NULL) || (rhs == NULL)) {
        return false;
    }

    return strcmp(lhs, rhs) == 0;
}

static void record_bluetooth_discoveries(DiscoverySummary* const                summary,
                                         const BluetoothDeviceInfoBase* const    devices,
                                         const uint32_t                          device_count,
                                         const time_t                            discovered_at) {
    if ((summary == NULL) || (devices == NULL)) {
        return;
    }

    for (uint32_t i = 0U; i < device_count; ++i) {
        bool seen = false;
        for (uint32_t j = 0U; j < summary->bluetooth_count; ++j) {
            if (strings_equal(summary->bluetooth[j].mac_address, devices[i].mac_address)) {
                seen = true;
                break;
            }
        }

        if (seen || (summary->bluetooth_count >= BLUETOOTH_MAX_DEVICES)) {
            continue;
        }

        BluetoothDiscoveryRecord* const record = &summary->bluetooth[summary->bluetooth_count++];
        memset(record, 0, sizeof(*record));
        strncpy(record->mac_address, devices[i].mac_address, sizeof(record->mac_address) - 1U);
        strncpy(record->name, devices[i].name, sizeof(record->name) - 1U);
        record->first_seen = discovered_at;
    }
}

static void record_wifi_discoveries(DiscoverySummary* const           summary,
                                    const WifiDeviceInfoBase* const    devices,
                                    const uint32_t                     device_count,
                                    const time_t                       discovered_at) {
    if ((summary == NULL) || (devices == NULL)) {
        return;
    }

    for (uint32_t i = 0U; i < device_count; ++i) {
        bool seen = false;
        for (uint32_t j = 0U; j < summary->wifi_count; ++j) {
            if (strings_equal(summary->wifi[j].mac_address, devices[i].mac_address)) {
                seen = true;
                break;
            }
        }

        if (seen || (summary->wifi_count >= WIFI_MAX_DEVICES)) {
            continue;
        }

        WifiDiscoveryRecord* const record = &summary->wifi[summary->wifi_count++];
        memset(record, 0, sizeof(*record));
        strncpy(record->mac_address, devices[i].mac_address, sizeof(record->mac_address) - 1U);
        strncpy(record->ssid, devices[i].ssid, sizeof(record->ssid) - 1U);
        record->first_seen = discovered_at;
    }
}

static void print_discovery_summary(const DiscoverySummary* const summary) {
    char first_seen[64];

    printf("Bluetooth unique discoveries: %u\n", summary->bluetooth_count);
    for (uint32_t i = 0U; i < summary->bluetooth_count; ++i) {
        format_timestamp(summary->bluetooth[i].first_seen, first_seen, sizeof(first_seen));
        printf("  [%u] mac=%s name=%s first_seen=%s\n",
               i,
               summary->bluetooth[i].mac_address,
               summary->bluetooth[i].name[0] != '\0' ? summary->bluetooth[i].name : "<unknown>",
               first_seen);
    }

    printf("Wi-Fi unique discoveries: %u\n", summary->wifi_count);
    for (uint32_t i = 0U; i < summary->wifi_count; ++i) {
        format_timestamp(summary->wifi[i].first_seen, first_seen, sizeof(first_seen));
        printf("  [%u] mac=%s ssid=%s first_seen=%s\n",
               i,
               summary->wifi[i].mac_address,
               summary->wifi[i].ssid[0] != '\0' ? summary->wifi[i].ssid : "<hidden>",
               first_seen);
    }
}

static int exercise_bluetooth(const InstrumentAPI* const api, DiscoverySummary* const summary) {
    BluetoothAdapterInfoBase adapters[BLUETOOTH_MAX_ADAPTERS];
    BluetoothDeviceInfoBase devices[BLUETOOTH_MAX_DEVICES];
    uint32_t adapter_count = 0U;
    uint32_t device_count = 0U;

    memset(adapters, 0, sizeof(adapters));
    memset(devices, 0, sizeof(devices));

    const instrument_api_status_t adapter_status = api->instrumentAction(
        INSTRUMENT_BLUETOOTH_GET_ADAPTER_INFO, NULL, 0U, adapters, &adapter_count);
    if ((adapter_status != INSTRUMENT_API_SUCCESS) && (adapter_status != INSTRUMENT_API_NOT_SUPPORTED)) {
        return report_unexpected_status("Bluetooth adapter query", adapter_status);
    }

    printf("  Bluetooth adapter status=%d count=%u\n", adapter_status, adapter_count);
    if ((adapter_status != INSTRUMENT_API_SUCCESS) || (adapter_count == 0U)) {
        return 0;
    }

    const instrument_api_status_t discover_status = api->instrumentAction(
        INSTRUMENT_BLUETOOTH_DISCOVER_DEVICES, &adapters[0], 1U, devices, &device_count);
    if ((discover_status != INSTRUMENT_API_SUCCESS) && (discover_status != INSTRUMENT_API_NOT_SUPPORTED)) {
        return report_unexpected_status("Bluetooth discovery", discover_status);
    }

    printf("  Bluetooth discovery status=%d count=%u\n", discover_status, device_count);
    if (discover_status == INSTRUMENT_API_SUCCESS) {
        record_bluetooth_discoveries(summary, devices, device_count, time(NULL));
    }

    return 0;
}

static int exercise_wifi(const InstrumentAPI* const api, DiscoverySummary* const summary) {
    WifiDeviceInfoBase devices[WIFI_MAX_DEVICES];
    uint32_t adapter_count = 0U;
    uint32_t device_count = 0U;

    memset(devices, 0, sizeof(devices));

    const instrument_api_status_t adapter_status =
        api->instrumentAction(INSTRUMENT_WIFI_GET_MY_DEVICE, NULL, 0U, devices, &adapter_count);
    if ((adapter_status != INSTRUMENT_API_SUCCESS) && (adapter_status != INSTRUMENT_API_NOT_SUPPORTED)) {
        return report_unexpected_status("Wi-Fi adapter query", adapter_status);
    }

    printf("  Wi-Fi adapter status=%d count=%u\n", adapter_status, adapter_count);

    memset(devices, 0, sizeof(devices));
    const instrument_api_status_t discover_status =
        api->instrumentAction(INSTRUMENT_WIFI_DISCOVER_DEVICES, NULL, 0U, devices, &device_count);
    if ((discover_status != INSTRUMENT_API_SUCCESS) && (discover_status != INSTRUMENT_API_NOT_SUPPORTED)) {
        return report_unexpected_status("Wi-Fi discovery", discover_status);
    }

    printf("  Wi-Fi discovery status=%d count=%u\n", discover_status, device_count);
    if (discover_status == INSTRUMENT_API_SUCCESS) {
        record_wifi_discoveries(summary, devices, device_count, time(NULL));
    }

    return 0;
}

static int exercise_gps(const InstrumentAPI* const api) {
    C_PositionInfo position;
    uint32_t count = 0U;

    memset(&position, 0, sizeof(position));

    const instrument_api_status_t status =
        api->instrumentAction(INSTRUMENT_GPS_GET_POSITION, NULL, 0U, &position, &count);
    if ((status != INSTRUMENT_API_SUCCESS) && (status != INSTRUMENT_API_NOT_SUPPORTED)) {
        return report_unexpected_status("GPS poll", status);
    }

    printf("  GPS status=%d count=%u mode=%d timestamp=%lld\n",
           status,
           count,
           (int)position.mode,
           (long long)position.timestamp);
    return 0;
}

static int run_iteration(const InstrumentAPI* const api,
                         DiscoverySummary* const     summary,
                         const uint32_t              iteration,
                         const uint32_t              iterations) {
    printf("Iteration %u/%u\n", iteration + 1U, iterations);

    if (exercise_bluetooth(api, summary) != 0) {
        return 1;
    }
    if (exercise_wifi(api, summary) != 0) {
        return 1;
    }
    if (exercise_gps(api) != 0) {
        return 1;
    }

    return 0;
}

int main(int argc, char** argv) {
    uint32_t iterations = DEFAULT_ITERATIONS;

    if (argc > 2) {
        usage(argv[0]);
        return 1;
    }
    if ((argc == 2) && !parse_iterations(argv[1], &iterations)) {
        usage(argv[0]);
        return 1;
    }

    printf("Running repeated sensor sweep for %u iterations with %u second delay\n",
           iterations,
           ITERATION_DELAY_SECONDS);

    const InstrumentAPIConfig config = {0};
    InstrumentAPI* const api = createInstrumentAPI(config);
    if (api == NULL) {
        fprintf(stderr, "Failed to create InstrumentAPI\n");
        return 1;
    }

    DiscoverySummary summary;
    memset(&summary, 0, sizeof(summary));

    LeakSnapshot warmup = {0};
    for (uint32_t i = 0U; i < iterations; ++i) {
        if (run_iteration(api, &summary, i, iterations) != 0) {
            destroyInstrumentAPI(api);
            return 1;
        }

        if ((i == 0U) && (iterations > 1U)) {
            warmup = capture_leak_snapshot();
        }

        if (i + 1U < iterations) {
            sleep_between_iterations();
        }
    }

    print_discovery_summary(&summary);

    destroyInstrumentAPI(api);

    const LeakSnapshot after = capture_leak_snapshot();
    if ((iterations <= 1U) && after.supported) {
        printf("Leak check skipped: need at least 2 iterations to separate one-time runtime allocations from leaks\n");
    } else if (warmup.supported && after.supported) {
        const uint64_t leaked_bytes = (after.bytes_in_use > warmup.bytes_in_use)
            ? (after.bytes_in_use - warmup.bytes_in_use)
            : 0U;
        printf("Leak check: warmup=%" PRIu64 " after=%" PRIu64 " delta=%" PRIu64 "\n",
               warmup.bytes_in_use,
               after.bytes_in_use,
               leaked_bytes);
        if (leaked_bytes > LEAK_ALLOWANCE_BYTES) {
            fprintf(stderr,
                    "Leak check failed: allocator retained %" PRIu64 " bytes, above allowance of %u\n",
                    leaked_bytes,
                    LEAK_ALLOWANCE_BYTES);
            return 1;
        }
    } else {
        printf("Leak check skipped: allocator statistics unavailable on this libc\n");
    }

    printf("Repeated sensor sweep completed successfully\n");
    return 0;
}
