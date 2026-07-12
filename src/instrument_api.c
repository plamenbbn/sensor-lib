#define _GNU_SOURCE

#include "instrument_api.h"

#include "bluetooth_scanner.h"
#include "bluetooth_typedef.h"
#include "gps_typedef.h"
#include "wifi_scanner_bridge.h"
#include "wifi_typedef.h"

#include <arpa/inet.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <ctype.h>
#include <errno.h>
#include <net/if.h>
#include <linux/wireless.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define GPSD_DEFAULT_HOST     "127.0.0.1"
#define GPSD_DEFAULT_PORT     2947
#define GPSD_READ_TIMEOUT_MS  2500U

static instrument_api_status_t instrument_action_impl(InstrumentActions         action,
                                                      const InstrumentInputType input_data,
                                                      const uint32_t            input_len,
                                                      InstrumentOutputType      output_data,
                                                      uint32_t*                 output_len);
static instrument_api_status_t register_callback_impl(InstrumentType            instrument_type,
                                                      CallbackType              callback_type,
                                                      const InstrumentInputType input_data);
static instrument_api_status_t unregister_callback_impl(InstrumentType            instrument_type,
                                                        CallbackType              callback_type,
                                                        const InstrumentInputType input_data);

static void zero_output_len(uint32_t* const output_len) {
    if (output_len != NULL) {
        *output_len = 0U;
    }
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

static instrument_api_status_t open_inet_dgram_socket(int* const fd_out) {
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return INSTRUMENT_API_ERROR;
    }

    *fd_out = fd;
    return INSTRUMENT_API_SUCCESS;
}

static bool interface_is_wireless(const int fd, const char* const ifname) {
    struct iwreq req;
    memset(&req, 0, sizeof(req));
    copy_string(req.ifr_name, sizeof(req.ifr_name), ifname);

    return ioctl(fd, SIOCGIWNAME, &req) == 0;
}

static instrument_api_status_t get_if_flags(const int fd, const char* const ifname, short* const flags_out) {
    struct ifreq req;
    memset(&req, 0, sizeof(req));
    copy_string(req.ifr_name, sizeof(req.ifr_name), ifname);

    if (ioctl(fd, SIOCGIFFLAGS, &req) != 0) {
        return INSTRUMENT_API_ERROR;
    }

    *flags_out = req.ifr_flags;
    return INSTRUMENT_API_SUCCESS;
}

static instrument_api_status_t set_if_flags(const int fd, const char* const ifname, const short flags) {
    struct ifreq req;
    memset(&req, 0, sizeof(req));
    copy_string(req.ifr_name, sizeof(req.ifr_name), ifname);
    req.ifr_flags = flags;

    if (ioctl(fd, SIOCSIFFLAGS, &req) != 0) {
        return INSTRUMENT_API_ERROR;
    }

    return INSTRUMENT_API_SUCCESS;
}

static instrument_api_status_t find_wireless_interface(char* const ifname, const size_t ifname_size) {
    int fd = -1;
    if (open_inet_dgram_socket(&fd) != INSTRUMENT_API_SUCCESS) {
        return INSTRUMENT_API_ERROR;
    }

    struct if_nameindex* const ifaces = if_nameindex();
    if (ifaces == NULL) {
        close(fd);
        return INSTRUMENT_API_ERROR;
    }

    instrument_api_status_t status = INSTRUMENT_API_NOT_SUPPORTED;
    for (const struct if_nameindex* iface = ifaces; iface->if_index != 0U && iface->if_name != NULL; ++iface) {
        if (interface_is_wireless(fd, iface->if_name)) {
            copy_string(ifname, ifname_size, iface->if_name);
            status = INSTRUMENT_API_SUCCESS;
            break;
        }
    }

    if_freenameindex(ifaces);
    close(fd);
    return status;
}

