extern "C" {
#include "sensor_api.h"
}

#include <iostream>
#include <vector>

/**
 * @file wifiExample.cc
 * @brief A simple example of how to use the Wi-Fi API
 * @details
 * Prerequisites:
 * - Wi-Fi must be enabled on the device
 * - Run with root privileges
 *
 * Expectations:
 * 1. Get Wi-Fi adapter info
 * 2. Discover nearby Wi-Fi devices
 * 3. Check if Wi-Fi is on
 * 4. Turn off the Wi-Fi adapter
 * 5. Check if Wi-Fi is on
 * 6. Turn on the Wi-Fi adapter
 * 7. Check if Wi-Fi is on
 */

static instrument_api_status_t getWifiAdapterInfo(const InstrumentAPI*             api,
                                                  std::vector<WifiDeviceInfoBase>& wfDevices) {
    constexpr InstrumentActions action  = INSTRUMENT_WIFI_GET_MY_DEVICE;
    uint32_t                    infoLen = 0;

    std::cout << "Getting Adapter Info..." << std::endl;
    const auto result =
        api->instrumentAction(action, nullptr, 0, static_cast<InstrumentOutputType>(&wfDevices.front()), &infoLen);

    std::cout << "adapterInfoLen: " << infoLen << std::endl;

    if (result != INSTRUMENT_API_SUCCESS) {
        std::cout << "Error: " << result << std::endl;
        return INSTRUMENT_API_ERROR;
    }
    if (infoLen > WIFI_MAX_DEVICES) {
        std::cout << "Error: Too Many Adapters..." << std::endl;
        return INSTRUMENT_API_ERROR;
    }

    std::cout << "Got Adapter Info" << std::endl;

    for (uint32_t i = 0; i < infoLen; ++i) {
        std::cout << "****** Wi-Fi Adapter " << i << " ******" << std::endl;
        std::cout << "Wi-Fi Adapter SSID: " << wfDevices[i].ssid << std::endl;
        std::cout << "Wi-Fi Adapter MAC Address: " << wfDevices[i].mac_address << std::endl;
        std::cout << "Wi-Fi Adapter Device Type: " << (wfDevices[i].type == WIFI_ADAPTER ? "Adapter" : "Router")
                  << std::endl;
    }

    return INSTRUMENT_API_SUCCESS;
}

static instrument_api_status_t discoverWifiDevices(const InstrumentAPI* api, WifiDeviceInfoBase& device) {
    constexpr InstrumentActions     action = INSTRUMENT_WIFI_DISCOVER_DEVICES;
    std::vector<WifiDeviceInfoBase> wfDeviceInfo(WIFI_MAX_DEVICES);
    uint32_t                        infoLen = 0;
    const auto                      result  = api->instrumentAction(action,
                                              static_cast<InstrumentInputType>(&device),
                                              1,
                                              static_cast<InstrumentOutputType>(&wfDeviceInfo.front()),
                                              &infoLen);

    if (result != INSTRUMENT_API_SUCCESS) {
        std::cout << "Error: " << result << std::endl;
        return INSTRUMENT_API_ERROR;
    }

    std::cout << "Discovered " << infoLen << " Devices" << std::endl;

    for (uint32_t i = 0; i < infoLen; ++i) {
        std::cout << "****** Wi-Fi Device " << i << " ******" << std::endl;
        std::cout << "Wi-Fi Device SSID: " << wfDeviceInfo[i].ssid << std::endl;
        std::cout << "Wi-Fi Device MAC Address: " << wfDeviceInfo[i].mac_address << std::endl;
        std::cout << "Wi-Fi Device Type: " << (wfDeviceInfo[i].type == WIFI_ADAPTER ? "Adapter" : "Router")
                  << std::endl;
    }

    return INSTRUMENT_API_SUCCESS;
}

