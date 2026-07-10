extern "C" {
#include "sensor_api.h"
}

#include <iostream>
#include <vector>

/**
 * @file bluetoothExample.cc
 * @brief A simple example of how to use the Bluetooth API
 * @details
 * Prerequisites:
 * 1. Bluetooth must be enabled on the device.
 * 2. Run with root privileges
 *
 * Expectations:
 * 1. Get Bluetooth adapter info
 * 2. Discover nearby Bluetooth devices (~10 seconds)
 * 3. Turn off the bluetooth adapter
 * 4. Check if bluetooth is on
 * 5. Turn on the bluetooth adapter
 * 6. Check if bluetooth is on
 */

static instrument_api_status_t getBluetoothAdapterInfo(const InstrumentAPI*                   api,
                                                       std::vector<BluetoothAdapterInfoBase>& btAdapters) {
    constexpr InstrumentActions action  = INSTRUMENT_BLUETOOTH_GET_ADAPTER_INFO;
    uint32_t                    infoLen = 0;

    std::cout << "Getting radio info" << std::endl;
    const auto result =
        api->instrumentAction(action, nullptr, 0, static_cast<InstrumentOutputType>(&btAdapters.front()), &infoLen);

    std::cout << "radioInfoLen: " << infoLen << std::endl;

    if (result != INSTRUMENT_API_SUCCESS) {
        std::cout << "Error: " << result << std::endl;
        return INSTRUMENT_API_ERROR;
    }
    if (infoLen > BLUETOOTH_MAX_ADAPTERS) {
        std::cout << "Error: Too many adapters" << std::endl;
        return INSTRUMENT_API_ERROR;
    }

    std::cout << "Got radio info" << std::endl;

    for (uint32_t i = 0; i < infoLen; i++) {
        std::cout << "****** Bluetooth Radio " << i << " ******" << std::endl;
        std::cout << "Bluetooth Radio ID: " << btAdapters[i].id << std::endl;
        std::cout << "Bluetooth Radio Name: " << btAdapters[i].name << std::endl;
        std::cout << "Bluetooth Radio Address: " << btAdapters[i].mac_address << std::endl;
        std::cout << "*****************************" << std::endl;
    }

    return INSTRUMENT_API_SUCCESS;
}

static instrument_api_status_t discoverBluetoothDevices(const InstrumentAPI* api, BluetoothAdapterInfoBase& adapter) {
    constexpr InstrumentActions          action = INSTRUMENT_BLUETOOTH_DISCOVER_DEVICES;
    std::vector<BluetoothDeviceInfoBase> btDeviceInfo(BLUETOOTH_MAX_DEVICES);
    uint32_t                             infoLen = 0;
    const auto                           result  = api->instrumentAction(action,
                                              static_cast<InstrumentInputType>(&adapter),
                                              1,
                                              static_cast<InstrumentOutputType>(&btDeviceInfo.front()),
                                              &infoLen);

    if (result != INSTRUMENT_API_SUCCESS) {
        std::cout << "Error: " << result << std::endl;
        return INSTRUMENT_API_ERROR;
    }

    std::cout << "Discovered " << infoLen << " devices" << std::endl;

    for (auto i = 0; i < infoLen; i++) {
        std::cout << "****** Bluetooth Device " << i << " ******" << std::endl;
        std::cout << "Bluetooth Device ID: " << btDeviceInfo[i].id << std::endl;
        std::cout << "Bluetooth Device Name: " << btDeviceInfo[i].name << std::endl;
        std::cout << "Bluetooth Device Address: " << btDeviceInfo[i].mac_address << std::endl;
        std::cout << "Bluetooth Device Class: " << btDeviceInfo[i].dev_class[0] << " " << btDeviceInfo[i].dev_class[1]
                  << " " << btDeviceInfo[i].dev_class[2] << std::endl;
    }

    return INSTRUMENT_API_SUCCESS;
}