static instrument_api_status_t wifi_get_interface_info(const char* const ifname, WifiDeviceInfoBase* const device) {
    int fd = -1;
    if (open_inet_dgram_socket(&fd) != INSTRUMENT_API_SUCCESS) {
        return INSTRUMENT_API_ERROR;
    }

    memset(device, 0, sizeof(*device));
    device->type = WIFI_ADAPTER;

    struct ifreq hwreq;
    memset(&hwreq, 0, sizeof(hwreq));
    copy_string(hwreq.ifr_name, sizeof(hwreq.ifr_name), ifname);
    if (ioctl(fd, SIOCGIFHWADDR, &hwreq) == 0) {
        const unsigned char* const mac = (const unsigned char*)hwreq.ifr_hwaddr.sa_data;
        snprintf(device->mac_address,
                 sizeof(device->mac_address),
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0],
                 mac[1],
                 mac[2],
                 mac[3],
                 mac[4],
                 mac[5]);
    }

    struct iwreq essid_req;
    char         essid[IW_ESSID_MAX_SIZE + 1];
    memset(&essid_req, 0, sizeof(essid_req));
    memset(essid, 0, sizeof(essid));
    copy_string(essid_req.ifr_name, sizeof(essid_req.ifr_name), ifname);
    essid_req.u.essid.pointer = essid;
    essid_req.u.essid.length  = IW_ESSID_MAX_SIZE + 1;
    essid_req.u.essid.flags   = 0;
    if (ioctl(fd, SIOCGIWESSID, &essid_req) == 0) {
        essid[sizeof(essid) - 1U] = '\0';
        copy_string(device->ssid, sizeof(device->ssid), essid);
    }

    close(fd);
    return INSTRUMENT_API_SUCCESS;
}

static instrument_api_status_t wifi_get_my_device(InstrumentOutputType output_data, uint32_t* const output_len) {
    if ((output_data == NULL) || (output_len == NULL)) {
        return INSTRUMENT_API_ERROR;
    }

    int fd = -1;
    if (open_inet_dgram_socket(&fd) != INSTRUMENT_API_SUCCESS) {
        return INSTRUMENT_API_ERROR;
    }

    struct if_nameindex* const ifaces = if_nameindex();
    if (ifaces == NULL) {
        close(fd);
        return INSTRUMENT_API_ERROR;
    }

    WifiDeviceInfoBase* const devices = (WifiDeviceInfoBase*)output_data;
    uint32_t                  count   = 0U;

    for (const struct if_nameindex* iface = ifaces;
         iface->if_index != 0U && iface->if_name != NULL && count < WIFI_MAX_DEVICES;
         ++iface) {
        if (!interface_is_wireless(fd, iface->if_name)) {
            continue;
        }

        if (wifi_get_interface_info(iface->if_name, &devices[count]) == INSTRUMENT_API_SUCCESS) {
            ++count;
        }
    }

    if_freenameindex(ifaces);
    close(fd);

    *output_len = count;
    return (count > 0U) ? INSTRUMENT_API_SUCCESS : INSTRUMENT_API_NOT_SUPPORTED;
}

static instrument_api_status_t wifi_is_up(InstrumentOutputType output_data, uint32_t* const output_len) {
    if ((output_data == NULL) || (output_len == NULL)) {
        return INSTRUMENT_API_ERROR;
    }

    char ifname[IFNAMSIZ];
    memset(ifname, 0, sizeof(ifname));
    if (find_wireless_interface(ifname, sizeof(ifname)) != INSTRUMENT_API_SUCCESS) {
        return INSTRUMENT_API_NOT_SUPPORTED;
    }

    int fd = -1;
    if (open_inet_dgram_socket(&fd) != INSTRUMENT_API_SUCCESS) {
        return INSTRUMENT_API_ERROR;
    }

    short flags = 0;
    const instrument_api_status_t status = get_if_flags(fd, ifname, &flags);
    close(fd);
    if (status != INSTRUMENT_API_SUCCESS) {
        return status;
    }

    *(bool*)output_data = ((flags & IFF_UP) != 0);
    *output_len         = 1U;
    return INSTRUMENT_API_SUCCESS;
}

