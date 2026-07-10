#ifdef __cplusplus
extern "C" {
#endif
#include "sensor_api.h"
#ifdef __cplusplus
}
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

InstrumentAPI*              g_api;
std::atomic<bool>           g_isRunning = true;
std::vector<std::thread>    g_commsThreads;
std::mutex                  g_commsThreadsMut;
std::mutex                  g_executionMut;
std::condition_variable_any g_executionCondition;

std::string linkTypeToString(const BratislavaLinkType color) {
    switch (color) {
    case LINK_TYPE_BLUE:
        return {"Blue"};
    case LINK_TYPE_GREEN:
        return {"Green"};
    case LINK_TYPE_ORANGE:
        return {"Orange"};
    default:
        return {"Unknown"};
    }
}

void signal_handler(const int signum) {
    std::cout << "Interrupt signal (" << signum << ") received." << std::endl;
    std::cout << "Cleaning up and exiting...\n" << std::endl;

    g_isRunning = false;
    g_executionCondition.notify_all();
}

static void doLinkRecv(const int delay, const BratislavaLink* link) {
    uint32_t      messageLength              = 0;
    const ssize_t bytes_received_message_len = bratislavaLinkRecv(link, &messageLength, sizeof(messageLength));
    if (bytes_received_message_len <= 0) {
        switch (errno) {
        case ETIMEDOUT:
            std::cout << "Timeout occurred while receiving messages." << std::endl;
        case EPIPE:
            std::cout << "Device is no longer detected. Disconnecting." << std::endl;
        default:
            std::cout << "Failed to receive message length." << std::endl;
        }
        return;
    }

    const auto    messageBuffer          = new char[messageLength];
    const ssize_t bytes_received_message = bratislavaLinkRecv(link, messageBuffer, messageLength);
    if (bytes_received_message <= 0) {
        switch (errno) {
        case ETIMEDOUT:
            std::cout << "Timeout occurred while receiving messages." << std::endl;
        case EPIPE:
            std::cout << "Device is no longer detected. Disconnecting." << std::endl;
        default:
            std::cout << "Failed to receive message." << std::endl;
            return;
        }
    }

    const std::string message(messageBuffer);
    delete[] messageBuffer;

    std::cout << "Received Message: " << message << std::endl;
}

static void doLinkSend(const int delay, const BratislavaLink* link, const std::string& message) {
    // Send message length, followed by message
    const uint32_t messageLength = message.size() + 1; // +1 for null-terminator
    const int      status        = bratislavaLinkSend(link, &messageLength, sizeof(messageLength));

    if (status == -1) {
        if (errno == EPIPE) {
            std::cout << "Device is no longer detected. Disconnecting." << std::endl;
        } else {
            std::cout << "Failed to send message length." << std::endl;
        }
        return;
    }

    if (bratislavaLinkSend(link, message.c_str(), message.size() + 1) == -1) {
        std::cout << "Failed to send message." << std::endl;
    }
}

static int StartWifi() {
    // Start scanning on Wi-Fi
    std::cout << "Starting Wifi Scanning" << std::endl;
    std::vector<WifiDeviceInfoBase> wfDevices(WIFI_MAX_DEVICES);
    uint32_t                        infoLen = 0;
    auto                            result  = g_api->instrumentAction(
        INSTRUMENT_WIFI_GET_MY_DEVICE, nullptr, 0, static_cast<InstrumentOutputType>(&wfDevices.front()), &infoLen);

    if (result != INSTRUMENT_API_SUCCESS) {
        std::cout << "No wifi devices" << std::endl;
        return -1;
    }

    WifiDeviceInfoBase myWifi = wfDevices[0];
    bool               status;
    uint32_t           outputLen{};

    // Check if Wi-Fi is on
    result = g_api->instrumentAction(INSTRUMENT_WIFI_IS_UP,
                                     static_cast<InstrumentInputType>(&myWifi),
                                     1,
                                     static_cast<InstrumentOutputType>(&status),
                                     &outputLen);
    if (result != INSTRUMENT_API_SUCCESS) {
        std::cout << "Failed to get wifi status." << std::endl;
        return -1;
    }

    // If Wi-Fi is not on, make it so
    if (!status) {
        result = g_api->instrumentAction(
            INSTRUMENT_WIFI_TURN_ON, static_cast<InstrumentInputType>(&myWifi), 1, nullptr, nullptr);
        if (result != INSTRUMENT_API_SUCCESS) {
            std::cout << "Could not turn on Wifi" << std::endl;
            return -1;
        }
    }

    // Wi-Fi is on. Start Scanning
    result = g_api->instrumentAction(INSTRUMENT_WIFI_SCAN_ON, nullptr, 0, nullptr, 0);
    if (result != INSTRUMENT_API_SUCCESS) {
        std::cout << "Failed to begin scanning" << std::endl;
        return -1;
    }

    return 0;
}

