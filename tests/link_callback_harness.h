#ifndef LINK_CALLBACK_HARNESS_H
#define LINK_CALLBACK_HARNESS_H

#include "instrument_api.h"

typedef enum {
    HARNESS_WIFI_MODE_CLIENT = 0,
    HARNESS_WIFI_MODE_HOTSPOT,
} HarnessWifiMode;

int run_link_callback_harness(const InstrumentAPI* api, unsigned interval_seconds, HarnessWifiMode wifi_mode);

#endif
