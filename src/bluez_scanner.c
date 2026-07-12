#include "bluetooth_scanner.h"

#include "bluetooth_typedef.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BLUEZ_SCAN_SECONDS 6U
#define BLUEZ_SCAN_OUTPUT_MAX (256U * 1024U)
#define BLUEZ_INFO_OUTPUT_MAX (16U * 1024U)

typedef struct {
    char mac_address[18];
    char name[256];
} BluezScanEntry;

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

static void trim_trailing_whitespace(char* const text) {
    if (text == NULL) {
        return;
    }

    size_t len = strlen(text);
    while ((len > 0U) && isspace((unsigned char)text[len - 1U])) {
        text[--len] = '\0';
    }
}

static void strip_ansi(char* const text) {
    if (text == NULL) {
        return;
    }

    char* src = text;
    char* dst = text;
    while (*src != '\0') {
        if ((src[0] == '\033') && (src[1] == '[')) {
            src += 2;
            while ((*src != '\0') && ((*src < '@') || (*src > '~'))) {
                ++src;
            }
            if (*src != '\0') {
                ++src;
            }
            continue;
        }
        *dst++ = *src++;
    }
    *dst = '\0';
}

static bool read_command_output(const char* const command, char* const output, const size_t output_size) {
    if ((command == NULL) || (output == NULL) || (output_size == 0U)) {
        return false;
    }

    FILE* const pipe = popen(command, "r");
    if (pipe == NULL) {
        output[0] = '\0';
        return false;
    }

    size_t used = 0U;
    while ((used + 1U) < output_size) {
        const size_t bytes = fread(output + used, 1U, output_size - used - 1U, pipe);
        used += bytes;
        if (bytes == 0U) {
            break;
        }
    }
    output[used] = '\0';

    const int rc = pclose(pipe);
    return rc == 0;
}

