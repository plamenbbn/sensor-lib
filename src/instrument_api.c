#define _GNU_SOURCE

#include "instrument_api.h"

#include "bluetooth_typedef.h"
#include "gps_typedef.h"
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

#define WIFI_SCAN_BUFFER_SIZE 32768U
#define WIFI_SCAN_TIMEOUT_MS  5000U
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

static void format_mac_from_sockaddr(const struct sockaddr* const addr, char* const dst, const size_t dst_size) {
    if ((addr == NULL) || (dst == NULL) || (dst_size < 18U)) {
        return;
    }

    const unsigned char* const bytes = (const unsigned char*)addr->sa_data;
    snprintf(dst,
             dst_size,
             "%02X:%02X:%02X:%02X:%02X:%02X",
             bytes[0],
             bytes[1],
             bytes[2],
             bytes[3],
             bytes[4],
             bytes[5]);
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

static int wifi_quality_to_dbm(const struct iw_quality qual) {
    if ((qual.updated & IW_QUAL_LEVEL_INVALID) != 0U) {
        return 0;
    }

    if (((qual.updated & IW_QUAL_DBM) != 0U) || (qual.level > 63U)) {
        return (qual.level >= 64U) ? ((int)qual.level - 0x100) : (int)qual.level;
    }

    return (int)qual.level;
}

static instrument_api_status_t wifi_wait_for_scan_results(const int       fd,
                                                          const char*     ifname,
                                                          unsigned char*  scan_buffer,
                                                          const size_t    scan_buffer_size,
                                                          struct iwreq*   req) {
    const struct timespec sleep_step = {
        .tv_sec  = 0,
        .tv_nsec = 100L * 1000L * 1000L,
    };
    uint32_t waited_ms = 0U;

    while (waited_ms <= WIFI_SCAN_TIMEOUT_MS) {
        memset(req, 0, sizeof(*req));
        copy_string(req->ifr_name, sizeof(req->ifr_name), ifname);
        req->u.data.pointer = scan_buffer;
        req->u.data.length  = (uint16_t)scan_buffer_size;
        req->u.data.flags   = 0U;

        if (ioctl(fd, SIOCGIWSCAN, req) == 0) {
            return INSTRUMENT_API_SUCCESS;
        }

        if ((errno != EAGAIN) && (errno != EBUSY)) {
            return INSTRUMENT_API_ERROR;
        }

        nanosleep(&sleep_step, NULL);
        waited_ms += 100U;
    }

    return INSTRUMENT_API_ERROR;
}

static instrument_api_status_t wifi_trigger_scan(const int fd, const char* const ifname) {
    struct iwreq req;
    memset(&req, 0, sizeof(req));
    copy_string(req.ifr_name, sizeof(req.ifr_name), ifname);
    req.u.data.pointer = NULL;
    req.u.data.length  = 0U;
    req.u.data.flags   = 0U;

    if (ioctl(fd, SIOCSIWSCAN, &req) != 0) {
        if ((errno == EPERM) || (errno == EOPNOTSUPP)) {
            return INSTRUMENT_API_NOT_SUPPORTED;
        }
        return INSTRUMENT_API_ERROR;
    }

    return INSTRUMENT_API_SUCCESS;
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

    int fd = -1;
    if (open_inet_dgram_socket(&fd) != INSTRUMENT_API_SUCCESS) {
        return INSTRUMENT_API_ERROR;
    }

    instrument_api_status_t status = wifi_trigger_scan(fd, ifname);
    if ((status != INSTRUMENT_API_SUCCESS) && (status != INSTRUMENT_API_NOT_SUPPORTED)) {
        close(fd);
        return status;
    }

    unsigned char scan_buffer[WIFI_SCAN_BUFFER_SIZE];
    struct iwreq  scan_req;
    status = wifi_wait_for_scan_results(fd, ifname, scan_buffer, sizeof(scan_buffer), &scan_req);
    close(fd);
    if (status != INSTRUMENT_API_SUCCESS) {
        return status;
    }

    WifiDeviceInfoBase* const devices = (WifiDeviceInfoBase*)output_data;
    uint32_t                  count   = 0U;
    WifiDeviceInfoBase*       current = NULL;

    const unsigned char* pos = scan_buffer;
    const unsigned char* end = scan_buffer + scan_req.u.data.length;

    while ((pos + IW_EV_LCP_PK_LEN) <= end) {
        uint16_t event_len = 0U;
        uint16_t event_cmd = 0U;
        memcpy(&event_len, pos, sizeof(event_len));
        memcpy(&event_cmd, pos + sizeof(event_len), sizeof(event_cmd));

        if ((event_len <= IW_EV_LCP_PK_LEN) || ((size_t)(end - pos) < event_len)) {
            break;
        }

        switch (event_cmd) {
        case SIOCGIWAP:
            if ((event_len >= IW_EV_ADDR_PK_LEN) && (count < WIFI_MAX_DEVICES)) {
                current = &devices[count];
                memset(current, 0, sizeof(*current));
                current->type = WIFI_ROUTER;
                struct sockaddr ap_addr;
                memset(&ap_addr, 0, sizeof(ap_addr));
                memcpy(&ap_addr, pos + IW_EV_LCP_PK_LEN, sizeof(ap_addr));
                format_mac_from_sockaddr(&ap_addr, current->mac_address, sizeof(current->mac_address));
                ++count;
            } else {
                current = NULL;
            }
            break;
        case SIOCGIWESSID:
            if ((current != NULL) && (event_len >= IW_EV_POINT_PK_LEN)) {
                uint16_t essid_len = 0U;
                memcpy(&essid_len, pos + IW_EV_LCP_PK_LEN, sizeof(essid_len));
                if (essid_len > 0U) {
                    const size_t payload_len = event_len - IW_EV_POINT_PK_LEN;
                    if (essid_len > payload_len) {
                        essid_len = (uint16_t)payload_len;
                    }
                    if (essid_len >= sizeof(current->ssid)) {
                        essid_len = (uint16_t)(sizeof(current->ssid) - 1U);
                    }
                    memcpy(current->ssid, pos + IW_EV_POINT_PK_LEN, essid_len);
                    current->ssid[essid_len] = '\0';
                }
            }
            break;
        case IWEVQUAL:
            if ((current != NULL) && (event_len >= IW_EV_QUAL_PK_LEN)) {
                struct iw_quality qual;
                memset(&qual, 0, sizeof(qual));
                memcpy(&qual, pos + IW_EV_LCP_PK_LEN, sizeof(qual));
                current->rssi.value_dbm = (int16_t)wifi_quality_to_dbm(qual);
                current->rssi.valid     = ((qual.updated & IW_QUAL_LEVEL_INVALID) == 0U);
            }
            break;
        default:
            break;
        }

        pos += event_len;
    }

    *output_len = count;
    return INSTRUMENT_API_SUCCESS;
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