static int StartBluetooth() {
    // Start scanning on Bluetooth
    std::cout << "Starting Bluetooth Scanning" << std::endl;
    std::vector<BluetoothDeviceInfoBase> btDevices(BLUETOOTH_MAX_ADAPTERS);
    uint32_t                             infoLen = 0;
    auto                                 result  = g_api->instrumentAction(INSTRUMENT_BLUETOOTH_GET_ADAPTER_INFO,
                                          nullptr,
                                          0,
                                          static_cast<InstrumentOutputType>(&btDevices.front()),
                                          &infoLen);

    if (result != INSTRUMENT_API_SUCCESS) {
        std::cout << "No Bluetooth devices" << std::endl;
        return -1;
    }

    BluetoothDeviceInfoBase myBT = btDevices[0];
    bool                    status;
    uint32_t                outputLen{};

    // Check if Bluetooth is on
    result = g_api->instrumentAction(INSTRUMENT_BLUETOOTH_IS_UP,
                                     static_cast<InstrumentInputType>(&myBT),
                                     1,
                                     static_cast<InstrumentOutputType>(&status),
                                     &outputLen);
    if (result != INSTRUMENT_API_SUCCESS) {
        std::cout << "Failed to get bluetooth status." << std::endl;
        return -1;
    }

    // If Bluetooth is not on, make it so
    if (!status) {
        result = g_api->instrumentAction(
            INSTRUMENT_BLUETOOTH_TURN_ON, static_cast<InstrumentInputType>(&myBT), 1, nullptr, nullptr);
        if (result != INSTRUMENT_API_SUCCESS) {
            std::cout << "Could not turn on Bluetooth" << std::endl;
            return -1;
        }
    }

    // Bluetooth is on. Start Scanning
    result = g_api->instrumentAction(INSTRUMENT_BLUETOOTH_SCAN_ON, nullptr, 0, nullptr, 0);
    if (result != INSTRUMENT_API_SUCCESS) {
        std::cout << "Failed to begin scanning" << std::endl;
        return -1;
    }

    return 0;
}

int main() {
    constexpr ServerConfig        serverConfig = {"192.168.1.1", 50052};
    constexpr InstrumentAPIConfig config{serverConfig};

    g_api = createInstrumentAPI(config);

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    if (0 != StartWifi()) {
        std::cout << "Wifi is unavailable" << std::endl;
    } else {
        std::cout << "Wifi Scanning started" << std::endl;
    }

    if (0 != StartBluetooth()) {
        std::cout << "Bluetooth is unavailable" << std::endl;
    } else {
        std::cout << "Bluetooth scanning started" << std::endl;
    }

    while (g_isRunning) {
        {
            std::lock_guard<std::mutex> lock(g_executionMut);
            g_executionCondition.wait_for(g_executionMut, std::chrono::seconds(5), [&] { return !g_isRunning; });
            if (!g_isRunning) {
                break;
            }
        }

        auto**   visibleLinks    = new BratislavaLink*[COMMS_MAX_LINKS];
        uint32_t numVisibleLinks = 0;

        const instrument_api_status_t result = g_api->instrumentAction(
            INSTRUMENT_COMMS_DISCOVER_BRATISLAVA_LINKS, nullptr, 0, visibleLinks, &numVisibleLinks);
        if (INSTRUMENT_API_SUCCESS != result) {
            std::cout << "Failed to get list of visible links" << std::endl;
        }

        std::cout << "Number of visible links: " + std::to_string(numVisibleLinks) << std::endl;
        if (0 < numVisibleLinks) {
            std::cout << "Visible Bratislava Links: " << std::endl;
            for (int i = 0; i < numVisibleLinks; ++i) {
                BratislavaLinkInfo linkInfo = bratislavaGetLinkInfo(visibleLinks[i]);
                std::cout << "  Link ID: " << linkInfo.linkID << std::endl;
                std::cout << "    Device ID: " << linkInfo.devID << std::endl;
                std::cout << "    Link Type: " << linkTypeToString(linkInfo.linkType) << std::endl;
                std::cout << std::endl;

                std::string message = "Hello Link: " + std::string(linkInfo.devID) + "!";
                g_commsThreads.emplace_back(doLinkSend, 3, visibleLinks[i], message);
                g_commsThreads.emplace_back(doLinkRecv, 3, visibleLinks[i]);
            }
        }
        delete[] visibleLinks;

        // Wait for all the threads which came up in this iteration to exit.
        std::vector<std::thread> blockGroup;
        {
            std::unique_lock<std::mutex> lock(g_commsThreadsMut);

            // Move the old collection, and create a new one.
            blockGroup     = std::move(g_commsThreads);
            g_commsThreads = std::vector<std::thread>();
        }

        for (auto& thread : blockGroup) {
            thread.join();
        }
    }

    g_api->instrumentAction(INSTRUMENT_WIFI_SCAN_OFF, nullptr, 0, nullptr, 0);
    destroyInstrumentAPI(g_api);

    return 0;
}
