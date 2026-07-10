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
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

/**
 * @file socketExample.cc
 * @brief A simple example demonstrating usage of the Bratislava socket for
 * device communication.
 * @details
 * Prerequisites:
 * 1. An Anduril Sim environment with entities who are assigned Bluetooth and/or
 * Wi-Fi devices
 *
 * Expectations:
 *
 * doSocketSend:
 * 1. Send the given message's length with the given Bratislava socket.
 * 2. Send the given message with the given Bratislava socket.
 * 3. If a device is no longer detected, disconnect the socket.
 *
 * doSocketRecv:
 * 1. Receive the length of an incoming message from a Bratislava node.
 * 2. Receive an incoming message from a Bratislava node.
 * 3. If a device is no longer detected, disconnect the socket.
 *
 * GenerateMessage:
 * 1. Create a message that advertises what type of device you are to a
 * compatible link.
 *
 * DiscoveredLinkResponse:
 * 1. Print discovered link profile.
 * 2. Create a Bratislava socket for the given link.
 * 3. Establish a connection on the Bratislava socket with the given link's
 * device.
 * 4. Spin off a thread for sending messages to the discovered node.
 * 5. Spin off a thread for receiving messages from the discovered node.
 *
 * main:
 * 1. Create the Instrument API.
 * 2. Register a callback to DiscoveredLinkResponse when new Bluetooth
 * Bratislava links are discovered.
 * 3. Register a callback to DiscoveredLinkResponse when new Wi-Fi Bratislava
 * links are discovered.
 * 4. Clean up any loose threads when they stop running.
 *
 */

InstrumentAPI*              g_api;
std::atomic<bool>           g_isRunning = true;
std::vector<std::thread>    g_commsThreads;
std::mutex                  g_commsThreadsMut;
std::mutex                  g_executionMut;
std::atomic<int>            g_btCallbackCount     = 0;
std::atomic<int>            g_btConnectionCount   = 0;
std::atomic<int>            g_wifiCallbackCount   = 0;
std::atomic<int>            g_wifiConnectionCount = 0;
std::condition_variable_any g_executionCondition;

void signal_handler(const int signum) {
    std::cout << "Interrupt signal (" << signum << ") received." << std::endl;
    std::cout << "Cleaning up and exiting...\n" << std::endl;

    g_isRunning = false;
    g_executionCondition.notify_all();
}

static void doSocketSend(const int delay, BratislavaSocket* bsock, const std::string& message) {
    while (g_isRunning) {
        // Send message length, followed by message
        uint32_t  messageLength = message.size() + 1; // +1 for null-terminator
        const int status        = bratislavaSend(bsock, &messageLength, sizeof(messageLength));

        if (status == -1) {
            if (errno == EPIPE) {
                std::cout << "Device is no longer detected. Disconnecting." << std::endl;
            } else {
                std::cout << "Failed to send message length." << std::endl;
            }
            break;
        }

        if (bratislavaSend(bsock, message.c_str(), message.size() + 1) == -1) {
            std::cout << "Failed to send message." << std::endl;
            break;
        }

        std::this_thread::sleep_for(std::chrono::seconds(delay));
    }
    bratislavaDestroy(bsock);
}

static void doSocketRecv(const int delay, BratislavaSocket* bsock) {
    // Set the timeout for bratislavaRecv in milliseconds. This is optional and
    // overrides the default timeout provided in BratislavaSocket.h
    // bratislavaRecv will return -1 with errno ETIMEDOUT on timeout.
    const int status = bratislavaRecvTimeout(bsock, 3000);
    if (status == -1) {
        std::cout << "Failed to set recv timeout." << std::endl;
        return;
    }

    while (g_isRunning) {
        // Receive message length, followed by message
        uint32_t      messageLength              = 0;
        const ssize_t bytes_received_message_len = bratislavaRecv(bsock, &messageLength, sizeof(messageLength));
        if (bytes_received_message_len <= 0) {
            switch (errno) {
            case ETIMEDOUT:
                std::cout << "Timeout occurred while receiving messages." << std::endl;
            case EPIPE:
                std::cout << "Device is no longer detected. Disconnecting." << std::endl;
            default:
                std::cout << "Failed to receive message length." << std::endl;
            }
        }

        const auto    messageBuffer          = new char[messageLength];
        const ssize_t bytes_received_message = bratislavaRecv(bsock, messageBuffer, messageLength);
        if (bytes_received_message <= 0) {
            std::cout << "Failed to receive message." << std::endl;
            break;
        }

        std::string message(messageBuffer);
        delete[] messageBuffer;

        std::cout << "Received Message: " << message << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(delay));
    }
    bratislavaDestroy(bsock);
}

