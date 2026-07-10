#ifndef INSTRUMENT_API_H
#define INSTRUMENT_API_H

/**
 * @file instrument_api.h
 * @brief The Instrument API interface.
 */

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

#include "action_typedef.h"
#include "callback_typedef.h"
#include "logger_typedef.h"

#include <stdbool.h>

typedef struct {
    char     addr[30];
    uint16_t port;
    bool     nonblocking_rpc;
} ServerConfig;

typedef struct {
    ServerConfig serverConfig;
    LoggerConfig loggerConfig;
} InstrumentAPIConfig;

/**
 * @brief Structure representing the Instrument API.
 */
typedef struct {
    /**
     * @brief Function pointer for instrument action.
     *
     * This is the main API function to perform an Instrument Action. It is generically typed and will accept any
     * defined Instrument Action and input/output data types that have been casted to a void pointer. The API expects
     * the caller to handle the allocation and deallocation of the input/output data. The caller must also use a data
     * type that is contiguous in memory and pass a pointer to the first element in memory(ie. think array in C). This
     * is because the API traverses input data and populates output data sequentially in memory. To allow for easier
     * use, it is required that a user preallocates the input/output data types to a max size (eg.
     * BLUETOOTH_MAX_ADAPTERS in bluetooth_typedef.h).
     *
     * The API call returns a simple and generic status code which can be expanded upon in the future.
     *
     * @param action The instrument action to perform.
     * @param input_data The input data for the action.
     * @param input_len The number of objects in the input array.
     * @param output_data The output data from the action.
     * @param output_len The number of the output objects.
     * @return The status of the instrument action.
     */
    instrument_api_status_t (*instrumentAction)(InstrumentActions         action,
                                                const InstrumentInputType input_data,
                                                const uint32_t            input_len,
                                                InstrumentOutputType      output_data,
                                                uint32_t*                 output_len);

    /**
     * @brief Function pointer for registering callbacks.
     *
     * Callbacks are fired sequentially and run in the same thread as scanning.
     *
     * @param instrument_type The instrument type to register a callback for.
     * @param callback_type The type of callback to register.
     * @param input_data The input data for the action.
     * @return The status of the instrument action.
     */
    instrument_api_status_t (*registerCallback)(InstrumentType            instrument_type,
                                                CallbackType              callback_type,
                                                const InstrumentInputType input_data);

    /**
     * @brief Function pointer for unregistering callbacks.
     *
     * @param instrument_type The instrument type to unregister a callback for.
     * @param callback_type The type of callback to unregister.
     * @param input_data The input data for the action.
     * @return The status of the instrument action.
     */
    instrument_api_status_t (*unregisterCallback)(InstrumentType            instrument_type,
                                                  CallbackType              callback_type,
                                                  const InstrumentInputType input_data);

    /**
     * @brief The config for the instrument API.
     */
    InstrumentAPIConfig interfaceConfig;
} InstrumentAPI;

/**
 * @brief Creates an instance of the Instrument API.
 * @param config The configuration for the instrument API.
 *
 * The user can call this function to get a handle to the Instrument API. This function allocates space on
 * the heap, so `destroyInstrumentAPI` should be called when the API is no longer needed.
 *
 * @return A pointer to the created InstrumentAPI object.
 */
InstrumentAPI* createInstrumentAPI(const InstrumentAPIConfig config);

/**
 * @brief Destroys the Instrument API instance.
 *
 * This function is used to destroy the Instrument API instance and must be called after `createInstrumentAPI`.
 *
 * @param instrumentAPI The InstrumentAPI object to destroy.
 */
void destroyInstrumentAPI(InstrumentAPI* instrumentAPI);

/**
 * @brief Gets the Instrument API version.
 *
 * @return The Instrument API version.
 */
const char* getInstrumentAPIVersion();

/**
 * @brief Gets the Instrument API platform.
 *
 * @return The Instrument API platform.
 */
const char* getInstrumentAPIPlatform();

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // INSTRUMENT_API_H
