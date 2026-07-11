#ifndef WIFI_SCANNER_BRIDGE_H
#define WIFI_SCANNER_BRIDGE_H

#include "instrument_api.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

instrument_api_status_t wifi_discover_devices_nl80211(const char* ifname,
                                                       InstrumentOutputType output_data,
                                                       uint32_t* output_len);

#ifdef __cplusplus
}
#endif

#endif