static instrument_api_status_t wifi_set_up(const bool up) {
    char ifname[IFNAMSIZ];
    memset(ifname, 0, sizeof(ifname));
    if (find_wireless_interface(ifname, sizeof(ifname)) != INSTRUMENT_API_SUCCESS) {
        return INSTRUMENT_API_NOT_SUPPORTED;
    }

    int fd = -1;
    if (open_inet_dgram_socket(&fd) != INSTRUMENT_API_SUCCESS) {
        return INSTRUMENT_API_ERROR;
    }

    short flags = 0;
    instrument_api_status_t status = get_if_flags(fd, ifname, &flags);
    if (status == INSTRUMENT_API_SUCCESS) {
        if (up) {
            flags |= IFF_UP;
        } else {
            flags &= (short)~IFF_UP;
        }
        status = set_if_flags(fd, ifname, flags);
    }

    close(fd);
    return status;
}

static instrument_api_status_t wifi_discover_devices(InstrumentOutputType output_data, uint32_t* const output_len) {
    if ((output_data == NULL) || (output_len == NULL)) {
        return INSTRUMENT_API_ERROR;
    }

    char ifname[IFNAMSIZ];
    memset(ifname, 0, sizeof(ifname));
    if (find_wireless_interface(ifname, sizeof(ifname)) != INSTRUMENT_API_SUCCESS) {
        return INSTRUMENT_API_NOT_SUPPORTED;
    }

    return wifi_discover_devices_nl80211(ifname, output_data, output_len);
}

static instrument_api_status_t bluetooth_get_adapter_info(InstrumentOutputType output_data, uint32_t* const output_len) {
    if ((output_data == NULL) || (output_len == NULL)) {
        return INSTRUMENT_API_ERROR;
    }

    const int ctl = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
    if (ctl < 0) {
        return (errno == EAFNOSUPPORT) ? INSTRUMENT_API_NOT_SUPPORTED : INSTRUMENT_API_ERROR;
    }

    unsigned char raw[sizeof(struct hci_dev_list_req) +
                      (BLUETOOTH_MAX_ADAPTERS * sizeof(struct hci_dev_req))];
    memset(raw, 0, sizeof(raw));
    struct hci_dev_list_req* const dev_list = (struct hci_dev_list_req*)raw;
    dev_list->dev_num                        = BLUETOOTH_MAX_ADAPTERS;

    if (ioctl(ctl, HCIGETDEVLIST, dev_list) != 0) {
        close(ctl);
        return INSTRUMENT_API_ERROR;
    }

    BluetoothAdapterInfoBase* const adapters = (BluetoothAdapterInfoBase*)output_data;
    uint32_t                       count     = 0U;

    for (int i = 0; (i < dev_list->dev_num) && (count < BLUETOOTH_MAX_ADAPTERS); ++i) {
        struct hci_dev_info info;
        memset(&info, 0, sizeof(info));
        info.dev_id = dev_list->dev_req[i].dev_id;
        if (ioctl(ctl, HCIGETDEVINFO, (void*)&info) != 0) {
            continue;
        }

        memset(&adapters[count], 0, sizeof(adapters[count]));
        adapters[count].id = info.dev_id;
        ba2str(&info.bdaddr, adapters[count].mac_address);
        copy_string(adapters[count].name, sizeof(adapters[count].name), info.name);
        adapters[count].flags = info.flags;
        adapters[count].type  = info.type;
        ++count;
    }

    close(ctl);
    *output_len = count;
    return (count > 0U) ? INSTRUMENT_API_SUCCESS : INSTRUMENT_API_NOT_SUPPORTED;
}

