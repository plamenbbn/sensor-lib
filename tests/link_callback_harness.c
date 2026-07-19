#include "link_callback_harness.h"

#include "BratislavaSocket.h"
#include "bluetooth_typedef.h"
#include "callback_typedef.h"
#include "comms_typedef.h"
#include "sensing_typedef.h"
#include "wifi_typedef.h"

#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    InstrumentType instrument_type;
    char           mac_address[18];
    char           display_name[256];
    char           link_id[20];
    bool           callback_seen;
    bool           socket_ready;
    bool           exchange_started;
    bool           exchange_completed;
    bool           worker_running;
    bool           worker_joined;
    unsigned       tx_count;
    unsigned       rx_count;
    pthread_t      worker;
} LinkResultRecord;

static pthread_mutex_t g_link_result_mutex = PTHREAD_MUTEX_INITIALIZER;
static LinkResultRecord g_link_results[COMMS_MAX_LINKS * 4];
static unsigned g_link_result_count = 0U;
static volatile sig_atomic_t g_stop_requested = 0;
static const unsigned g_nmcli_timeout_seconds = 15U;
static const unsigned g_test_message_count = 3U;
static const unsigned g_message_delay_seconds = 1U;
static const unsigned g_recv_idle_limit = 4U;
static const unsigned g_recv_timeout_ms = 1000U;
static const char* g_default_test_ssid = "sensor-lib-link-callback";
static const char* g_default_test_password = "sensorlib123";

typedef struct {
    BratislavaLink link;
    char           local_node_name[128];
} LinkWorkerArgs;

typedef struct {
    bool configured;
    HarnessWifiMode mode;
    char interface_name[64];
    char previous_connection[256];
} WifiModeState;

static void handle_signal(const int signum) {
    (void)signum;
    g_stop_requested = 1;
}

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

static const char* getenv_or_default(const char* const name, const char* const fallback) {
    const char* const value = getenv(name);
    return ((value != NULL) && (value[0] != '\0')) ? value : fallback;
}

static const char* instrument_name(const InstrumentType instrument_type) {
    switch (instrument_type) {
    case INSTRUMENT_BLUETOOTH:
        return "BT";
    case INSTRUMENT_WIFI:
        return "WF";
    default:
        return "??";
    }
}

static void get_local_node_name(char* const buffer, const size_t buffer_size) {
    if ((buffer == NULL) || (buffer_size == 0U)) {
        return;
    }

    if (gethostname(buffer, buffer_size) != 0) {
        snprintf(buffer, buffer_size, "pid-%ld", (long)getpid());
        return;
    }

    buffer[buffer_size - 1U] = '\0';
}

static const char* display_bt_name(const char* const name) {
    return ((name != NULL) && (name[0] != '\0')) ? name : "<NONE>";
}

static bool read_first_line(const char* const command, char* const output, const size_t output_size) {
    if ((command == NULL) || (output == NULL) || (output_size == 0U)) {
        return false;
    }

    FILE* const pipe = popen(command, "r");
    if (pipe == NULL) {
        output[0] = '\0';
        return false;
    }

    const bool ok = (fgets(output, (int)output_size, pipe) != NULL);
    (void)pclose(pipe);
    if (!ok) {
        output[0] = '\0';
        return false;
    }

    output[strcspn(output, "\r\n")] = '\0';
    return output[0] != '\0';
}

