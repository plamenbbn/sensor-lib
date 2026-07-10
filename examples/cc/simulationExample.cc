#ifdef __cplusplus
extern "C" {
#endif
#include "sensor_api.h"
#ifdef __cplusplus
}
#endif

#include <chrono>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>
#include <time.h> // POSIX gmtime_r
#include <vector>

/**
 * @file simulationExample.cc
 * @brief A simple example of Bluetooth functionality known to work with
 * AndurilSim
 * @details
 *
 * Expectations:
 * doSimulationActions:
 * 1. Get the current simulation state and print the simulation start time,
 * clock time, and play rate.
 *
 * doCellularActions:
 * 1. Geolocate a target device using their phone number (MSISDN)
 *
 * doGpsActions:
 * 1. Return the GPS position of this entity.
 *
 * doBluetoothScan:
 * 1. Discover devices
 * 2. If the target device is discovered, log its information.
 */

static void doSimulationActions(const InstrumentAPI* apiHandle, const int delay) {
    while (true) {
        constexpr InstrumentActions action = INSTRUMENT_SIMULATION_GET_STATE;
        C_SimState                  simState;
        uint32_t                    outputLen{};

        const auto result =
            apiHandle->instrumentAction(action, nullptr, 0, static_cast<InstrumentOutputType>(&simState), &outputLen);

        if (result != INSTRUMENT_API_SUCCESS) {
            std::cout << "Error: " << result << std::endl;
            break;
        }
        if (!outputLen) {
            std::cout << "Empty response! " << std::endl;
        } else {
            std::tm utc_starttime{};
            gmtime_r(&simState.startTime, &utc_starttime);
            std::stringstream ss_utc_starttime;
            ss_utc_starttime << std::put_time(&utc_starttime, "%Y-%m-%d %H:%M:%S");

            std::tm utc_clocktime{};
            gmtime_r(&simState.clockTime, &utc_clocktime);
            std::stringstream ss_utc_clocktime;
            ss_utc_clocktime << std::put_time(&utc_clocktime, "%Y-%m-%d %H:%M:%S");

            std::cout << "Current simulation state: " << std::endl;
            std::cout << "    Start Time: " << ss_utc_starttime.str() << std::endl;
            std::cout << "    Clock Time: " << ss_utc_clocktime.str() << std::endl;
            std::cout << "     Play Rate: " << simState.playRate << std::endl;
            std::cout << std::boolalpha << "     Paused:    " << simState.paused << std::endl;
        }

        std::this_thread::sleep_for(std::chrono::seconds(delay));
    }
}

static void doCellularActions(const InstrumentAPI* apiHandle, const int delay) {
    while (true) {
        char targetMSISDN[] = "8772274669";

        std::cout << "Geolocating target device with MSISDN " << targetMSISDN << std::endl;

        // Make a get position action
        constexpr InstrumentActions action = INSTRUMENT_CELLULAR_GEOLOCATE;
        std::vector<C_PositionInfo> positionInfo(1);
        uint32_t                    outputLen{};

        const auto result = apiHandle->instrumentAction(action,
                                                        static_cast<InstrumentInputType>(targetMSISDN),
                                                        1,
                                                        static_cast<InstrumentOutputType>(&positionInfo.front()),
                                                        &outputLen);

        if (result != INSTRUMENT_API_SUCCESS) {
            std::cout << "Error: " << result << std::endl;
            break;
        }
        if (!outputLen) {
            std::cout << "Empty response! " << std::endl;
        } else {
            std::cout << "Cellular device with MSISDN " << targetMSISDN << " has been geolocated!" << std::endl;
            std::cout << "    Timestamp: " << positionInfo[0].timestamp << std::endl;
            std::cout << "    Position: " << positionInfo[0].position.lat << ", " << positionInfo[0].position.lon
                      << ", " << positionInfo[0].position.alt << std::endl;
        }

        std::this_thread::sleep_for(std::chrono::seconds(delay));
    }
}

static void doGpsActions(const InstrumentAPI* apiHandle, const int delay) {
    while (true) {
        std::cout << "Getting device location using GPS" << std::endl;

        // Make a get position action
        constexpr InstrumentActions action = INSTRUMENT_GPS_GET_POSITION;
        std::vector<C_PositionInfo> positionInfo(1);
        uint32_t                    outputLen{};

        const auto result = apiHandle->instrumentAction(
            action, nullptr, 0, static_cast<InstrumentOutputType>(&positionInfo.front()), &outputLen);

        if (result != INSTRUMENT_API_SUCCESS) {
            std::cout << "Error: " << result << std::endl;
            break;
        }
        if (!outputLen) {
            std::cout << "Empty response! " << std::endl;
        } else {
            std::cout << "Acquired GPS position data: " << std::endl;
            std::cout << "    Timestamp: " << positionInfo[0].timestamp << std::endl;
            std::cout << "    Position: " << positionInfo[0].position.lat << ", " << positionInfo[0].position.lon
                      << ", " << positionInfo[0].position.alt << std::endl;
        }

        std::this_thread::sleep_for(std::chrono::seconds(delay));
    }
}

