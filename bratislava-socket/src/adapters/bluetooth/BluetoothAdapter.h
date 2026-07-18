#ifndef BLUETOOTH_ADAPTER_H
#define BLUETOOTH_ADAPTER_H

#include "bluetooth_typedef.h"

#include <adapters/BaseAdapter.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/l2cap.h>

#include <atomic>
#include <chrono>
#include <cstddef>

struct BratislavaSocket;

class BluetoothAdapter : public BaseAdapter {
public:
    explicit BluetoothAdapter(BratislavaSocket* bsock);
    ~BluetoothAdapter() override;

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
    bool loadLocalAddress();
    void setupRemoteAddress();
    bool waitForSocket(int socket_fd, short events, std::chrono::milliseconds timeout) const;
    uint16_t portForMac(const char* mac_address) const;

    BratislavaSocket*         bsock_ = nullptr;
    BluetoothDeviceInfoBase*  btDeviceInfo_ = nullptr;
    int                       sockfd_ = -1;
    int                       sockfd_listen_ = -1;
    bool                      is_listener_ = false;
    std::atomic<bool>         connected_{false};
    sockaddr_l2               localAddr_{};
    sockaddr_l2               remoteAddr_{};
    std::chrono::milliseconds conn_timeout_{10000};
    std::chrono::milliseconds recv_timeout_{5000};
};

#endif