std::string GenerateMessage(const BratislavaLink* link) {
    std::string message;
    switch (link->instrumentType) {
    case INSTRUMENT_BLUETOOTH: {
        message = "I'm a Bluetooth device with MAC address " + std::string(link->bluetoothDeviceInfo.mac_address) +
                  "! Nice to meet you!";
        break;
    }
    case INSTRUMENT_WIFI: {
        switch (link->wifiDeviceInfo.type) {
        case WIFI_ADAPTER:
            message = "I'm a WiFi router! You're a device with MAC address " +
                      std::string(link->wifiDeviceInfo.mac_address) + "! What's it to you?";
            break;
        case WIFI_ROUTER:
            message = "I'm a WiFi adapter! You're a device with MAC address " +
                      std::string(link->wifiDeviceInfo.mac_address) + "! How's it going?";
            break;
        default:
            message = "I am a WiFi device! My type is unknown returning...";
            return message;
        }
        break;
    }
    default: {
        message = "What kind of device are you?";
        break;
    }
    }
    return message;
}

void DiscoveredLinkResponse(const BratislavaLink* link) {
    std::cout << "Discovered a new Bratislava link. Automatically ingesting: " << std::endl;
    std::cout << "*** Bratislava Link Info ***" << std::endl;
    std::cout << "    Latency:    " << link->latency << std::endl;
    std::cout << "    Throughput: " << link->throughput << std::endl;
    std::cout << "    Message Loss Probability: " << link->messageLoss << std::endl;

    switch (link->linkType) {
    case LINK_TYPE_BLUE: {
        std::cout << "    Link Type: Blue" << std::endl;
        break;
    }
    case LINK_TYPE_GREEN: {
        std::cout << "    Link Type: Green" << std::endl;
        break;
    }
    case LINK_TYPE_ORANGE: {
        std::cout << "    Link Type: Orange" << std::endl;
        break;
    }
    }

    switch (link->instrumentType) {
    case INSTRUMENT_BLUETOOTH: {
        std::cout << "    Instrument Type: Bluetooth" << std::endl;
        std::cout << "****************************" << std::endl;

        std::cout << "***** Bluetooth Device *****" << std::endl;
        std::cout << "Bluetooth Device ID: " << link->bluetoothDeviceInfo.id << std::endl;
        std::cout << "Bluetooth Device Name: " << link->bluetoothDeviceInfo.name << std::endl;
        std::cout << "Bluetooth Device Address: " << link->bluetoothDeviceInfo.mac_address << std::endl;
        std::cout << "Bluetooth Device Class: " << link->bluetoothDeviceInfo.dev_class[0] << " "
                  << link->bluetoothDeviceInfo.dev_class[1] << " " << link->bluetoothDeviceInfo.dev_class[2]
                  << std::endl;
        // Mirror BluetoothDeviceResponse: print RSSI when the link's device
        // payload reports it as valid; otherwise note that it's missing.
        if (link->bluetoothDeviceInfo.rssi.valid) {
            std::cout << "Bluetooth Device RSSI: " << link->bluetoothDeviceInfo.rssi.value_dbm << " dBm" << std::endl;
        } else {
            std::cout << "Bluetooth Device RSSI: (not available)" << std::endl;
        }
        std::cout << "****************************" << std::endl;
        ++g_btCallbackCount;
        break;
    }
    case INSTRUMENT_WIFI: {
        std::cout << "    Instrument Type: WiFi" << std::endl;
        std::cout << "****************************" << std::endl;

        std::cout << "***** WiFi Device *****" << std::endl;
        std::cout << "WiFi device SSID: " << link->wifiDeviceInfo.ssid << std::endl;
        std::cout << "WiFi device address: " << link->wifiDeviceInfo.mac_address << std::endl;
        switch (link->wifiDeviceInfo.type) {
        case WIFI_ADAPTER:
            std::cout << "Device Type: Adapter" << std::endl;
            break;
        case WIFI_ROUTER:
            std::cout << "Device Type: Router" << std::endl;
            break;
        default:
            std::cout << "Device Type: Unknown" << std::endl;
            return;
        }
        // Mirror WifiDeviceResponse: print RSSI when the link's device
        // payload reports it as valid; otherwise note that it's missing.
        if (link->wifiDeviceInfo.rssi.valid) {
            std::cout << "WiFi Device RSSI: " << link->wifiDeviceInfo.rssi.value_dbm << " dBm" << std::endl;
        } else {
            std::cout << "WiFi Device RSSI: (not available)" << std::endl;
        }
        std::cout << "****************************" << std::endl;
        ++g_wifiCallbackCount;
        break;
    }
    default: {
        std::cout << "Instrument Type: Unsupported" << std::endl;
        std::cout << "****************************" << std::endl;
        return;
    }
    }

    std::cout << "Creating Bratislava socket" << std::endl;
    BratislavaSocket* bsock = bratislavaSocket(*link);

    std::cout << "Establishing Bratislava socket connection with discovered node" << std::endl;
    // Set the connection timeout in milliseconds.
    //  This is optional. The default value is 10 seconds.
    int status = bratislavaConnTimeout(bsock, 10000);
    if (status == -1) {
        std::cout << "Failed to set connection timeout." << std::endl;
        bratislavaDestroy(bsock);
        return;
    }
    status = bratislavaConn(bsock);
    if (status == -1) {
        std::cout << "Failed to connect to discovered Bratislava node." << std::endl;
        bratislavaDestroy(bsock);
        return;
    }

    switch (link->instrumentType) {
    case INSTRUMENT_BLUETOOTH: {
        ++g_btConnectionCount;
        break;
    }
    case INSTRUMENT_WIFI: {
        ++g_wifiConnectionCount;
        break;
    }
    default: {
        std::cout << "Instrument Type: Unsupported" << std::endl;
        return;
    }
    }

    std::string message = GenerateMessage(link);

    std::cout << "Beginning to send and receive messages with the discovered node" << std::endl;
    {
        std::unique_lock<std::mutex> lock(g_commsThreadsMut);
        if (!g_isRunning) {
            return;
        }
        g_commsThreads.emplace_back(doSocketSend, 3, bsock, message);
        g_commsThreads.emplace_back(doSocketRecv, 3, bsock);
    }
}