static void doBluetoothScan(const InstrumentAPI*                   apiHandle,
                            const int                              delay,
                            std::vector<BluetoothAdapterInfoBase>& btAdapterOutput) {
    static std::random_device rd;

    while (true) {
        // Discover Devices for Adapter - adapter1_mac
        constexpr auto                       action   = INSTRUMENT_BLUETOOTH_DISCOVER_DEVICES;
        constexpr uint32_t                   inputLen = 1;
        InstrumentInputType                  inputData{btAdapterOutput.data()};
        std::vector<BluetoothDeviceInfoBase> btDeviceInfo(BLUETOOTH_MAX_DEVICES);
        uint32_t                             outputLen = {};
        std::cout << "Discover Devices - Adapter " << btAdapterOutput[0].mac_address << std::endl;
        const auto result = apiHandle->instrumentAction(
            action, &inputData, inputLen, static_cast<InstrumentOutputType>(btDeviceInfo.data()), &outputLen);
        if (result != INSTRUMENT_API_SUCCESS) {
            std::cout << "Error: " << result << std::endl;
            return;
        }
        if (!outputLen) {
            std::cout << "No devices found! " << std::endl;
        } else {
            // Print out the discovered devices
            std::cout << "Discovered " << outputLen << " devices" << std::endl;
            if (outputLen) {
                bool foundTarget = false;
                for (auto i = 0; i < outputLen; i++) {
                    const auto target = "3A:D2:D3:AE:F0:8F";
                    if (strcmp(target, btDeviceInfo[i].mac_address) == 0) {
                        foundTarget = true;
                    }
                    std::cout << "****** Bluetooth Device " << i << " ******" << std::endl;
                    std::cout << "Bluetooth device name:    " << btDeviceInfo[i].name << std::endl;
                    std::cout << "Bluetooth device address: " << btDeviceInfo[i].mac_address << std::endl;
                    if (foundTarget) {
                        std::cout << "FOUND TARGET DEVICE: " << target << std::endl;
                        std::cout << "Target device name is " << btDeviceInfo[i].name << std::endl;
                    }
                }
                std::cout << "****************************" << std::endl;
            }
        }

        std::this_thread::sleep_for(std::chrono::seconds(delay));
    }
}

int main() {
    // Create an instrument API
    const auto                logFile      = "./logs/simulationExample.log";
    constexpr ServerConfig    serverConfig = {"192.168.1.1", 50052};
    const LoggerConfig        loggerConfig = {INFO, LOG_FILE, logFile};
    const InstrumentAPIConfig config{serverConfig, loggerConfig};

    InstrumentAPI* api = createInstrumentAPI(config);

    // Make a Get Adapter Info action
    constexpr auto                        action = INSTRUMENT_BLUETOOTH_GET_ADAPTER_INFO;
    std::vector<BluetoothAdapterInfoBase> btAdapterOutput(BLUETOOTH_MAX_ADAPTERS);
    uint32_t                              outputLen{};
    const instrument_api_status_t         result = api->instrumentAction(
        action, nullptr, 0, static_cast<InstrumentOutputType>(btAdapterOutput.data()), &outputLen);
    std::cout << "Get Adapter Info" << std::endl;
    bool has_bluetooth = true;
    if (result == INSTRUMENT_API_NOT_SUPPORTED) {
        has_bluetooth = false;
    } else if (result != INSTRUMENT_API_SUCCESS) {
        std::cout << "Error getting adapter info: " << result << std::endl;
        return 1;
    } else if (!outputLen) {
        std::cout << "Empty response! " << std::endl;
        return 1;
    } else {
        std::cout << "Got " << outputLen << " adapters" << std::endl;
        for (auto i = 0; i < outputLen; i++) {
            std::cout << "****** Bluetooth Radio " << i << " ******" << std::endl;
            std::cout << "Bluetooth Radio ID: " << btAdapterOutput[i].id << std::endl;
            std::cout << "Bluetooth Radio Name: " << btAdapterOutput[i].name << std::endl;
            std::cout << "Bluetooth Radio Address: " << btAdapterOutput[i].mac_address << std::endl;
        }
        std::cout << "****************************" << std::endl;
    }

    // Run threads
    std::thread simulationThread(doSimulationActions, api, 10);
    std::thread cellularThread(doCellularActions, api, 3);
    std::thread gpsThread(doGpsActions, api, 3);

    std::thread bluetoothThread;
    if (has_bluetooth) {
        bluetoothThread = std::thread(doBluetoothScan, api, 5, std::ref(btAdapterOutput));
    }

    // Join threads
    if (simulationThread.joinable()) {
        simulationThread.join();
    }
    if (cellularThread.joinable()) {
        cellularThread.join();
    }
    if (gpsThread.joinable()) {
        gpsThread.join();
    }
    if (has_bluetooth && bluetoothThread.joinable()) {
        bluetoothThread.join();
    }

    destroyInstrumentAPI(api);

    return 0;
}