static instrument_api_status_t isBluetoothOn(const InstrumentAPI* api, BluetoothAdapterInfoBase& adapter) {
    constexpr InstrumentActions action = INSTRUMENT_BLUETOOTH_IS_UP;
    std::cout << "Checking if Bluetooth adapter is on" << std::endl;

    bool     status;
    uint32_t outputLen{};

    const auto result = api->instrumentAction(
        action, static_cast<InstrumentInputType>(&adapter), 1, static_cast<InstrumentOutputType>(&status), &outputLen);
    if (result != INSTRUMENT_API_SUCCESS) {
        std::cout << "Error: " << result << std::endl;
        return INSTRUMENT_API_ERROR;
    }

    std::cout << "Bluetooth adapter up status: " << std::boolalpha << status << std::endl;

    return INSTRUMENT_API_SUCCESS;
}

static instrument_api_status_t turnBluetoothOn(const InstrumentAPI* api, BluetoothAdapterInfoBase& adapter) {
    constexpr InstrumentActions action = INSTRUMENT_BLUETOOTH_TURN_ON;
    std::cout << "Turning on Bluetooth adapter" << std::endl;
    const auto result = api->instrumentAction(action, static_cast<InstrumentInputType>(&adapter), 1, nullptr, nullptr);

    if (result != INSTRUMENT_API_SUCCESS) {
        std::cout << "Error: " << result << std::endl;
        return INSTRUMENT_API_ERROR;
    }
    return INSTRUMENT_API_SUCCESS;
}

static instrument_api_status_t turnBluetoothOff(const InstrumentAPI* api, BluetoothAdapterInfoBase& adapter) {
    constexpr InstrumentActions action = INSTRUMENT_BLUETOOTH_TURN_OFF;
    std::cout << "Turning off Bluetooth adapter" << std::endl;

    const auto result = api->instrumentAction(action, static_cast<InstrumentInputType>(&adapter), 1, nullptr, nullptr);
    if (result != INSTRUMENT_API_SUCCESS) {
        std::cout << "Error: " << result << std::endl;
        return INSTRUMENT_API_ERROR;
    }
    return INSTRUMENT_API_SUCCESS;
}

int main() {
    int                     out    = 1;
    instrument_api_status_t result = INSTRUMENT_API_ERROR;

    std::cout << "Creating instrument API" << std::endl;
    constexpr ServerConfig        serverConfig = {"192.168.1.1", 50052};
    constexpr LoggerConfig        loggerConfig = {INFO};
    constexpr InstrumentAPIConfig config{serverConfig, loggerConfig};
    InstrumentAPI*                api = createInstrumentAPI(config);
    if (api == nullptr) {
        std::cout << "Failed to create API: " << result << std::endl;
        return out;
    }
    std::cout << "Instrument API created" << std::endl;

    std::vector<BluetoothAdapterInfoBase> btAdapters(BLUETOOTH_MAX_ADAPTERS);

    // Create API action request to get radio info(s)
    result = getBluetoothAdapterInfo(api, btAdapters);
    if (result != INSTRUMENT_API_SUCCESS) {
        destroyInstrumentAPI(api);
        return out;
    }

    BluetoothAdapterInfoBase adapter = btAdapters[0];

    // Create API action to search for devices
    result = discoverBluetoothDevices(api, adapter);
    if (result != INSTRUMENT_API_SUCCESS) {
        goto cleanup;
    }

    // Create API action to turn off Bluetooth adapter
    result = turnBluetoothOff(api, adapter);
    if (result != INSTRUMENT_API_SUCCESS) {
        goto cleanup;
    }

    // Check Bluetooth Adapter status
    result = isBluetoothOn(api, adapter);
    if (result != INSTRUMENT_API_SUCCESS) {
        goto cleanup;
    }

    // Create API action to turn on Bluetooth adapter
    result = turnBluetoothOn(api, adapter);
    if (result != INSTRUMENT_API_SUCCESS) {
        goto cleanup;
    }

    // Check Bluetooth Adapter status
    result = isBluetoothOn(api, adapter);
    if (result != INSTRUMENT_API_SUCCESS) {
        goto cleanup;
    }

    out = 0;

cleanup:
    destroyInstrumentAPI(api);
    return out;
}