static instrument_api_status_t bluetooth_is_up(const InstrumentInputType input_data,
                                               const uint32_t            input_len,
                                               InstrumentOutputType      output_data,
                                               uint32_t* const           output_len) {
    if ((input_data == NULL) || (input_len == 0U) || (output_data == NULL) || (output_len == NULL)) {
        return INSTRUMENT_API_ERROR;
    }

    const BluetoothAdapterInfoBase* const adapter = (const BluetoothAdapterInfoBase*)input_data;
    struct hci_dev_info                   info;
    memset(&info, 0, sizeof(info));
    info.dev_id = adapter->id;

    const int ctl = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
    if (ctl < 0) {
        return INSTRUMENT_API_ERROR;
    }

    if (ioctl(ctl, HCIGETDEVINFO, (void*)&info) != 0) {
        close(ctl);
        return INSTRUMENT_API_ERROR;
    }

    *(bool*)output_data = (hci_test_bit(HCI_UP, &info.flags) != 0);
    *output_len         = 1U;
    close(ctl);
    return INSTRUMENT_API_SUCCESS;
}

static instrument_api_status_t bluetooth_set_up(const InstrumentInputType input_data,
                                                const uint32_t            input_len,
                                                const bool                up) {
    if ((input_data == NULL) || (input_len == 0U)) {
        return INSTRUMENT_API_ERROR;
    }

    const BluetoothAdapterInfoBase* const adapter = (const BluetoothAdapterInfoBase*)input_data;
    const int ctl = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
    if (ctl < 0) {
        return INSTRUMENT_API_ERROR;
    }

    const int request = up ? HCIDEVUP : HCIDEVDOWN;
    const int rc      = ioctl(ctl, request, adapter->id);
    close(ctl);

    if ((rc == 0) || (errno == EALREADY)) {
        return INSTRUMENT_API_SUCCESS;
    }

    return INSTRUMENT_API_ERROR;
}

static instrument_api_status_t bluetooth_discover_devices(const InstrumentInputType input_data,
                                                          const uint32_t            input_len,
                                                          InstrumentOutputType      output_data,
                                                          uint32_t* const           output_len) {
    return bluetooth_discover_devices_backend(input_data, input_len, output_data, output_len);
}

static const char* json_find_value(const char* const json, const char* const key) {
    if ((json == NULL) || (key == NULL)) {
        return NULL;
    }

    char pattern[64];
    const int written = snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    if ((written <= 0) || ((size_t)written >= sizeof(pattern))) {
        return NULL;
    }

    const char* value = strstr(json, pattern);
    if (value == NULL) {
        return NULL;
    }

    value += (size_t)written;
    while ((*value != '\0') && isspace((unsigned char)*value)) {
        ++value;
    }
    return value;
}

static bool json_parse_number(const char* const json, const char* const key, long double* const value_out) {
    const char* const value = json_find_value(json, key);
    if (value == NULL) {
        return false;
    }

    char* end = NULL;
    const long double parsed = strtold(value, &end);
    if (end == value) {
        return false;
    }

    *value_out = parsed;
    return true;
}

static bool json_parse_int(const char* const json, const char* const key, int* const value_out) {
    const char* const value = json_find_value(json, key);
    if (value == NULL) {
        return false;
    }

    char* end = NULL;
    const long parsed = strtol(value, &end, 10);
    if (end == value) {
        return false;
    }

    *value_out = (int)parsed;
    return true;
}

static bool json_parse_string(const char* const json,
                              const char* const key,
                              char* const       value_out,
                              const size_t      value_out_size) {
    const char* value = json_find_value(json, key);
    if ((value == NULL) || (*value != '"') || (value_out_size == 0U)) {
        return false;
    }

    ++value;
    size_t index = 0U;
    while ((*value != '\0') && (*value != '"') && (index + 1U < value_out_size)) {
        if ((*value == '\\') && (value[1] != '\0')) {
            ++value;
        }
        value_out[index++] = *value++;
    }
    value_out[index] = '\0';

    return *value == '"';
}

static bool parse_gps_time(const char* const iso_time, time_t* const timestamp_out) {
    if ((iso_time == NULL) || (timestamp_out == NULL)) {
        return false;
    }

    struct tm tm_value;
    memset(&tm_value, 0, sizeof(tm_value));

    const char* parsed = strptime(iso_time, "%Y-%m-%dT%H:%M:%S", &tm_value);
    if (parsed == NULL) {
        return false;
    }

    *timestamp_out = timegm(&tm_value);
    return true;
}