void BluetoothDeviceResponse(const BluetoothDeviceInfoBase* btDevice) {
    std::cout << "Discovered a new device. Automatically ingesting: " << std::endl;
    std::cout << "    Device Name: " << btDevice->name << std::endl;
    std::cout << "    Device MAC Address: " << btDevice->mac_address << std::endl;
    if (btDevice->rssi.valid) {
        std::cout << "    Device RSSI: " << btDevice->rssi.value_dbm << " dBm" << std::endl;
    } else {
        std::cout << "    Device RSSI: (not available)" << std::endl;
    }
    std::cout << "****************************" << std::endl;
}

void WifiDeviceResponse(const WifiDeviceInfoBase* wifiDevice) {
    std::cout << "Discovered a new device. Automatically ingesting: " << std::endl;
    std::cout << "    Device SSID: " << wifiDevice->ssid << std::endl;
    std::cout << "    Device MAC Address: " << wifiDevice->mac_address << std::endl;
    switch (wifiDevice->type) {
    case WIFI_ADAPTER:
        std::cout << "    Device Type: Adapter" << std::endl;
        break;
    case WIFI_ROUTER:
        std::cout << "    Device Type: Router" << std::endl;
        break;
    default:
        std::cout << "    Device Type: Unknown" << std::endl;
        break;
    }
    if (wifiDevice->rssi.valid) {
        std::cout << "    Device RSSI: " << wifiDevice->rssi.value_dbm << " dBm" << std::endl;
    } else {
        std::cout << "    Device RSSI: (not available)" << std::endl;
    }
    std::cout << "****************************" << std::endl;
}

void DroppedLinkResponse(const BratislavaLinkInfo* linkInfo) {
    std::cout << "Dropped Link ID: " << linkInfo->linkID << std::endl;
}