static instrument_api_status_t isWifiOn(const InstrumentAPI* api, WifiDeviceInfoBase& device) {
    constexpr InstrumentActions action = INSTRUMENT_WIFI_IS_UP;
    std::cout << "Checking if Wi-Fi Adapter is Up..." << std::endl;

    bool     status;
    uint32_t outputLen{};

    const auto result = api->instrumentAction(
        action, static_cast<InstrumentInputType>(&device), 1, static_cast<InstrumentOutputType>(&status), &outputLen);
    if (result != -INSTRUMENT_API_SUCCESS) {
        std::cout << "Error: " << result << std::endl;
        return INSTRUMENT_API_ERROR;
    }

    std::cout << "Wi-Fi Adapter Up Status: " << std::boolalpha << status << std::endl;

    return INSTRUMENT_API_SUCCESS;
}

static instrument_api_status_t turnWifiOn(const InstrumentAPI* api, WifiDeviceInfoBase& device) {
    constexpr InstrumentActions action = INSTRUMENT_WIFI_TURN_ON;
    std::cout << "Turning on Wi-Fi Adapter..." << std::endl;
    const auto result = api->instrumentAction(action, static_cast<InstrumentInputType>(&device), 1, nullptr, nullptr);

    if (result != INSTRUMENT_API_SUCCESS) {
        std::cout << "Error: " << result << std::endl;
        return INSTRUMENT_API_ERROR;
    }
    return INSTRUMENT_API_SUCCESS;
}

static instrument_api_status_t turnWifiOff(const InstrumentAPI* api, WifiDeviceInfoBase& device) {
    constexpr InstrumentActions action = INSTRUMENT_WIFI_TURN_OFF;
    std::cout << "Turning off Wi-Fi Adapter..." << std::endl;

    const auto result = api->instrumentAction(action, static_cast<InstrumentInputType>(&device), 1, nullptr, nullptr);
    if (result != INSTRUMENT_API_SUCCESS) {
        std::cout << "Error: " << result << std::endl;
        return INSTRUMENT_API_ERROR;
    }
    return INSTRUMENT_API_SUCCESS;
}

int main() {
    int                     out    = 1;
    instrument_api_status_t result = INSTRUMENT_API_ERROR;

    std::cout << "Creating Instrument API..." << std::endl;
    constexpr ServerConfig        serverConfig = {"192.168.1.1", 50052};
    constexpr LoggerConfig        loggerConfig = {INFO};
    constexpr InstrumentAPIConfig config{serverConfig, loggerConfig};
    InstrumentAPI*                api = createInstrumentAPI(config);
    if (api == nullptr) {
        std::cout << "Failed to create API: " << result << std::endl;
        return out;
    }
    std::cout << "Instrument API Created" << std::endl;

    std::vector<WifiDeviceInfoBase> wfDevices(WIFI_MAX_DEVICES);

    // Create API action request to get adapter info(s)
    result = getWifiAdapterInfo(api, wfDevices);
    if (result != INSTRUMENT_API_SUCCESS) {
        destroyInstrumentAPI(api);
        return out;
    }

    WifiDeviceInfoBase device = wfDevices[0];

    // Create API action to discover devices
    result = discoverWifiDevices(api, device);
    if (result != INSTRUMENT_API_SUCCESS) {
        destroyInstrumentAPI(api);
        return out;
    }

    // Check Wi-Fi Adapter status
    result = isWifiOn(api, device);
    if (result != INSTRUMENT_API_SUCCESS) {
        goto cleanup;
    }

    // Create API action to turn off Wi-Fi adapter
    result = turnWifiOff(api, device);
    if (result != INSTRUMENT_API_SUCCESS) {
        goto cleanup;
    }

    // Check Wi-Fi Adapter status
    result = isWifiOn(api, device);
    if (result != INSTRUMENT_API_SUCCESS) {
        goto cleanup;
    }

    // Create API action to turn on Wi-Fi adapter
    result = turnWifiOn(api, device);
    if (result != INSTRUMENT_API_SUCCESS) {
        goto cleanup;
    }

    // Check Wi-Fi Adapter status
    result = isWifiOn(api, device);
    if (result != INSTRUMENT_API_SUCCESS) {
        goto cleanup;
    }

    out = 0;

cleanup:
    destroyInstrumentAPI(api);
    return out;
}