static bool run_timed_command(const char* const label, const char* const command, const bool warn_only) {
    if (command == NULL) {
        return false;
    }

    char temp_path[] = "/tmp/sensor-lib-link-callback.XXXXXX";
    const int temp_fd = mkstemp(temp_path);
    if (temp_fd >= 0) {
        close(temp_fd);
    } else {
        temp_path[0] = '\0';
    }

    char wrapped_command[2048];
    snprintf(wrapped_command,
             sizeof(wrapped_command),
             "timeout %us /bin/bash -lc \"%s\"%s%s%s",
             g_nmcli_timeout_seconds,
             command,
             temp_path[0] != '\0' ? " >" : "",
             temp_path[0] != '\0' ? temp_path : "",
             temp_path[0] != '\0' ? " 2>&1" : "");

    const int rc = system(wrapped_command);
    if (rc == 0) {
        printf("[wifi-mode] %s: ok\n", label);
        fflush(stdout);
        if (temp_path[0] != '\0') {
            (void)unlink(temp_path);
        }
        return true;
    }

    if (WIFEXITED(rc) && (WEXITSTATUS(rc) == 124)) {
        fprintf(stderr, "[wifi-mode] %s: timed out after %u seconds\n", label, g_nmcli_timeout_seconds);
    } else if (WIFEXITED(rc)) {
        fprintf(stderr, "[wifi-mode] %s: failed with exit code %d\n", label, WEXITSTATUS(rc));
    } else {
        fprintf(stderr, "[wifi-mode] %s: failed\n", label);
    }
    fflush(stderr);

    if (temp_path[0] != '\0') {
        FILE* const temp_file = fopen(temp_path, "r");
        if (temp_file != NULL) {
            char line[512];
            while (fgets(line, sizeof(line), temp_file) != NULL) {
                fprintf(stderr, "[wifi-mode] %s output: %s", label, line);
            }
            fclose(temp_file);
        }
        (void)unlink(temp_path);
    }

    return warn_only ? true : false;
}

static bool configure_wifi_mode(const HarnessWifiMode wifi_mode, WifiModeState* const state) {
    memset(state, 0, sizeof(*state));
    state->mode = wifi_mode;

    if (!read_first_line("nmcli -t -f DEVICE,TYPE device status | awk -F: '$2==\"wifi\"{print $1; exit}'",
                         state->interface_name,
                         sizeof(state->interface_name))) {
        fprintf(stderr, "No Wi-Fi interface found for requested mode.\n");
        return false;
    }

    (void)read_first_line(
        "nmcli -t -f NAME,TYPE connection show --active | awk -F: '$2==\"802-11-wireless\"{print $1; exit}'",
        state->previous_connection,
        sizeof(state->previous_connection));

    if (wifi_mode == HARNESS_WIFI_MODE_CLIENT) {
        const char* const ssid = getenv_or_default("SENSOR_LIB_TEST_SSID", g_default_test_ssid);
        const char* const password = getenv_or_default("SENSOR_LIB_TEST_PASSWORD", g_default_test_password);

        (void)run_timed_command("enable Wi-Fi radio", "nmcli radio wifi on", true);
        (void)run_timed_command("delete stale client profile", "nmcli connection delete sensor-lib-link-callback-client || true", true);

        char command[1024];
        snprintf(command,
                 sizeof(command),
                 "nmcli device wifi rescan ifname '%s' || true",
                 state->interface_name);
        (void)run_timed_command("rescan Wi-Fi", command, true);

        snprintf(command,
                 sizeof(command),
                 "nmcli device wifi connect '%s' password '%s' ifname '%s' name sensor-lib-link-callback-client",
                 ssid,
                 password,
                 state->interface_name);
        if (!run_timed_command("connect to test hotspot", command, false)) {
            return false;
        }

        snprintf(command,
                 sizeof(command),
                 "gateway=$(ip route show dev '%s' default | awk '/default/ {print $3; exit}'); "
                 "if [ -n \"$gateway\" ]; then ping -c 2 -W 2 \"$gateway\" >/dev/null 2>&1 || true; fi",
                 state->interface_name);
        (void)run_timed_command("prime ARP with gateway ping", command, true);

        state->configured = true;
        return true;
    }

    const char* const ssid = getenv_or_default("SENSOR_LIB_TEST_SSID", g_default_test_ssid);
    const char* const password = getenv_or_default("SENSOR_LIB_TEST_PASSWORD", g_default_test_password);

    (void)run_timed_command("enable Wi-Fi radio", "nmcli radio wifi on", true);
    (void)run_timed_command("delete stale hotspot profile", "nmcli connection delete sensor-lib-link-callback-hotspot || true", true);

    char command[1024];
    snprintf(command,
             sizeof(command),
             "nmcli device wifi hotspot ifname '%s' con-name sensor-lib-link-callback-hotspot "
             "ssid '%s' password '%s'",
             state->interface_name,
             ssid,
             password);
    if (!run_timed_command("start hotspot", command, false)) {
        return false;
    }

    state->configured = true;
    return true;
}

