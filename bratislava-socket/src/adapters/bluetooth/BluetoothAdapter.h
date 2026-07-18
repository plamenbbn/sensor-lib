#ifndef BLUETOOTH_ADAPTER_H
#define BLUETOOTH_ADAPTER_H

#ifdef DEVICE_BUILD

#include "bluetooth_typedef.h"
#include "sensing_typedef.h"

#include <anduril/serialization/LinkSerialization.h>

#include <BratislavaSocketRegistry.h>
#include <adapters/BaseAdapter.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/l2cap.h>
#include <nlohmann/json.hpp>
#include <openssl/sha.h>
#include <telemetry/api.hpp>

#include <cstring>
#include <functional>
#include <poll.h>
#include <random>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

class BluetoothAdapter : public BaseAdapter {
public:
    BluetoothAdapter(BratislavaSocket* bsock);
    ~BluetoothAdapter();
    int conn() override;
    int disconn() override;
    int send(const void* buf, size_t len) override;
    int recv(void* buf, size_t len) override;
    int setConnTimeout(unsigned int milliseconds) override;
    int setRecvTimeout(unsigned int milliseconds) override;

private:
    void     ConfigureSocketForConnection(int& sockfd, const std::chrono::milliseconds& timeout);
    void     ConfigureSocketForComms();
    void     CreateSocket();
    void     CloseSocket();
    bool     CreateConnectionRequestSocket();
    bool     CreateListeningSocket();
    bool     WaitForSocket(const int& sockfd, std::chrono::milliseconds timeout);
    bool     SendConnectionRequest(std::chrono::time_point<std::chrono::steady_clock>& start_time);
    void     ConnectToPeer(std::chrono::time_point<std::chrono::steady_clock>& start_time);
    void     ListenForPeer(std::chrono::time_point<std::chrono::steady_clock>& start_time);
    void     SetupLocalAddr();
    void     SetupRemoteAddr();
    uint16_t GenerateUniquePort(const char* mac_address);
    std::chrono::milliseconds
    GetRemainingConnectionTime(const std::chrono::time_point<std::chrono::steady_clock>& start_time);

    static constexpr uint16_t BRATISLAVA_PORT = 0x1001;

    BratislavaSocket*              bsock_;
    int                            sockfd_         = -1;
    int                            sockfd_request_ = -1;
    int                            sockfd_listen_  = -1;
    bool                           is_listener_    = false;
    std::atomic<bool>              connected_      = false;
    struct sockaddr_l2             localAddr_;
    struct sockaddr_l2             remoteAddr_;
    BluetoothDeviceInfoBase*       btDeviceInfo_;
    nlohmann::json                 linkTelemetryJSON_;
    anduril::telemetry::DeviceType telemetryDeviceType_ = anduril::telemetry::DeviceType::BLUETOOTH;
    std::chrono::milliseconds      conn_timeout_        = std::chrono::milliseconds(DEFAULT_CONN_TIMEOUT_MS);
    std::chrono::milliseconds      recv_timeout_        = std::chrono::milliseconds(DEFAULT_RECV_TIMEOUT_MS);
};

#endif // DEVICE_BUILD

#endif // BLUETOOTH_ADAPTER_H
