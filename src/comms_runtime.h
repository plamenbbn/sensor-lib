#ifndef COMMS_RUNTIME_H
#define COMMS_RUNTIME_H

#include "instrument_api.h"

#ifdef __cplusplus
extern "C" {
#endif

instrument_api_status_t comms_runtime_initialize(const InstrumentAPIConfig* config);
void comms_runtime_shutdown(void);

instrument_api_status_t comms_runtime_register_callback(InstrumentType instrument_type,
                                                        CallbackType callback_type,
                                                        InstrumentInputType input_data);
instrument_api_status_t comms_runtime_unregister_callback(InstrumentType instrument_type,
                                                          CallbackType callback_type,
                                                          InstrumentInputType input_data);

instrument_api_status_t comms_runtime_discover_links(InstrumentType instrument_type,
                                                     InstrumentOutputType output_data,
                                                     uint32_t* output_len);

#ifdef __cplusplus
}
#endif

#endif