static void restore_wifi_mode(const WifiModeState* const state) {
    if ((state == NULL) || !state->configured) {
        return;
    }

    if (state->mode == HARNESS_WIFI_MODE_HOTSPOT) {
        (void)run_timed_command("bring hotspot down", "nmcli connection down sensor-lib-link-callback-hotspot", true);
        (void)run_timed_command("delete hotspot profile", "nmcli connection delete sensor-lib-link-callback-hotspot", true);
    } else {
        (void)run_timed_command("bring client profile down", "nmcli connection down sensor-lib-link-callback-client", true);
        (void)run_timed_command("delete client profile", "nmcli connection delete sensor-lib-link-callback-client", true);
    }

    if (state->previous_connection[0] != '\0') {
        char command[1024];
        snprintf(command,
                 sizeof(command),
                 "nmcli connection up '%s' ifname '%s'",
                 state->previous_connection,
                 state->interface_name);
        (void)run_timed_command("restore previous Wi-Fi connection", command, true);
    } else if (state->interface_name[0] != '\0') {
        char command[1024];
        snprintf(command, sizeof(command), "nmcli device connect '%s'", state->interface_name);
        (void)run_timed_command("restore generic Wi-Fi client mode", command, true);
    }
}

static void reset_link_results(void) {
    pthread_mutex_lock(&g_link_result_mutex);
    memset(g_link_results, 0, sizeof(g_link_results));
    g_link_result_count = 0U;
    pthread_mutex_unlock(&g_link_result_mutex);
}

static void record_link_success(const BratislavaLink* const link) {
    if (link == NULL) {
        return;
    }

    BratislavaSocket* const bsock = bratislavaSocket(*link);
    const bool socket_ready = (bsock != NULL);

    const char* mac_address = NULL;
    const char* display_name = NULL;
    switch (link->instrumentType) {
    case INSTRUMENT_BLUETOOTH:
        mac_address = link->bluetoothDeviceInfo.mac_address;
        display_name = link->bluetoothDeviceInfo.name;
        break;
    case INSTRUMENT_WIFI:
        mac_address = link->wifiDeviceInfo.mac_address;
        display_name = link->wifiDeviceInfo.ssid;
        break;
    default:
        return;
    }

    pthread_mutex_lock(&g_link_result_mutex);
    for (unsigned i = 0U; i < g_link_result_count; ++i) {
        if ((g_link_results[i].instrument_type == link->instrumentType) &&
            (strcmp(g_link_results[i].mac_address, mac_address) == 0)) {
            g_link_results[i].callback_seen = true;
            g_link_results[i].socket_ready = socket_ready;
            copy_string(g_link_results[i].link_id, sizeof(g_link_results[i].link_id), link->linkID);
            if (g_link_results[i].display_name[0] == '\0') {
                copy_string(g_link_results[i].display_name, sizeof(g_link_results[i].display_name), display_name);
            }
            pthread_mutex_unlock(&g_link_result_mutex);
            return;
        }
    }

    if (g_link_result_count < (sizeof(g_link_results) / sizeof(g_link_results[0]))) {
        LinkResultRecord* const record = &g_link_results[g_link_result_count++];
        memset(record, 0, sizeof(*record));
        record->instrument_type = link->instrumentType;
        copy_string(record->mac_address, sizeof(record->mac_address), mac_address);
        copy_string(record->display_name, sizeof(record->display_name), display_name);
        copy_string(record->link_id, sizeof(record->link_id), link->linkID);
        record->callback_seen = true;
        record->socket_ready = socket_ready;
    }
    pthread_mutex_unlock(&g_link_result_mutex);
}

static bool find_link_success(const InstrumentType instrument_type,
                              const char* const    mac_address,
                              bool* const          socket_ready_out) {
    bool found = false;

    pthread_mutex_lock(&g_link_result_mutex);
    for (unsigned i = 0U; i < g_link_result_count; ++i) {
        if ((g_link_results[i].instrument_type == instrument_type) &&
            (strcmp(g_link_results[i].mac_address, mac_address) == 0) &&
            g_link_results[i].callback_seen) {
            found = true;
            if (socket_ready_out != NULL) {
                *socket_ready_out = g_link_results[i].socket_ready;
            }
            break;
        }
    }
    pthread_mutex_unlock(&g_link_result_mutex);

    return found;
}

