#ifndef WIFI_ADAPTER_H
#define WIFI_ADAPTER_H

#include "BratislavaSocketImpl.h"

#include <adapters/BaseAdapter.h>

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <netinet/in.h>

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
    void interrupt() override;

private:
    void createSocket();
    void createListeningSocket();
    void closeSocket();
    void closeListeningSocket();
    void configureSocketTimeout(int socket_fd, std::chrono::milliseconds timeout) const;
    void configureSocketForComms() const;
    bool resolveLocalIp();
    void setupAddresses();
    bool waitForSocket(int socket_fd, short events, std::chrono::milliseconds timeout) const;
    uint16_t portForIdentity(const char* identity) const;

    BratislavaSocket*         bsock_ = nullptr;
    WifiDeviceInfoBase*       wifiDeviceInfo_ = nullptr;
    int                       sockfd_ = -1;
    int                       sockfd_listen_ = -1;
    bool                      is_listener_ = false;
    std::atomic<bool>         connected_{false};
    sockaddr_in               localAddr_{};
    sockaddr_in               remoteAddr_{};
    char                      local_ip_[INET_ADDRSTRLEN] = {0};
    std::chrono::milliseconds conn_timeout_{10000};
    std::chrono::milliseconds recv_timeout_{5000};
};

#endif