static EBluetoothDeviceMajor bluetooth_major_from_cod(const uint32_t class_of_device) {
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

static bool looks_like_mac(const char* const text) {
    if (text == NULL) {
        return false;
    }

    for (int i = 0; i < 17; ++i) {
        const char ch = text[i];
        if (((i + 1) % 3) == 0) {
            if (ch != ':') {
                return false;
            }
        } else if (!isxdigit((unsigned char)ch)) {
            return false;
        }
    }

    return text[17] == '\0';
}

static bool parse_scan_line(const char* const line, BluezScanEntry* const entry) {
    if ((line == NULL) || (entry == NULL)) {
        return false;
    }

    const char* device = strstr(line, "Device ");
    if (device == NULL) {
        return false;
    }
    device += strlen("Device ");

    char mac[18];
    memset(mac, 0, sizeof(mac));
    strncpy(mac, device, sizeof(mac) - 1U);
    mac[17] = '\0';
    if (!looks_like_mac(mac)) {
        return false;
    }

    copy_string(entry->mac_address, sizeof(entry->mac_address), mac);
    entry->name[0] = '\0';

    const char* name = device + 17;
    while ((*name != '\0') && isspace((unsigned char)*name)) {
        ++name;
    }
    copy_string(entry->name, sizeof(entry->name), name);
    trim_trailing_whitespace(entry->name);
    return true;
}

static bool find_or_add_entry(BluezScanEntry* const entries,
                              uint32_t* const       count,
                              const BluezScanEntry* const incoming,
                              BluezScanEntry** const out_entry) {
    for (uint32_t i = 0U; i < *count; ++i) {
        if (strcmp(entries[i].mac_address, incoming->mac_address) == 0) {
            *out_entry = &entries[i];
            return true;
        }
    }

    if (*count >= BLUETOOTH_MAX_DEVICES) {
        return false;
    }

    entries[*count] = *incoming;
    *out_entry = &entries[*count];
    ++(*count);
    return true;
}

static void parse_scan_output(const char* const output, BluezScanEntry* const entries, uint32_t* const count) {
    char* buffer = strdup(output != NULL ? output : "");
    if (buffer == NULL) {
        return;
    }

    char* saveptr = NULL;
    for (char* line = strtok_r(buffer, "\n", &saveptr); line != NULL; line = strtok_r(NULL, "\n", &saveptr)) {
        strip_ansi(line);

        BluezScanEntry parsed;
        memset(&parsed, 0, sizeof(parsed));
        if (!parse_scan_line(line, &parsed)) {
            continue;
        }

        BluezScanEntry* slot = NULL;
        if (!find_or_add_entry(entries, count, &parsed, &slot)) {
            continue;
        }

        if ((slot->name[0] == '\0') && (parsed.name[0] != '\0')) {
            copy_string(slot->name, sizeof(slot->name), parsed.name);
        }
    }

    free(buffer);
}

static void parse_class_property(const char* const info_output, BluetoothDeviceInfoBase* const device) {
    const char* class_line = strstr(info_output, "\tClass:");
    if (class_line == NULL) {
        return;
    }

    const char* value = strstr(class_line, "0x");
    if (value == NULL) {
        return;
    }

    char* end = NULL;
    const unsigned long parsed = strtoul(value, &end, 16);
    if (end == value) {
        return;
    }

    device->dev_class[0] = (uint8_t)(parsed & 0xFFU);
    device->dev_class[1] = (uint8_t)((parsed >> 8U) & 0xFFU);
    device->dev_class[2] = (uint8_t)((parsed >> 16U) & 0xFFU);
    device->major = bluetooth_major_from_cod((uint32_t)parsed);
}

static void parse_rssi_property(const char* const info_output, BluetoothDeviceInfoBase* const device) {
    const char* rssi_line = strstr(info_output, "\tRSSI:");
    if (rssi_line == NULL) {
        return;
    }

    const char* value = rssi_line + strlen("\tRSSI:");
    while ((*value != '\0') && isspace((unsigned char)*value)) {
        ++value;
    }

    char* end = NULL;
    const long parsed = strtol(value, &end, 10);
    if (end == value) {
        return;
    }

    device->rssi.valid = true;
    device->rssi.value_dbm = (int16_t)parsed;
}

static void enrich_device_info(BluetoothDeviceInfoBase* const device) {
    char command[128];
    snprintf(command, sizeof(command), "bluetoothctl info %s 2>/dev/null", device->mac_address);

    char info_output[BLUEZ_INFO_OUTPUT_MAX];
    if (!read_command_output(command, info_output, sizeof(info_output))) {
        return;
    }

    const char* name_line = strstr(info_output, "\tName:");
    if ((device->name[0] == '\0') && (name_line != NULL)) {
        name_line += strlen("\tName:");
        while ((*name_line != '\0') && isspace((unsigned char)*name_line)) {
            ++name_line;
        }
        copy_string(device->name, sizeof(device->name), name_line);
        trim_trailing_whitespace(device->name);
    }

    parse_class_property(info_output, device);
    parse_rssi_property(info_output, device);
}

instrument_api_status_t bluetooth_discover_devices_backend(const InstrumentInputType input_data,
                                                           const uint32_t            input_len,
                                                           InstrumentOutputType      output_data,
                                                           uint32_t* const           output_len) {
    (void)input_data;
    (void)input_len;

    if ((output_data == NULL) || (output_len == NULL)) {
        return INSTRUMENT_API_ERROR;
    }

    char scan_output[BLUEZ_SCAN_OUTPUT_MAX];
    if (!read_command_output("bluetoothctl --timeout 6 scan on 2>/dev/null", scan_output, sizeof(scan_output))) {
        return INSTRUMENT_API_ERROR;
    }

    BluezScanEntry discovered[BLUETOOTH_MAX_DEVICES];
    memset(discovered, 0, sizeof(discovered));
    uint32_t discovered_count = 0U;
    parse_scan_output(scan_output, discovered, &discovered_count);

    BluetoothDeviceInfoBase* const devices = (BluetoothDeviceInfoBase*)output_data;
    for (uint32_t i = 0U; i < discovered_count; ++i) {
        memset(&devices[i], 0, sizeof(devices[i]));
        devices[i].id = (uint16_t)i;
        copy_string(devices[i].mac_address, sizeof(devices[i].mac_address), discovered[i].mac_address);
        copy_string(devices[i].name, sizeof(devices[i].name), discovered[i].name);
        devices[i].major = UNCATEGORIZED;
        devices[i].rssi.valid = false;
        enrich_device_info(&devices[i]);
    }

    *output_len = discovered_count;
    return INSTRUMENT_API_SUCCESS;
}