static bool mark_link_exchange_started(const BratislavaLink* const link) {
    if (link == NULL) {
        return false;
    }

    const char* mac_address = NULL;
    switch (link->instrumentType) {
    case INSTRUMENT_BLUETOOTH:
        mac_address = link->bluetoothDeviceInfo.mac_address;
        break;
    case INSTRUMENT_WIFI:
        mac_address = link->wifiDeviceInfo.mac_address;
        break;
    default:
        return false;
    }

    bool should_start = false;
    pthread_mutex_lock(&g_link_result_mutex);
    for (unsigned i = 0U; i < g_link_result_count; ++i) {
        if ((g_link_results[i].instrument_type == link->instrumentType) &&
            (strcmp(g_link_results[i].mac_address, mac_address) == 0)) {
            if (!g_link_results[i].exchange_started) {
                g_link_results[i].exchange_started = true;
                should_start = true;
            }
            break;
        }
    }
    pthread_mutex_unlock(&g_link_result_mutex);
    return should_start;
}

static void update_link_exchange_state(const BratislavaLink* const link,
                                       const bool                 completed,
                                       const unsigned             tx_count,
                                       const unsigned             rx_count) {
    if (link == NULL) {
        return;
    }

    const char* mac_address = NULL;
    switch (link->instrumentType) {
    case INSTRUMENT_BLUETOOTH:
        mac_address = link->bluetoothDeviceInfo.mac_address;
        break;
    case INSTRUMENT_WIFI:
        mac_address = link->wifiDeviceInfo.mac_address;
        break;
    default:
        return;
    }

    pthread_mutex_lock(&g_link_result_mutex);
    for (unsigned i = 0U; i < g_link_result_count; ++i) {
        if ((g_link_results[i].instrument_type == link->instrumentType) &&
            (strcmp(g_link_results[i].mac_address, mac_address) == 0)) {
            g_link_results[i].exchange_completed = completed;
            g_link_results[i].worker_running = false;
            g_link_results[i].tx_count = tx_count;
            g_link_results[i].rx_count = rx_count;
            break;
        }
    }
    pthread_mutex_unlock(&g_link_result_mutex);
}

static void remember_link_worker(const BratislavaLink* const link, const pthread_t worker) {
    if (link == NULL) {
        return;
    }

    const char* mac_address = NULL;
    switch (link->instrumentType) {
    case INSTRUMENT_BLUETOOTH:
        mac_address = link->bluetoothDeviceInfo.mac_address;
        break;
    case INSTRUMENT_WIFI:
        mac_address = link->wifiDeviceInfo.mac_address;
        break;
    default:
        return;
    }

    pthread_mutex_lock(&g_link_result_mutex);
    for (unsigned i = 0U; i < g_link_result_count; ++i) {
        if ((g_link_results[i].instrument_type == link->instrumentType) &&
            (strcmp(g_link_results[i].mac_address, mac_address) == 0)) {
            g_link_results[i].worker_running = true;
            g_link_results[i].worker = worker;
            break;
        }
    }
    pthread_mutex_unlock(&g_link_result_mutex);
}

static void wait_for_link_workers(void) {
    pthread_t workers[COMMS_MAX_LINKS * 4];
    unsigned worker_count = 0U;

    pthread_mutex_lock(&g_link_result_mutex);
    for (unsigned i = 0U; i < g_link_result_count; ++i) {
        if (g_link_results[i].exchange_started && !g_link_results[i].worker_joined) {
            workers[worker_count++] = g_link_results[i].worker;
            g_link_results[i].worker_joined = true;
        }
    }
    pthread_mutex_unlock(&g_link_result_mutex);

    for (unsigned i = 0U; i < worker_count; ++i) {
        (void)pthread_join(workers[i], NULL);
    }
}

static int send_framed_message(BratislavaSocket* const bsock, const char* const message) {
    const uint32_t message_length = (uint32_t)strlen(message) + 1U;

    if (bratislavaSend(bsock, &message_length, sizeof(message_length)) == -1) {
        return -1;
    }

    if (bratislavaSend(bsock, message, message_length) == -1) {
        return -1;
    }

    return 0;
}