static bool gps_parse_tpv(const char* const line, C_PositionInfo* const position) {
    if ((line == NULL) || (position == NULL)) {
        return false;
    }

    if (strstr(line, "\"class\":\"TPV\"") == NULL) {
        return false;
    }

    int mode = MODE_NOT_SEEN;
    (void)json_parse_int(line, "mode", &mode);
    position->mode = (gps_mode_t)mode;

    long double lat = 0.0L;
    long double lon = 0.0L;
    long double alt = 0.0L;
    (void)json_parse_number(line, "lat", &lat);
    (void)json_parse_number(line, "lon", &lon);
    if (!json_parse_number(line, "altHAE", &alt)) {
        (void)json_parse_number(line, "alt", &alt);
    }

    position->position.lat = lat;
    position->position.lon = lon;
    position->position.alt = alt;
    position->timestamp    = time(NULL);

    char time_buf[64];
    if (json_parse_string(line, "time", time_buf, sizeof(time_buf))) {
        (void)parse_gps_time(time_buf, &position->timestamp);
    }

    return true;
}

static bool gps_parse_poll(const char* const line, C_PositionInfo* const position) {
    if ((line == NULL) || (position == NULL)) {
        return false;
    }

    if (strstr(line, "\"class\":\"POLL\"") == NULL) {
        return false;
    }

    int active = 0;
    (void)json_parse_int(line, "active", &active);
    position->mode = (active > 0) ? MODE_NO_FIX : MODE_NOT_SEEN;

    char time_buf[64];
    if (json_parse_string(line, "time", time_buf, sizeof(time_buf))) {
        (void)parse_gps_time(time_buf, &position->timestamp);
    }

    return true;
}

static instrument_api_status_t gps_get_position(InstrumentOutputType output_data, uint32_t* const output_len) {
    if ((output_data == NULL) || (output_len == NULL)) {
        return INSTRUMENT_API_ERROR;
    }

    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return INSTRUMENT_API_ERROR;
    }

    struct timeval timeout;
    timeout.tv_sec  = GPSD_READ_TIMEOUT_MS / 1000U;
    timeout.tv_usec = (GPSD_READ_TIMEOUT_MS % 1000U) * 1000U;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(GPSD_DEFAULT_PORT);
    if (inet_pton(AF_INET, GPSD_DEFAULT_HOST, &addr.sin_addr) != 1) {
        close(fd);
        return INSTRUMENT_API_ERROR;
    }

    if (connect(fd, (const struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd);
        return (errno == ECONNREFUSED) ? INSTRUMENT_API_NOT_SUPPORTED : INSTRUMENT_API_ERROR;
    }

    static const char gpsd_watch_cmd[] = "?WATCH={\"enable\":true,\"json\":true};\n?POLL;\n";
    if (send(fd, gpsd_watch_cmd, sizeof(gpsd_watch_cmd) - 1U, 0) < 0) {
        close(fd);
        return INSTRUMENT_API_ERROR;
    }

    C_PositionInfo* const position = (C_PositionInfo*)output_data;
    memset(position, 0, sizeof(*position));
    position->timestamp = time(NULL);
    position->mode      = MODE_NOT_SEEN;
    bool saw_poll       = false;

    char   recv_buffer[4096];
    size_t used = 0U;

    while (used + 1U < sizeof(recv_buffer)) {
        const ssize_t bytes = recv(fd, recv_buffer + used, sizeof(recv_buffer) - used - 1U, 0);
        if (bytes <= 0) {
            break;
        }

        used += (size_t)bytes;
        recv_buffer[used] = '\0';

        char* line_start = recv_buffer;
        char* newline    = NULL;
        while ((newline = strchr(line_start, '\n')) != NULL) {
            *newline = '\0';
            if (gps_parse_tpv(line_start, position)) {
                close(fd);
                *output_len = 1U;
                return INSTRUMENT_API_SUCCESS;
            }
            if (gps_parse_poll(line_start, position)) {
                saw_poll = true;
            }
            line_start = newline + 1;
        }

        const size_t remaining = used - (size_t)(line_start - recv_buffer);
        memmove(recv_buffer, line_start, remaining);
        used = remaining;
        recv_buffer[used] = '\0';
    }

    close(fd);
    if (saw_poll) {
        *output_len = 1U;
        return INSTRUMENT_API_SUCCESS;
    }

    *output_len = 1U;
    return INSTRUMENT_API_ERROR;
}

