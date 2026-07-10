#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bluetooth_typedef.h"
#include "sensor_api.h"

/**
 * @file bluetoothExample.c
 * @brief A simple example of how to use the Bluetooth API in C
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

static instrument_api_status_t getBluetoothAdapterInfo(
    const InstrumentAPI* api, BluetoothAdapterInfoBase* adapters) {
    const InstrumentActions action = INSTRUMENT_BLUETOOTH_GET_ADAPTER_INFO;
    uint32_t infoLen = 0;

    printf("Getting radio info\n");
    const instrument_api_status_t result = api->instrumentAction(
        action, NULL, 0, (InstrumentOutputType)(adapters), &infoLen);

    printf("Number of Radios: %d\n", infoLen);

    if (result != INSTRUMENT_API_SUCCESS) {
        printf("Error: %d\n", result);
        return INSTRUMENT_API_ERROR;
    }
    if (infoLen > BLUETOOTH_MAX_ADAPTERS) {
        printf("Error: Too many adapters\n");
        return INSTRUMENT_API_ERROR;
    }

    printf("Got radio info %d\n", infoLen);

    for (uint32_t i = 0; i < infoLen; i++) {
        printf("****** Bluetooth Radio %d ******\n", i);
        printf("Bluetooth radio id: %d\n", (int)adapters[i].id);
        printf("Bluetooth radio name: %s\n", adapters[i].name);
        printf("Bluetooth radio address: %s\n", adapters[i].mac_address);
    }

    return INSTRUMENT_API_SUCCESS;
}

static instrument_api_status_t discoverBluetoothDevices(
    const InstrumentAPI* api, BluetoothAdapterInfoBase* adapter) {
    const InstrumentActions action = INSTRUMENT_BLUETOOTH_DISCOVER_DEVICES;
    BluetoothDeviceInfoBase* btDeviceInfo = (BluetoothDeviceInfoBase*)malloc(
        BLUETOOTH_MAX_DEVICES * sizeof(BluetoothDeviceInfoBase));
    uint32_t infoLen = 0;
    const instrument_api_status_t result =
        api->instrumentAction(action, (InstrumentInputType)(adapter), 1,
                              (InstrumentOutputType)(btDeviceInfo), &infoLen);

    if (result != INSTRUMENT_API_SUCCESS) {
        printf("Error: %d\n", result);
        return INSTRUMENT_API_ERROR;
    }

    printf("Discovered %d devices\n", infoLen);

    for (uint32_t i = 0; i < infoLen; i++) {
        printf("****** Bluetooth Device %d ******\n", i);
        printf("Bluetooth device id: %d\n", btDeviceInfo[i].id);
        printf("Bluetooth device name: %s\n", btDeviceInfo[i].name);
        printf("Bluetooth device address: %s\n", btDeviceInfo[i].mac_address);
        printf("Bluetooth device class: %d, %d, %d\n",
               btDeviceInfo[i].dev_class[0], btDeviceInfo[i].dev_class[1],
               btDeviceInfo[i].dev_class[2]);
    }

    free(btDeviceInfo);

    return INSTRUMENT_API_SUCCESS;
}

static instrument_api_status_t isBluetoothOn(
    const InstrumentAPI* api, BluetoothAdapterInfoBase* adapter) {
    const InstrumentActions action = INSTRUMENT_BLUETOOTH_IS_UP;
    printf("Checking if Bluetooth adapter is on\n");

    bool status;
    uint32_t infoLen = 0;

    const instrument_api_status_t result =
        api->instrumentAction(action, (InstrumentInputType)(adapter), 1,
                              (InstrumentOutputType)(&status), &infoLen);
    if (result != INSTRUMENT_API_SUCCESS) {
        printf("Error: %d\n", result);
        return INSTRUMENT_API_ERROR;
    }

    printf("Bluetooth adapter up status: %d\n", status);

    return INSTRUMENT_API_SUCCESS;
}

static instrument_api_status_t turnBluetoothOn(
    const InstrumentAPI* api, BluetoothAdapterInfoBase* adapter) {
    const InstrumentActions action = INSTRUMENT_BLUETOOTH_TURN_ON;
    printf("Turning on Bluetooth adapter\n");
    const instrument_api_status_t result = api->instrumentAction(
        action, (InstrumentInputType)(adapter), 1, NULL, NULL);

    if (result != INSTRUMENT_API_SUCCESS) {
        printf("Error: %d\n", result);
        return INSTRUMENT_API_ERROR;
    }
    return INSTRUMENT_API_SUCCESS;
}

static instrument_api_status_t turnBluetoothOff(
    const InstrumentAPI* api, BluetoothAdapterInfoBase* adapter) {
    const InstrumentActions action = INSTRUMENT_BLUETOOTH_TURN_OFF;
    printf("Turning off Bluetooth adapter\n");
    const instrument_api_status_t result = api->instrumentAction(
        action, (InstrumentInputType)(adapter), 1, NULL, NULL);

    if (result != INSTRUMENT_API_SUCCESS) {
        printf("Error: %d\n", result);
        return INSTRUMENT_API_ERROR;
    }
    return INSTRUMENT_API_SUCCESS;
}

int main() {
    printf("Creating instrument API\n");
    int out = 1;
    const InstrumentAPIConfig config = {};
    InstrumentAPI* api = createInstrumentAPI(config);
    if (api == NULL) {
        printf("Error: %d", INSTRUMENT_API_ERROR);
        return 1;
    }
    printf("Instrument API created\n");

    instrument_api_status_t result = INSTRUMENT_API_ERROR;

    BluetoothAdapterInfoBase* btAdapters = (BluetoothAdapterInfoBase*)malloc(
        BLUETOOTH_MAX_ADAPTERS * sizeof(BluetoothAdapterInfoBase));

    // Create API action request to get radio info(s)
    result = getBluetoothAdapterInfo(api, btAdapters);
    if (result != INSTRUMENT_API_SUCCESS) {
        destroyInstrumentAPI(api);
        return out;
    }

    BluetoothAdapterInfoBase adapter = btAdapters[0];

    // Create API action to search for devices
    result = discoverBluetoothDevices(api, &adapter);
    if (result != INSTRUMENT_API_SUCCESS) {
        goto cleanup;
    }

    // Create API action to turn off Bluetooth adapter
    result = turnBluetoothOff(api, &adapter);
    if (result != INSTRUMENT_API_SUCCESS) {
        goto cleanup;
    }

    // Check Bluetooth Adapter status
    result = isBluetoothOn(api, &adapter);
    if (result != INSTRUMENT_API_SUCCESS) {
        goto cleanup;
    }

    // Create API action to turn on Bluetooth adapter
    result = turnBluetoothOn(api, &adapter);
    if (result != INSTRUMENT_API_SUCCESS) {
        goto cleanup;
    }

    // Check Bluetooth Adapter status
    result = isBluetoothOn(api, &adapter);
    if (result != INSTRUMENT_API_SUCCESS) {
        goto cleanup;
    }

    out = 0;

cleanup:
    destroyInstrumentAPI(api);
    free(btAdapters);
    return out;
}