static int recv_framed_message(BratislavaSocket* const bsock, char** const message_out) {
    uint32_t message_length = 0U;
    const int header_rc = bratislavaRecv(bsock, &message_length, sizeof(message_length));
    if (header_rc <= 0) {
        if ((header_rc < 0) && (errno == ETIMEDOUT)) {
            return 0;
        }
        return -1;
    }

    char* const message = (char*)malloc(message_length);
    if (message == NULL) {
        errno = ENOMEM;
        return -1;
    }

    const int body_rc = bratislavaRecv(bsock, message, message_length);
    if (body_rc <= 0) {
        free(message);
        if ((body_rc < 0) && (errno == ETIMEDOUT)) {
            return 0;
        }
        return -1;
    }

    message[message_length - 1U] = '\0';
    *message_out = message;
    return 1;
}

static void* link_message_worker(void* const opaque_args) {
    LinkWorkerArgs* const args = (LinkWorkerArgs*)opaque_args;
    BratislavaSocket* const bsock = bratislavaSocket(args->link);
    unsigned tx_count = 0U;
    unsigned rx_count = 0U;
    bool completed = false;

    if (bsock == NULL) {
        fprintf(stderr, "Message test skipped: no BratislavaSocket for link %s\n", args->link.linkID);
        goto cleanup;
    }

    (void)bratislavaConnTimeout(bsock, DEFAULT_CONN_TIMEOUT_MS);
    (void)bratislavaRecvTimeout(bsock, g_recv_timeout_ms);

    if (bratislavaConn(bsock) != 0) {
        fprintf(stderr, "Message test connect failed for link %s: errno=%d\n", args->link.linkID, errno);
        goto cleanup_socket;
    }

    printf("Message test connected: %s link=%s dev=%s\n",
           instrument_name(args->link.instrumentType),
           args->link.linkID,
           args->link.devID);
    fflush(stdout);

    for (unsigned i = 0U; (i < g_test_message_count) && !g_stop_requested; ++i) {
        char message[512];
        snprintf(message,
                 sizeof(message),
                 "clear-text test %u/%u from %s pid=%ld over %s to %s",
                 i + 1U,
                 g_test_message_count,
                 args->local_node_name,
                 (long)getpid(),
                 instrument_name(args->link.instrumentType),
                 args->link.devID);
        printf("TX %s link=%s: %s\n", instrument_name(args->link.instrumentType), args->link.linkID, message);
        fflush(stdout);

        if (send_framed_message(bsock, message) != 0) {
            fprintf(stderr, "Message send failed for link %s: errno=%d\n", args->link.linkID, errno);
            goto cleanup_socket;
        }
        ++tx_count;

        sleep(g_message_delay_seconds);
    }

    unsigned idle_timeouts = 0U;
    while (!g_stop_requested && (idle_timeouts < g_recv_idle_limit)) {
        char* message = NULL;
        const int recv_rc = recv_framed_message(bsock, &message);
        if (recv_rc > 0) {
            printf("RX %s link=%s: %s\n", instrument_name(args->link.instrumentType), args->link.linkID, message);
            fflush(stdout);
            free(message);
            ++rx_count;
            idle_timeouts = 0U;
            continue;
        }

        if (recv_rc == 0) {
            ++idle_timeouts;
            continue;
        }

        fprintf(stderr, "Message receive failed for link %s: errno=%d\n", args->link.linkID, errno);
        goto cleanup_socket;
    }

    completed = true;

cleanup_socket:
    bratislavaDestroy(bsock);
cleanup:
    update_link_exchange_state(&args->link, completed, tx_count, rx_count);
    free(args);
    return NULL;
}

