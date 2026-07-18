#include "BluetoothAdapter.h"

#include <BratislavaSocketImpl.h>
#include <BratislavaSocketRegistry.h>

#include <cerrno>
#include <cstring>
#include <functional>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

BluetoothAdapter::BluetoothAdapter(BratislavaSocket* bsock) : bsock_(bsock) {
    btDeviceInfo_ = &bsock_->link.bluetoothDeviceInfo;
    if (!loadLocalAddress()) {
        return;
    }
    setupRemoteAddress();
    char local_mac[18] = {0};
    ba2str(&localAddr_.l2_bdaddr, local_mac);
    is_listener_ = (std::strcmp(local_mac, btDeviceInfo_->mac_address) < 0);
    createSocket();
    createListeningSocket();
}

BluetoothAdapter::~BluetoothAdapter() {
    closeListeningSocket();
    closeSocket();
}

int BluetoothAdapter::conn() {
    connected_ = false;
    closeSocket();
    closeListeningSocket();
    createSocket();
    createListeningSocket();

    if ((sockfd_ < 0) || (sockfd_listen_ < 0)) {
        return -1;
    }

    if (is_listener_) {
        configureSocketTimeout(sockfd_listen_, conn_timeout_);
        if (bind(sockfd_listen_, reinterpret_cast<sockaddr*>(&localAddr_), sizeof(localAddr_)) != 0) {
            return -1;
        }
        if (listen(sockfd_listen_, 1) != 0) {
            return -1;
        }
        sockaddr_l2 remote_addr{};
        socklen_t remote_len = sizeof(remote_addr);
        const int accepted = accept(sockfd_listen_, reinterpret_cast<sockaddr*>(&remote_addr), &remote_len);
        if (accepted < 0) {
            return -1;
        }
        closeSocket();
        sockfd_ = accepted;
        connected_ = true;
    } else {
        configureSocketTimeout(sockfd_, conn_timeout_);
        if (!waitForSocket(sockfd_, POLLOUT, conn_timeout_)) {
            return -1;
        }
        if (connect(sockfd_, reinterpret_cast<sockaddr*>(&remoteAddr_), sizeof(remoteAddr_)) != 0) {
            return -1;
        }
        connected_ = true;
    }

    BratislavaSocketRegistry::GetInstance().Connect(bsock_);
    configureSocketForComms();
    return 0;
}

int BluetoothAdapter::disconn() {
    connected_ = false;
    if (sockfd_ >= 0) {
        shutdown(sockfd_, SHUT_RDWR);
    }
    return 0;
}

int BluetoothAdapter::send(const void* buf, size_t len) {
    if (!BratislavaSocketRegistry::GetInstance().IsConnected(bsock_) || (sockfd_ < 0)) {
        errno = EPIPE;
        return -1;
    }
    return static_cast<int>(::send(sockfd_, buf, len, MSG_NOSIGNAL));
}

int BluetoothAdapter::recv(void* buf, size_t len) {
    if (!BratislavaSocketRegistry::GetInstance().IsConnected(bsock_) || (sockfd_ < 0)) {
        errno = EPIPE;
        return -1;
    }
    return static_cast<int>(::recv(sockfd_, buf, len, MSG_NOSIGNAL));
}

int BluetoothAdapter::setConnTimeout(unsigned int milliseconds) {
    conn_timeout_ = std::chrono::milliseconds(milliseconds);
    return 0;
}

int BluetoothAdapter::setRecvTimeout(unsigned int milliseconds) {
    recv_timeout_ = std::chrono::milliseconds(milliseconds);
    if (sockfd_ >= 0) {
        configureSocketForComms();
    }
    return 0;
}

void BluetoothAdapter::interrupt() {
    if (sockfd_ >= 0) {
        shutdown(sockfd_, SHUT_RDWR);
    }
    if (sockfd_listen_ >= 0) {
        shutdown(sockfd_listen_, SHUT_RDWR);
    }
}

void BluetoothAdapter::createSocket() {
    sockfd_ = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
}

void BluetoothAdapter::createListeningSocket() {
    sockfd_listen_ = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
}

void BluetoothAdapter::closeSocket() {
    if (sockfd_ >= 0) {
        close(sockfd_);
        sockfd_ = -1;
    }
}

void BluetoothAdapter::closeListeningSocket() {
    if (sockfd_listen_ >= 0) {
        close(sockfd_listen_);
        sockfd_listen_ = -1;
    }
}

void BluetoothAdapter::configureSocketTimeout(const int socket_fd, const std::chrono::milliseconds timeout) const {
    timeval tv{};
    tv.tv_sec = static_cast<long>(timeout.count() / 1000);
    tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

void BluetoothAdapter::configureSocketForComms() const {
    configureSocketTimeout(sockfd_, recv_timeout_);
}

bool BluetoothAdapter::loadLocalAddress() {
    std::memset(&localAddr_, 0, sizeof(localAddr_));
    localAddr_.l2_family = AF_BLUETOOTH;
    localAddr_.l2_psm = htobs(portForMac(btDeviceInfo_->mac_address));

    const int dev_id = hci_get_route(nullptr);
    if (dev_id < 0) {
        return false;
    }
    const int hci_socket = hci_open_dev(dev_id);
    if (hci_socket < 0) {
        return false;
    }

    const int rc = hci_read_bd_addr(hci_socket, &localAddr_.l2_bdaddr, 1000);
    close(hci_socket);
    return rc == 0;
}

void BluetoothAdapter::setupRemoteAddress() {
    std::memset(&remoteAddr_, 0, sizeof(remoteAddr_));
    remoteAddr_.l2_family = AF_BLUETOOTH;
    remoteAddr_.l2_psm = htobs(portForMac(btDeviceInfo_->mac_address));
    (void)str2ba(btDeviceInfo_->mac_address, &remoteAddr_.l2_bdaddr);
}

bool BluetoothAdapter::waitForSocket(const int socket_fd,
                                     const short events,
                                     const std::chrono::milliseconds timeout) const {
    pollfd pfd{};
    pfd.fd = socket_fd;
    pfd.events = events;
    const int rc = poll(&pfd, 1, static_cast<int>(timeout.count()));
    return (rc > 0) && ((pfd.revents & events) != 0);
}

uint16_t BluetoothAdapter::portForMac(const char* mac_address) const {
    constexpr uint16_t kRangeStart = 0x1002;
    constexpr uint16_t kRangeEnd = 0x1100;
    constexpr uint16_t kOddSlotCount = static_cast<uint16_t>(((kRangeEnd - kRangeStart) / 2) + 1);
    const size_t hash_value = std::hash<std::string>{}(mac_address != nullptr ? std::string(mac_address) : std::string());
    return static_cast<uint16_t>(kRangeStart + ((hash_value % kOddSlotCount) * 2) + 1);
}