int main() {
    // Create an instrument API
    constexpr ServerConfig        serverConfig = {"192.168.1.1", 50052};
    constexpr LoggerConfig        loggerConfig = {INFO};
    constexpr InstrumentAPIConfig config{serverConfig, loggerConfig};

    std::cout << "API Platform: " << getInstrumentAPIPlatform() << " API Version: " << getInstrumentAPIVersion()
              << std::endl;

    g_api = createInstrumentAPI(config);

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    bool                    has_bluetooth = false;
    bool                    has_wifi      = false;
    instrument_api_status_t result        = INSTRUMENT_API_ERROR;

    // Register a callback that runs when new Bluetooth devices are discovered
    BluetoothDeviceDiscoveredCallback btDeviceCallback = BluetoothDeviceResponse;
    result                                             = g_api->registerCallback(INSTRUMENT_BLUETOOTH,
                                     DEVICE_DISCOVERED,
                                     static_cast<InstrumentInputType>(&btDeviceCallback));
    if (result == INSTRUMENT_API_NOT_SUPPORTED) {
        std::cout << "This entity does not support Bluetooth devices. Failed "
                     "to register Bluetooth callback."
                  << std::endl;
    } else if (result != INSTRUMENT_API_SUCCESS) {
        std::cout << "Error: " << result << std::endl;
        return result;
    } else {
        has_bluetooth = true;
    }

    // Register a callback that runs when new Wi-Fi devices are discovered
    WifiDeviceDiscoveredCallback wifiDeviceCallback = WifiDeviceResponse;
    result                                          = g_api->registerCallback(INSTRUMENT_WIFI,
                                     DEVICE_DISCOVERED,
                                     static_cast<InstrumentInputType>(&wifiDeviceCallback));
    if (result == INSTRUMENT_API_NOT_SUPPORTED) {
        std::cout << "This entity does not support WiFi devices. Failed to "
                     "register WiFi callback."
                  << std::endl;
    } else if (result != INSTRUMENT_API_SUCCESS) {
        std::cout << "Error: " << result << std::endl;
        return result;
    } else {
        has_wifi = true;
    }

    // If no supported interfaces exist, return early
    if (!has_bluetooth && !has_wifi) {
        std::cout << "This device does not support Bluetooth or Wi-Fi. Failed "
                     "to register any callbacks. Exiting."
                  << std::endl;
        destroyInstrumentAPI(g_api);
        return 1;
    }

    // Register a callback that runs when new links are discovered
    LinkDiscoveredCallback LinkCallback = DiscoveredLinkResponse;
    result =
        g_api->registerCallback(INSTRUMENT_COMMS, LINK_DISCOVERED, static_cast<InstrumentInputType>(&LinkCallback));
    if (result != INSTRUMENT_API_SUCCESS) {
        std::cout << "Error: " << result << std::endl;
        return result;
    }

    // Register a callback that runs when links are dropped
    LinkDroppedCallback DropCallback = DroppedLinkResponse;
    result = g_api->registerCallback(INSTRUMENT_COMMS, LINK_DROPPED, static_cast<InstrumentInputType>(&DropCallback));
    if (result != INSTRUMENT_API_SUCCESS) {
        std::cout << "Error: " << result << std::endl;
        return result;
    }

    // Periodically print counts
    while (g_isRunning) {
        {
            std::lock_guard<std::mutex> lock(g_executionMut);
            g_executionCondition.wait_for(g_executionMut, std::chrono::seconds(60), [&] { return !g_isRunning; });
            if (!g_isRunning) {
                break;
            }
        }

        std::cout << "Bluetooth Callbacks | Connections: " << g_btCallbackCount << " | " << g_btConnectionCount
                  << std::endl;
        std::cout << "Wifi      Callbacks | Connections: " << g_wifiCallbackCount << " | " << g_wifiConnectionCount
                  << std::endl;

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

    g_api->unregisterCallback(INSTRUMENT_COMMS, LINK_DISCOVERED, static_cast<InstrumentInputType>(&LinkCallback));
    g_api->unregisterCallback(INSTRUMENT_COMMS, LINK_DROPPED, static_cast<InstrumentInputType>(&DropCallback));
    g_api->unregisterCallback(INSTRUMENT_WIFI,
                              DEVICE_DISCOVERED,
                              static_cast<InstrumentInputType>(&wifiDeviceCallback));
    g_api->unregisterCallback(INSTRUMENT_BLUETOOTH,
                              DEVICE_DISCOVERED,
                              static_cast<InstrumentInputType>(&btDeviceCallback));

    {
        std::unique_lock<std::mutex> lock(g_commsThreadsMut);
        for (auto& thread : g_commsThreads) {
            thread.join();
        }
    }

    destroyInstrumentAPI(g_api);

    return 0;
}
