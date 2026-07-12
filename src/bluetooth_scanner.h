#ifndef BLUETOOTH_SCANNER_H
#define BLUETOOTH_SCANNER_H

#include "instrument_api.h"

#ifdef __cplusplus
extern "C" {
#endif

instrument_api_status_t bluetooth_discover_devices_backend(const InstrumentInputType input_data,
                                                           uint32_t input_len,
                                                           InstrumentOutputType output_data,
                                                           uint32_t* output_len);

#ifdef __cplusplus
}
#endif

#endif