static instrument_api_status_t instrument_action_impl(const InstrumentActions         action,
                                                      const InstrumentInputType input_data,
                                                      const uint32_t            input_len,
                                                      InstrumentOutputType      output_data,
                                                      uint32_t* const           output_len) {
    zero_output_len(output_len);

    switch (action) {
    case INSTRUMENT_BLUETOOTH_GET_ADAPTER_INFO:
        return bluetooth_get_adapter_info(output_data, output_len);
    case INSTRUMENT_BLUETOOTH_DISCOVER_DEVICES:
        return bluetooth_discover_devices(input_data, input_len, output_data, output_len);
    case INSTRUMENT_BLUETOOTH_IS_UP:
        return bluetooth_is_up(input_data, input_len, output_data, output_len);
    case INSTRUMENT_BLUETOOTH_TURN_ON:
        return bluetooth_set_up(input_data, input_len, true);
    case INSTRUMENT_BLUETOOTH_TURN_OFF:
        return bluetooth_set_up(input_data, input_len, false);
    case INSTRUMENT_BLUETOOTH_SCAN_ON:
    case INSTRUMENT_BLUETOOTH_SCAN_OFF:
        return INSTRUMENT_API_SUCCESS;

    case INSTRUMENT_WIFI_GET_MY_DEVICE:
        return wifi_get_my_device(output_data, output_len);
    case INSTRUMENT_WIFI_DISCOVER_DEVICES:
        return wifi_discover_devices(output_data, output_len);
    case INSTRUMENT_WIFI_IS_UP:
        return wifi_is_up(output_data, output_len);
    case INSTRUMENT_WIFI_TURN_ON:
        return wifi_set_up(true);
    case INSTRUMENT_WIFI_TURN_OFF:
        return wifi_set_up(false);
    case INSTRUMENT_WIFI_SCAN_ON:
    case INSTRUMENT_WIFI_SCAN_OFF:
        return INSTRUMENT_API_SUCCESS;

    case INSTRUMENT_GPS_GET_POSITION:
        return gps_get_position(output_data, output_len);

    default:
        return INSTRUMENT_API_NOT_SUPPORTED;
    }
}

static instrument_api_status_t register_callback_impl(const InstrumentType            instrument_type,
                                                      const CallbackType              callback_type,
                                                      const InstrumentInputType input_data) {
    (void)instrument_type;
    (void)callback_type;
    (void)input_data;
    return INSTRUMENT_API_NOT_SUPPORTED;
}

static instrument_api_status_t unregister_callback_impl(const InstrumentType            instrument_type,
                                                        const CallbackType              callback_type,
                                                        const InstrumentInputType input_data) {
    (void)instrument_type;
    (void)callback_type;
    (void)input_data;
    return INSTRUMENT_API_NOT_SUPPORTED;
}

InstrumentAPI* createInstrumentAPI(const InstrumentAPIConfig config) {
    InstrumentAPI* const api = (InstrumentAPI*)calloc(1U, sizeof(*api));
    if (api == NULL) {
        return NULL;
    }

    api->instrumentAction   = instrument_action_impl;
    api->registerCallback   = register_callback_impl;
    api->unregisterCallback = unregister_callback_impl;
    api->interfaceConfig    = config;

    return api;
}

void destroyInstrumentAPI(InstrumentAPI* const instrumentAPI) {
    free(instrumentAPI);
}

const char* getInstrumentAPIVersion() {
    return "0.1.0";
}

const char* getInstrumentAPIPlatform() {
    return "linux";
}
