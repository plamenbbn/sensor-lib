#include "WifiAdapter.h"

#include <BratislavaSocketRegistry.h>

#include <cerrno>
#include <cstring>
#include <functional>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

WifiAdapter::WifiAdapter(BratislavaSocket* bsock) : bsock_(bsock) {
    wifiDeviceInfo_ = &bsock_->link.wifiDeviceInfo;
    if (!resolveLocalIp()) {
        return;
    }
    setupAddresses();
    is_listener_ = std::strcmp(local_ip_, wifiDeviceInfo_->ssid) < 0;
    createSocket();
    createListeningSocket();
}

WifiAdapter::~WifiAdapter() {
    closeListeningSocket();
    closeSocket();
}

int WifiAdapter::conn() {
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
        sockaddr_in remote_addr{};
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

int WifiAdapter::disconn() {
    connected_ = false;
    if (sockfd_ >= 0) {
        shutdown(sockfd_, SHUT_RDWR);
    }
    return 0;
}

int WifiAdapter::send(const void* buf, size_t len) {
    if (!BratislavaSocketRegistry::GetInstance().IsConnected(bsock_) || (sockfd_ < 0)) {
        errno = EPIPE;
        return -1;
    }
    return static_cast<int>(::send(sockfd_, buf, len, MSG_NOSIGNAL));
}

int WifiAdapter::recv(void* buf, size_t len) {
    if (!BratislavaSocketRegistry::GetInstance().IsConnected(bsock_) || (sockfd_ < 0)) {
        errno = EPIPE;
        return -1;
    }
    return static_cast<int>(::recv(sockfd_, buf, len, MSG_NOSIGNAL));
}

int WifiAdapter::setConnTimeout(unsigned int milliseconds) {
    conn_timeout_ = std::chrono::milliseconds(milliseconds);
    return 0;
}

int WifiAdapter::setRecvTimeout(unsigned int milliseconds) {
    recv_timeout_ = std::chrono::milliseconds(milliseconds);
    if (sockfd_ >= 0) {
        configureSocketForComms();
    }
    return 0;
}

void WifiAdapter::interrupt() {
    if (sockfd_ >= 0) {
        shutdown(sockfd_, SHUT_RDWR);
    }
    if (sockfd_listen_ >= 0) {
        shutdown(sockfd_listen_, SHUT_RDWR);
    }
}

void WifiAdapter::createSocket() {
    sockfd_ = socket(AF_INET, SOCK_STREAM, 0);
}

void WifiAdapter::createListeningSocket() {
    sockfd_listen_ = socket(AF_INET, SOCK_STREAM, 0);
}

void WifiAdapter::closeSocket() {
    if (sockfd_ >= 0) {
        close(sockfd_);
        sockfd_ = -1;
    }
}

void WifiAdapter::closeListeningSocket() {
    if (sockfd_listen_ >= 0) {
        close(sockfd_listen_);
        sockfd_listen_ = -1;
    }
}

void WifiAdapter::configureSocketTimeout(const int socket_fd, const std::chrono::milliseconds timeout) const {
    timeval tv{};
    tv.tv_sec = static_cast<long>(timeout.count() / 1000);
    tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

void WifiAdapter::configureSocketForComms() const {
    configureSocketTimeout(sockfd_, recv_timeout_);
}

bool WifiAdapter::resolveLocalIp() {
    if ((wifiDeviceInfo_ == nullptr) || (wifiDeviceInfo_->ssid[0] == '\0')) {
        errno = EDESTADDRREQ;
        return false;
    }

    const int probe_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (probe_fd < 0) {
        return false;
    }

    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(9);
    if (inet_pton(AF_INET, wifiDeviceInfo_->ssid, &remote.sin_addr) != 1) {
        close(probe_fd);
        errno = EDESTADDRREQ;
        return false;
    }

    if (connect(probe_fd, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) != 0) {
        const int saved_errno = errno;
        close(probe_fd);
        errno = saved_errno;
        return false;
    }

    sockaddr_in local{};
    socklen_t local_len = sizeof(local);
    if (getsockname(probe_fd, reinterpret_cast<sockaddr*>(&local), &local_len) != 0) {
        const int saved_errno = errno;
        close(probe_fd);
        errno = saved_errno;
        return false;
    }

    close(probe_fd);
    return inet_ntop(AF_INET, &local.sin_addr, local_ip_, sizeof(local_ip_)) != nullptr;
}

void WifiAdapter::setupAddresses() {
    std::memset(&localAddr_, 0, sizeof(localAddr_));
    localAddr_.sin_family = AF_INET;
    localAddr_.sin_port = htons(portForIdentity(local_ip_));
    localAddr_.sin_addr.s_addr = htonl(INADDR_ANY);

    std::memset(&remoteAddr_, 0, sizeof(remoteAddr_));
    remoteAddr_.sin_family = AF_INET;
    remoteAddr_.sin_port = htons(portForIdentity(wifiDeviceInfo_->ssid));
    (void)inet_pton(AF_INET, wifiDeviceInfo_->ssid, &remoteAddr_.sin_addr);
}

bool WifiAdapter::waitForSocket(const int socket_fd,
                                const short events,
                                const std::chrono::milliseconds timeout) const {
    pollfd pfd{};
    pfd.fd = socket_fd;
    pfd.events = events;
    const int rc = poll(&pfd, 1, static_cast<int>(timeout.count()));
    return (rc > 0) && ((pfd.revents & events) != 0);
}

uint16_t WifiAdapter::portForIdentity(const char* identity) const {
    constexpr uint16_t kRangeStart = 40000;
    constexpr uint16_t kRangeSize = 1024;
    const size_t hash_value = std::hash<std::string>{}(identity != nullptr ? std::string(identity) : std::string());
    return static_cast<uint16_t>(kRangeStart + (hash_value % kRangeSize));
}