static void link_callback_logger(const BratislavaLink* const link) {
    BratislavaSocket* const bsock = bratislavaSocket(*link);
    const char* mac_address = "";
    const char* display_name = "";
    switch (link->instrumentType) {
    case INSTRUMENT_BLUETOOTH:
        mac_address = link->bluetoothDeviceInfo.mac_address;
        display_name = link->bluetoothDeviceInfo.name;
        break;
    case INSTRUMENT_WIFI:
        mac_address = link->wifiDeviceInfo.mac_address;
        display_name = link->wifiDeviceInfo.ssid;
        break;
    default:
        break;
    }

    printf("LINK_DISCOVERED callback: %s mac=%s name=%s bsock=%p\n",
           instrument_name(link->instrumentType),
           mac_address,
           (link->instrumentType == INSTRUMENT_BLUETOOTH) ? display_bt_name(display_name) : display_name,
           (void*)bsock);
    record_link_success(link);

    if (mark_link_exchange_started(link)) {
        LinkWorkerArgs* const args = (LinkWorkerArgs*)calloc(1U, sizeof(*args));
        if (args == NULL) {
            fprintf(stderr, "Failed to allocate link worker args.\n");
            fflush(stderr);
            return;
        }

        args->link = *link;
        get_local_node_name(args->local_node_name, sizeof(args->local_node_name));

        pthread_t worker;
        if (pthread_create(&worker, NULL, link_message_worker, args) != 0) {
            fprintf(stderr, "Failed to launch message worker for link %s\n", link->linkID);
            fflush(stderr);
            update_link_exchange_state(link, false, 0U, 0U);
            free(args);
        } else {
            remember_link_worker(link, worker);
        }
    }
    fflush(stdout);
}

static void report_bluetooth_devices(const BluetoothDeviceInfoBase* const devices, const uint32_t device_count) {
    for (uint32_t i = 0U; i < device_count; ++i) {
        bool socket_ready = false;
        const bool success = find_link_success(INSTRUMENT_BLUETOOTH, devices[i].mac_address, &socket_ready);
        printf("  [BT] mac=%s name=%s -> %s",
               devices[i].mac_address,
               display_bt_name(devices[i].name),
               success ? "SUCCESS" : "FAIL");
        if (success) {
            printf(" bsock=%s", socket_ready ? "ready" : "missing");
        }
        pthread_mutex_lock(&g_link_result_mutex);
        for (unsigned j = 0U; j < g_link_result_count; ++j) {
            if ((g_link_results[j].instrument_type == INSTRUMENT_BLUETOOTH) &&
                (strcmp(g_link_results[j].mac_address, devices[i].mac_address) == 0)) {
                if (g_link_results[j].exchange_started) {
                    printf(" msg-test=started");
                }
                if (g_link_results[j].exchange_completed) {
                    printf(" tx=%u rx=%u", g_link_results[j].tx_count, g_link_results[j].rx_count);
                }
                break;
            }
        }
        pthread_mutex_unlock(&g_link_result_mutex);
        printf("\n");
    }
}

static void report_wifi_devices(const WifiDeviceInfoBase* const devices, const uint32_t device_count) {
    for (uint32_t i = 0U; i < device_count; ++i) {
        bool socket_ready = false;
        const bool success = find_link_success(INSTRUMENT_WIFI, devices[i].mac_address, &socket_ready);
        printf("  [WF] mac=%s ssid=%s -> %s",
               devices[i].mac_address,
               devices[i].ssid,
               success ? "SUCCESS" : "FAIL");
        if (success) {
            printf(" bsock=%s", socket_ready ? "ready" : "missing");
        }
        pthread_mutex_lock(&g_link_result_mutex);
        for (unsigned j = 0U; j < g_link_result_count; ++j) {
            if ((g_link_results[j].instrument_type == INSTRUMENT_WIFI) &&
                (strcmp(g_link_results[j].mac_address, devices[i].mac_address) == 0)) {
                if (g_link_results[j].exchange_started) {
                    printf(" msg-test=started");
                }
                if (g_link_results[j].exchange_completed) {
                    printf(" tx=%u rx=%u", g_link_results[j].tx_count, g_link_results[j].rx_count);
                }
                break;
            }
        }
        pthread_mutex_unlock(&g_link_result_mutex);
        printf("\n");
    }
}

