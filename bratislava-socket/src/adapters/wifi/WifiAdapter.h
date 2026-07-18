#ifndef WIFI_ADAPTER_H
#define WIFI_ADAPTER_H

#include "BratislavaSocketImpl.h"

#include <anduril/serialization/LinkSerialization.h>
#include <anduril/util/Logger.h>

#include <NetworkManager.h>
#include <adapters/BaseAdapter.h>
#include <nlohmann/json.hpp>
#include <telemetry/api.hpp>
#include <wifi_typedef.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>

#define DEFAULT_CONN_TIMEOUT_MS 10000

class WifiAdapter : public BaseAdapter {
public:
    explicit WifiAdapter(BratislavaSocket* bsock);
    ~WifiAdapter() override;
    int conn() override;
    int disconn() override;
    int send(const void* buf, size_t len) override;
    int recv(void* buf, size_t len) override;
    int setConnTimeout(unsigned int milliseconds) override;
    int setRecvTimeout(unsigned int milliseconds) override;

private:
    template <typename T>
    class NMSafeWrapper {
    public:
        NMSafeWrapper() : obj(nullptr) {}

        explicit NMSafeWrapper(T* obj) : obj(obj) {}

        NMSafeWrapper(NMSafeWrapper&)                  = delete;
        NMSafeWrapper& operator=(const NMSafeWrapper&) = delete;

        // Explicit move
        NMSafeWrapper(NMSafeWrapper&& moveMe) noexcept {
            this->obj  = moveMe.obj;
            moveMe.obj = nullptr;
        }

        NMSafeWrapper&& operator=(const NMSafeWrapper&&) = delete;

        ~NMSafeWrapper() {
            if (!this->obj) {
                return;
            }
            g_object_unref(this->obj);
            this->obj = nullptr;
        }

        T* Ref() { return this->obj; }

        T* operator->() { return this->obj; }

    private:
        T* obj;
    };

    void        ConfigureSocketForConnection(const int& sockfd, const std::chrono::milliseconds& timeout);
    void        ConfigureSocketForComms();
    void        CreateSocket();
    void        CloseSocket();
    bool        CreateConnectionRequestSocket();
    bool        CreateListeningSocket();
    static bool WaitForSocket(const int& sockfd, std::chrono::milliseconds timeout);
    bool        SendConnectionRequest(const std::chrono::time_point<std::chrono::steady_clock>& start_time);
    void        ConnectToPeer(const std::chrono::time_point<std::chrono::steady_clock>& start_time);
    void        ListenForPeer(const std::chrono::time_point<std::chrono::steady_clock>& start_time);
    void        SetupLocalAddr();
    void        SetupRemoteAddr();

    static NMSafeWrapper<NMClient> GetNMClient();
    static NMDevice*               GetNMDevice(NMClient* client);
    static WifiDeviceType          GetWifiDeviceType();

    [[nodiscard]] std::chrono::milliseconds
    GetRemainingConnectionTime(const std::chrono::time_point<std::chrono::steady_clock>& start_time) const;
    static const char* GetLocalIP();
    int                GetAccessPointIP();
    void               SetGateway(const char* gateway);
    static bool        GetSSID(char* ssid, size_t ssid_size);
    static bool        GetMacAddress(char* mac_addr, size_t mac_addr_size);

    const int BRATISLAVA_PORT = 5566;
    const int BACKLOG         = 10;

    BratislavaSocket*   bsock_;
    WifiDeviceInfoBase* wifiDeviceInfo_;
    WifiDeviceType      deviceType_;
    char                mac_address_[18]{};
    char                ssid_[256]{};
    int                 remote_port_ = -1;
    char*               gateway_;

    int sockfd_         = -1;
    int sockfd_request_ = -1;
    int sockfd_listen_  = -1;

    std::atomic<bool>              connected_ = false;
    sockaddr_in                    localAddr_{};
    sockaddr_in                    remoteAddr_{};
    nlohmann::json                 linkTelemetryJSON_;
    std::chrono::milliseconds      conn_timeout_        = std::chrono::milliseconds(DEFAULT_CONN_TIMEOUT_MS);
    std::chrono::milliseconds      recv_timeout_        = std::chrono::milliseconds(DEFAULT_RECV_TIMEOUT_MS);
    anduril::telemetry::DeviceType telemetryDeviceType_ = anduril::telemetry::DeviceType::WIFI;
};

#endif // WIFI_ADAPTER_H