int run_link_callback_harness(const InstrumentAPI* const api,
                              const unsigned             interval_seconds,
                              const HarnessWifiMode      wifi_mode) {
    if (api == NULL) {
        fprintf(stderr, "InstrumentAPI is NULL\n");
        return 1;
    }

    reset_link_results();
    g_stop_requested = 0;

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGINT, &action, NULL);
    (void)sigaction(SIGTERM, &action, NULL);

    WifiModeState wifi_mode_state;
    if (!configure_wifi_mode(wifi_mode, &wifi_mode_state)) {
        return 1;
    }

    printf("Wi-Fi harness mode: %s\n", wifi_mode == HARNESS_WIFI_MODE_HOTSPOT ? "hotspot" : "client");
    LinkDiscoveredCallback callback = link_callback_logger;
    instrument_api_status_t status =
        api->registerCallback(INSTRUMENT_COMMS, LINK_DISCOVERED, (InstrumentInputType)&callback);
    if (status != INSTRUMENT_API_SUCCESS) {
        fprintf(stderr, "Failed to register LINK_DISCOVERED callback: %d\n", status);
        restore_wifi_mode(&wifi_mode_state);
        return 1;
    }

    unsigned iteration = 0U;
    while (!g_stop_requested) {
        BluetoothAdapterInfoBase bt_adapters[BLUETOOTH_MAX_ADAPTERS];
        BluetoothDeviceInfoBase bt_devices[BLUETOOTH_MAX_DEVICES];
        WifiDeviceInfoBase wf_devices[WIFI_MAX_DEVICES];
        BratislavaLink* links[COMMS_MAX_LINKS];
        uint32_t bt_adapter_count = 0U;
        uint32_t bt_device_count = 0U;
        uint32_t wf_device_count = 0U;
        uint32_t link_count = 0U;

        ++iteration;

        status = api->instrumentAction(INSTRUMENT_BLUETOOTH_GET_ADAPTER_INFO, NULL, 0U, bt_adapters, &bt_adapter_count);
        if ((status == INSTRUMENT_API_SUCCESS) && (bt_adapter_count > 0U)) {
            status = api->instrumentAction(
                INSTRUMENT_BLUETOOTH_DISCOVER_DEVICES, &bt_adapters[0], 1U, bt_devices, &bt_device_count);
            if ((status != INSTRUMENT_API_SUCCESS) && (status != INSTRUMENT_API_NOT_SUPPORTED)) {
                fprintf(stderr, "  Bluetooth discovery failed: %d\n", status);
            }
        } else if (status != INSTRUMENT_API_NOT_SUPPORTED) {
            fprintf(stderr, "  Bluetooth adapter query failed: %d\n", status);
        }

        status = api->instrumentAction(INSTRUMENT_WIFI_DISCOVER_DEVICES, NULL, 0U, wf_devices, &wf_device_count);
        if ((status != INSTRUMENT_API_SUCCESS) && (status != INSTRUMENT_API_NOT_SUPPORTED)) {
            fprintf(stderr, "  Wi-Fi discovery failed: %d\n", status);
        }

        status = api->instrumentAction(INSTRUMENT_COMMS_DISCOVER_BRATISLAVA_LINKS, NULL, 0U, links, &link_count);
        if ((status != INSTRUMENT_API_SUCCESS) && (status != INSTRUMENT_API_NOT_SUPPORTED)) {
            fprintf(stderr, "  Comms link discovery failed: %d\n", status);
        }

        printf("Iteration %u summary\n", iteration);
        printf("  Active links: %u\n", link_count);
        if (bt_device_count == 0U) {
            printf("  [BT] no devices discovered\n");
        } else {
            report_bluetooth_devices(bt_devices, bt_device_count);
        }

        if (wf_device_count == 0U) {
            printf("  [WF] no devices discovered\n");
        } else {
            report_wifi_devices(wf_devices, wf_device_count);
        }

        fflush(stdout);
        fflush(stderr);

        if (g_stop_requested) {
            break;
        }

        for (unsigned second = 0U; second < interval_seconds; ++second) {
            if (g_stop_requested) {
                break;
            }
            sleep(1);
        }
    }

    (void)api->unregisterCallback(INSTRUMENT_COMMS, LINK_DISCOVERED, (InstrumentInputType)&callback);
    wait_for_link_workers();
    restore_wifi_mode(&wifi_mode_state);
    return 0;
}
