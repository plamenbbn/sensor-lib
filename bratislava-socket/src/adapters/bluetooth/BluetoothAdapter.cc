#ifdef DEVICE_BUILD

#include "BluetoothAdapter.h"

#include <anduril/util/ErrnoToString.h>
#include <anduril/util/Logger.h>

#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <spdlog/fmt/fmt.h>

#include <atomic>
#include <chrono>
#include <errno.h>
#include <fcntl.h>
#include <sys/time.h>
#include <thread>

BluetoothAdapter::BluetoothAdapter(BratislavaSocket* bsock) : bsock_(bsock) {
    btDeviceInfo_      = &bsock->link.bluetoothDeviceInfo;
    linkTelemetryJSON_ = bsock->link;
    CreateSocket();
    CreateConnectionRequestSocket();
    CreateListeningSocket();
    LOG_DEBUG("Bluetooth socket created. Socket FD: {}", sockfd_);
    SetupLocalAddr();
    SetupRemoteAddr();
    char localAddr_str[18];
    ba2str(&localAddr_.l2_bdaddr, localAddr_str);
    is_listener_ = (strcmp(btDeviceInfo_->mac_address, localAddr_str) > 0);
    LOG_DEBUG("Set Bluetooth remote address. MAC: {}, PSM: {}", btDeviceInfo_->mac_address, remoteAddr_.l2_psm);
    anduril::telemetry::success(anduril::telemetry::EventOwner::SENSOR_INTERFACE_API,
                                anduril::telemetry::EventType::BRATISLAVA_SOCKET_CREATED,
                                linkTelemetryJSON_,
                                telemetryDeviceType_);
}

BluetoothAdapter::~BluetoothAdapter() {
    close(sockfd_request_);
    close(sockfd_listen_);
    CloseSocket();
    LOG_DEBUG("Bluetooth socket closed.");
    anduril::telemetry::success(anduril::telemetry::EventOwner::SENSOR_INTERFACE_API,
                                anduril::telemetry::EventType::BRATISLAVA_SOCKET_DESTROYED,
                                linkTelemetryJSON_,
                                telemetryDeviceType_);
}

int BluetoothAdapter::disconn() {
    connected_ = false;
    CloseSocket();
    CreateSocket();
    if (sockfd_ != -1) {
        return 0;
    }
    return -1;
}

int BluetoothAdapter::conn() {
    connected_ = false;
    LOG_DEBUG("Starting connection process. Device MAC: {}, Timeout: {} ms, Is Listener: {}",
              btDeviceInfo_->mac_address,
              conn_timeout_.count(),
              is_listener_);

    auto start_time = std::chrono::steady_clock::now();

    std::thread request_thread(&BluetoothAdapter::SendConnectionRequest, this, std::ref(start_time));

    if (is_listener_) {
        ListenForPeer(start_time);
        LOG_DEBUG("Started ListenForPeer (Server)");
    } else {
        ConnectToPeer(start_time);
        LOG_DEBUG("Started ConnectToPeer (Client)");
    }

    // Stop the request thread if it's still running
    if (request_thread.joinable()) {
        request_thread.join();
    }

    if (!connected_) {
        LOG_ERROR("Failed to establish connection. Device MAC: {}", btDeviceInfo_->mac_address);
        anduril::telemetry::failure(anduril::telemetry::EventOwner::SENSOR_INTERFACE_API,
                                    anduril::telemetry::EventType::BRATISLAVA_SOCKET_CONNECTED,
                                    linkTelemetryJSON_,
                                    telemetryDeviceType_,
                                    "Failed to establish connection");
        return -1;
    }

    BratislavaSocketRegistry::GetInstance().Connect(bsock_);
    ConfigureSocketForComms();
    LOG_INFO("Connection established. Device MAC: {}", btDeviceInfo_->mac_address);
    anduril::telemetry::success(anduril::telemetry::EventOwner::SENSOR_INTERFACE_API,
                                anduril::telemetry::EventType::BRATISLAVA_SOCKET_CONNECTED,
                                linkTelemetryJSON_,
                                telemetryDeviceType_);
    return 0;
}

int BluetoothAdapter::send(const void* buf, size_t len) {
    if (!BratislavaSocketRegistry::GetInstance().IsConnected(bsock_)) {
        LOG_DEBUG("Attempted to send message on a disconnected Bluetooth socket.");
        anduril::telemetry::failure(anduril::telemetry::EventOwner::SENSOR_INTERFACE_API,
                                    anduril::telemetry::EventType::BRATISLAVA_SOCKET_SEND_MESSAGE,
                                    nlohmann::json{{"length", len}, {"link", linkTelemetryJSON_}},
                                    telemetryDeviceType_,
                                    "EPIPE: Bratislava socket is no longer connected.");
        errno = EPIPE;
        return -1;
    }

    LOG_DEBUG("Sending Bluetooth socket message.");
    auto status = ::send(sockfd_, buf, len, MSG_NOSIGNAL);

    if (status < 0) {
        auto errormsg = std::string("Bluetooth send error: ") + anduril::util::errnoToString();
        LOG_ERROR(errormsg);
        anduril::telemetry::failure(anduril::telemetry::EventOwner::SENSOR_INTERFACE_API,
                                    anduril::telemetry::EventType::BRATISLAVA_SOCKET_SEND_MESSAGE,
                                    nlohmann::json{{"length", len}, {"link", linkTelemetryJSON_}},
                                    telemetryDeviceType_,
                                    errormsg);
        return status;
    }
    anduril::telemetry::success(anduril::telemetry::EventOwner::SENSOR_INTERFACE_API,
                                anduril::telemetry::EventType::BRATISLAVA_SOCKET_SEND_MESSAGE,
                                nlohmann::json{{"length", len}, {"link", linkTelemetryJSON_}},
                                telemetryDeviceType_);
    LOG_DEBUG("Bluetooth socket message sent.");
    return status;
}

int BluetoothAdapter::recv(void* buf, size_t len) {
    if (!BratislavaSocketRegistry::GetInstance().IsConnected(bsock_)) {
        LOG_DEBUG("Attempted to receive message on a disconnected Bluetooth socket.");
        anduril::telemetry::failure(anduril::telemetry::EventOwner::SENSOR_INTERFACE_API,
                                    anduril::telemetry::EventType::BRATISLAVA_SOCKET_RECEIVE_MESSAGE,
                                    nlohmann::json{{"length", len}, {"link", linkTelemetryJSON_}},
                                    telemetryDeviceType_,
                                    "EPIPE: Bratislava socket is no longer connected.");
        errno = EPIPE;
        return -1;
    }

    LOG_DEBUG("Receiving Bluetooth socket message.");
    int bytes_read = ::recv(sockfd_, buf, len, MSG_NOSIGNAL);

    if (bytes_read < 0) {
        int failure_errno = errno;
        if (errno == ETIMEDOUT || errno == EAGAIN || errno == EWOULDBLOCK) {
            failure_errno = ETIMEDOUT;
            LOG_INFO("Bratislava socket timed out while receiving messages.");
            anduril::telemetry::success(anduril::telemetry::EventOwner::SENSOR_INTERFACE_API,
                                        anduril::telemetry::EventType::BRATISLAVA_SOCKET_RECEIVE_MESSAGE,
                                        nlohmann::json{{"length", len}, {"link", linkTelemetryJSON_}},
                                        telemetryDeviceType_);
        } else {
            std::string errormsg = std::string("Bluetooth recv error: ") + anduril::util::errnoToString();
            LOG_ERROR(errormsg);
            anduril::telemetry::failure(anduril::telemetry::EventOwner::SENSOR_INTERFACE_API,
                                        anduril::telemetry::EventType::BRATISLAVA_SOCKET_RECEIVE_MESSAGE,
                                        nlohmann::json{{"length", len}, {"link", linkTelemetryJSON_}},
                                        telemetryDeviceType_,
                                        errormsg);
        }
        errno = failure_errno;
        return bytes_read;
    }

    if (bytes_read == 0) {
        std::string debugmsg = "Zero read on Bluetooth socket, connection closed by peer";
        LOG_DEBUG(debugmsg);
        anduril::telemetry::failure(anduril::telemetry::EventOwner::SENSOR_INTERFACE_API,
                                    anduril::telemetry::EventType::BRATISLAVA_SOCKET_RECEIVE_MESSAGE,
                                    nlohmann::json{{"length", len}, {"link", linkTelemetryJSON_}},
                                    telemetryDeviceType_,
                                    debugmsg);
        return bytes_read;
    }

    anduril::telemetry::success(anduril::telemetry::EventOwner::SENSOR_INTERFACE_API,
                                anduril::telemetry::EventType::BRATISLAVA_SOCKET_RECEIVE_MESSAGE,
                                nlohmann::json{{"length", bytes_read}, {"link", linkTelemetryJSON_}},
                                telemetryDeviceType_);

    LOG_DEBUG("Bluetooth socket message received.");
    return bytes_read;
}

bool BluetoothAdapter::SendConnectionRequest(std::chrono::time_point<std::chrono::steady_clock>& start_time) {
    const int MAX_RETRIES = 5;
    int       retry_count = 0;

    // Configure remote address for connection requests
    struct sockaddr_l2 remoteRequestAddr;
    memset(&remoteRequestAddr, 0, sizeof(remoteRequestAddr));
    remoteRequestAddr.l2_family = AF_BLUETOOTH;
    remoteRequestAddr.l2_psm    = htobs(BRATISLAVA_PORT);
    str2ba(btDeviceInfo_->mac_address, &remoteRequestAddr.l2_bdaddr);

    while (!connected_ && retry_count < MAX_RETRIES) {
        // Get remaining connection time and configure socket to timeout accordingly
        std::chrono::milliseconds remaining_time = GetRemainingConnectionTime(start_time);
        if (remaining_time <= std::chrono::milliseconds(0)) {
            LOG_INFO("Connection request attempt timed out.");
            return false;
        }
        ConfigureSocketForConnection(sockfd_request_, remaining_time);

        // Wait for socket to become available
        if (!WaitForSocket(sockfd_request_, remaining_time)) {
            retry_count++;
            continue;
        }

        // Get remaining connection time and configure socket to timeout accordingly
        remaining_time = GetRemainingConnectionTime(start_time);
        if (remaining_time <= std::chrono::milliseconds(0)) {
            LOG_INFO("Connection request attempt timed out.");
            close(sockfd_request_);
            return false;
        }
        ConfigureSocketForConnection(sockfd_request_, remaining_time);

        // Log connection attempt
        char addr_str[18];
        ba2str(&remoteRequestAddr.l2_bdaddr, addr_str);
        LOG_DEBUG("SendConnectionRequest: Attempting to connect to remote address: {}, PSM: {}",
                  addr_str,
                  remoteRequestAddr.l2_psm);

        // Attempt to connect
        if (connect(sockfd_request_, (struct sockaddr*)&remoteRequestAddr, sizeof(remoteRequestAddr)) == 0) {
            LOG_DEBUG("Connection request with remote address {} is successful", addr_str);
            return true;
        }

        // Handle connection status
        int err = errno;
        LOG_DEBUG("Connection attempt failed. Error: {} ({})", anduril::util::errnoToString(err), err);

        switch (err) {
        case EISCONN:
            LOG_DEBUG("Connection request has already succeeded.");
            return true;
        case ECONNREFUSED:
        case EHOSTUNREACH:
        case EHOSTDOWN:
            LOG_DEBUG("Connection failed due to unavailable server, retrying...");
            break;
        case EINVAL:
            LOG_DEBUG("Invalid argument. Retrying...");
            break;
        case ETIMEDOUT:
        case EAGAIN:
            LOG_INFO("Connection request timed out for remote address: {}", addr_str);
            return false;
        default:
            LOG_ERROR("Unexpected error in connect. Errno: {}", err);
            return false;
        }

        retry_count++;

        // Determine retry backoff time (scaled to be between 0 and remaining_time / 2)
        remaining_time = GetRemainingConnectionTime(start_time);
        if (remaining_time <= std::chrono::milliseconds(0)) {
            LOG_INFO("Connection request attempt timed out during retry.");
            return false;
        }

        auto backoff_time = std::min(remaining_time / 2, std::chrono::milliseconds(100 * (1 << retry_count)));

        LOG_DEBUG("Retrying in {} ms. Remaining time: {} ms", backoff_time.count(), remaining_time.count());
        std::this_thread::sleep_for(backoff_time);
    }

    if (connected_) {
        return true;
    }

    // Failed after MAX_RETRIES attempts
    LOG_DEBUG("Max retries reached. Giving up on connection attempt.");
    return false;
}

void BluetoothAdapter::ConnectToPeer(std::chrono::time_point<std::chrono::steady_clock>& start_time) {
    LOG_DEBUG("ConnectToPeer: Configuring socket for connection.");
    const int MAX_RETRIES = 5;
    int       retry_count = 0;

    while (retry_count < MAX_RETRIES) {
        std::chrono::milliseconds remaining_time = GetRemainingConnectionTime(start_time);
        if (remaining_time <= std::chrono::milliseconds(0)) {
            LOG_INFO("Connection attempt timed out for peer {}.", btDeviceInfo_->mac_address);
            return;
        }

        ConfigureSocketForConnection(sockfd_, remaining_time);

        if (!WaitForSocket(sockfd_, remaining_time)) {
            return;
        }

        // Get remaining connection time and configure socket to timeout accordingly
        remaining_time = GetRemainingConnectionTime(start_time);
        if (remaining_time <= std::chrono::milliseconds(0)) {
            LOG_INFO("Connection request attempt timed out.");
            return;
        }
        ConfigureSocketForConnection(sockfd_, remaining_time);

        char addr_str[18];
        ba2str(&remoteAddr_.l2_bdaddr, addr_str);
        LOG_DEBUG("ConnectToPeer: Attempting to connect to remote address: {}, PSM: {}", addr_str, remoteAddr_.l2_psm);

        if (connect(sockfd_, (struct sockaddr*)&remoteAddr_, sizeof(remoteAddr_)) == 0) {
            connected_ = true;
            LOG_DEBUG("Successfully connected to peer: {}", addr_str);
            return;
        }

        int err = errno;
        LOG_DEBUG("Connection attempt failed. Error: {} ({})", anduril::util::errnoToString(err), err);

        switch (err) {
        case EISCONN:
            LOG_DEBUG("Already connected to {}", addr_str);
            return;
        case ECONNREFUSED:
        case EHOSTUNREACH:
        case EHOSTDOWN:
            LOG_DEBUG("Connection failed due to unavailable server, retrying...");
            break;
        case EINVAL:
            LOG_DEBUG("Invalid argument, retrying...");
            SetupRemoteAddr();
            break;
        case ETIMEDOUT:
        case EAGAIN:
            LOG_INFO("Connection attempt timed out for peer: {}", addr_str);
            return;
        default:
            LOG_ERROR("Unexpected error in connect. Errno: {}", err);
            return;
        }

        retry_count++;

        remaining_time = GetRemainingConnectionTime(start_time);
        if (remaining_time <= std::chrono::milliseconds(0)) {
            LOG_INFO("Connection attempt timed out during retry for peer {}.", btDeviceInfo_->mac_address);
            return;
        }

        // Calculate backoff time scaled to remaining time
        auto backoff_time = std::min(remaining_time / 2, // Use at most half of the remaining time
                                     std::chrono::milliseconds(100 * (1 << retry_count)) // Exponential backoff
        );

        LOG_DEBUG("Retrying in {} ms. Remaining time: {} ms", backoff_time.count(), remaining_time.count());
        std::this_thread::sleep_for(backoff_time);
    }

    LOG_DEBUG("Max retries reached. Giving up on connection attempt to peer {}.", btDeviceInfo_->mac_address);
}

void BluetoothAdapter::ListenForPeer(std::chrono::time_point<std::chrono::steady_clock>& start_time) {
    std::chrono::milliseconds remaining_time = GetRemainingConnectionTime(start_time);
    if (remaining_time <= std::chrono::milliseconds(0)) {
        LOG_INFO("Listen attempt timed out for peer {}.", btDeviceInfo_->mac_address);
        return;
    }
    ConfigureSocketForConnection(sockfd_listen_, remaining_time);

    char addr_str[18];
    ba2str(&localAddr_.l2_bdaddr, addr_str);
    LOG_DEBUG("ListenForPeer: Binding to address: {}, PSM: {}", addr_str, localAddr_.l2_psm);

    if (bind(sockfd_listen_, (struct sockaddr*)&localAddr_, sizeof(localAddr_)) < 0) {
        LOG_ERROR("Failed to bind listening socket. Error: {}", anduril::util::errnoToString());
        return;
    }
    LOG_DEBUG("ListenForPeer: Bound listening socket.");

    if (listen(sockfd_listen_, 1) < 0) {
        LOG_ERROR("Failed to listen on socket. Error: {}", anduril::util::errnoToString());
        return;
    }

    LOG_DEBUG("ListenForPeer: Waiting for incoming connections. Timeout: {} ms", conn_timeout_.count());

    struct sockaddr_l2 remote_addr = {0};
    socklen_t          opt         = sizeof(remote_addr);

    int client_sock = accept(sockfd_listen_, (struct sockaddr*)&remote_addr, &opt);
    if (client_sock >= 0) {
        sockfd_        = client_sock;
        connected_     = true;
        char buf[1024] = {0};
        ba2str(&remote_addr.l2_bdaddr, buf);
        LOG_DEBUG("Accepted connection from {}. New socket FD: {}", std::string(buf), client_sock);
    } else if (errno == ETIMEDOUT || errno == EAGAIN || errno == EWOULDBLOCK) {
        LOG_INFO("Accept operation timed out. No incoming connections within the timeout period.");
        close(client_sock);
    } else {
        LOG_ERROR("Error in accept: {}. Errno: {}", anduril::util::errnoToString(), errno);
        close(client_sock);
    }
}

int BluetoothAdapter::setConnTimeout(unsigned int milliseconds) {
    conn_timeout_ = std::chrono::milliseconds(milliseconds);
    anduril::telemetry::success(anduril::telemetry::EventOwner::SENSOR_INTERFACE_API,
                                anduril::telemetry::EventType::BRATISLAVA_SOCKET_SET_CONN_TIMEOUT,
                                linkTelemetryJSON_,
                                telemetryDeviceType_);
    return 0;
};

int BluetoothAdapter::setRecvTimeout(unsigned int milliseconds) {
    recv_timeout_ = std::chrono::milliseconds(milliseconds);
    if (connected_) {
        ConfigureSocketForComms();
    }
    anduril::telemetry::success(anduril::telemetry::EventOwner::SENSOR_INTERFACE_API,
                                anduril::telemetry::EventType::BRATISLAVA_SOCKET_SET_RECV_TIMEOUT,
                                linkTelemetryJSON_,
                                telemetryDeviceType_);
    return 0;
}

void BluetoothAdapter::ConfigureSocketForConnection(int& sockfd, const std::chrono::milliseconds& timeout) {
    // Set socket to blocking mode
    int flags = fcntl(sockfd_, F_GETFL, 0);
    fcntl(sockfd_, F_SETFL, flags & ~O_NONBLOCK);

    int reuse = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0 ||
        setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) < 0) {
        LOG_ERROR(std::string("setsockopt error: ") + anduril::util::errnoToString());
        anduril::telemetry::failure(anduril::telemetry::EventOwner::SENSOR_INTERFACE_API,
                                    anduril::telemetry::EventType::BRATISLAVA_SOCKET_SET_CONN_TIMEOUT,
                                    linkTelemetryJSON_,
                                    telemetryDeviceType_,
                                    "Failure configuring socket for connection.");
    }

    // Set socket timeout
    struct timeval tv;
    tv.tv_sec  = timeout.count() / 1000;
    tv.tv_usec = (timeout.count() % 1000) * 1000;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv) < 0 ||
        setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv) < 0) {
        LOG_ERROR(std::string("setsockopt timeout error: ") + anduril::util::errnoToString());
    }
}

void BluetoothAdapter::ConfigureSocketForComms() {
    // Set socket to blocking mode
    int flags = fcntl(sockfd_, F_GETFL, 0);
    fcntl(sockfd_, F_SETFL, flags & ~O_NONBLOCK);

    // Set socket timeout
    struct timeval recv_delay;
    recv_delay.tv_sec  = recv_timeout_.count() / 1000;
    recv_delay.tv_usec = (recv_timeout_.count() % 1000) * 1000;
    int status         = setsockopt(sockfd_, SOL_SOCKET, SO_RCVTIMEO, &recv_delay, sizeof(recv_delay));
    if (status < 0) {
        LOG_ERROR(std::string("setsockopt reset to default error: ") + anduril::util::errnoToString());
        anduril::telemetry::failure(anduril::telemetry::EventOwner::SENSOR_INTERFACE_API,
                                    anduril::telemetry::EventType::BRATISLAVA_SOCKET_SET_RECV_TIMEOUT,
                                    linkTelemetryJSON_,
                                    telemetryDeviceType_,
                                    "Failure resetting socket timeout for receiving messages.");
    }
}

void BluetoothAdapter::CreateSocket() {
    sockfd_ = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (sockfd_ == -1) {
        LOG_ERROR("Failed to create Bluetooth L2CAP socket");
        anduril::telemetry::failure(anduril::telemetry::EventOwner::SENSOR_INTERFACE_API,
                                    anduril::telemetry::EventType::BRATISLAVA_SOCKET_CREATED,
                                    linkTelemetryJSON_,
                                    telemetryDeviceType_,
                                    "Failed to create Bluetooth L2CAP socket");
    }
}

void BluetoothAdapter::CloseSocket() {
    if (sockfd_ >= 0) {
        close(sockfd_);
        sockfd_ = -1;
    }
}

bool BluetoothAdapter::CreateConnectionRequestSocket() {
    // Initialize a socket for connection requests
    sockfd_request_ = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (sockfd_request_ < 0) {
        LOG_ERROR("Failed to create connection request socket. Error: {}", anduril::util::errnoToString());
        return false;
    }
    return true;
}

bool BluetoothAdapter::CreateListeningSocket() {
    // Initialize a socket to listen for incoming connections
    sockfd_listen_ = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (sockfd_listen_ < 0) {
        LOG_ERROR("Failed to create listening socket. Error: {}", anduril::util::errnoToString());
        return false;
    }
    return true;
}

bool BluetoothAdapter::WaitForSocket(const int& sockfd, std::chrono::milliseconds timeout) {
    struct pollfd pfd;
    int           result;

    pfd.fd     = sockfd;
    pfd.events = POLLOUT;

    // Wait for the socket to be ready for writing
    result = poll(&pfd, 1, timeout.count());

    if (result > 0) {
        if (pfd.revents & POLLOUT) {
            // Socket is ready
            return true;
        }
    } else if (result == 0) {
        LOG_DEBUG("WaitForSocket: Poll timed out");
    } else {
        LOG_ERROR("WaitForSocket: Poll error: {} ({})", anduril::util::errnoToString(), errno);
    }

    return false;
}

std::chrono::milliseconds
BluetoothAdapter::GetRemainingConnectionTime(const std::chrono::time_point<std::chrono::steady_clock>& start_time) {
    std::chrono::steady_clock::time_point current_time = std::chrono::steady_clock::now();
    std::chrono::milliseconds             elapsed_time =
        std::chrono::duration_cast<std::chrono::milliseconds>(current_time - start_time);
    std::chrono::milliseconds remaining_time = conn_timeout_ - elapsed_time;
    return remaining_time;
}

void BluetoothAdapter::SetupLocalAddr() {
    const uint16_t remotePort = GenerateUniquePort(btDeviceInfo_->mac_address);

    memset(&localAddr_, 0, sizeof(localAddr_));
    localAddr_.l2_family = AF_BLUETOOTH;
    localAddr_.l2_psm    = htobs(remotePort);

    int dev_id = hci_get_route(NULL);
    int sock   = hci_open_dev(dev_id);

    if (dev_id < 0 || sock < 0) {
        LOG_ERROR("Failed to open HCI device");
        bdaddr_t bdaddr_any = {{0, 0, 0, 0, 0, 0}};
        bacpy(&localAddr_.l2_bdaddr, &bdaddr_any);
    } else {
        if (hci_read_bd_addr(sock, &localAddr_.l2_bdaddr, 1000) < 0) {
            LOG_ERROR("Failed to read local Bluetooth address");
            bdaddr_t bdaddr_any = {{0, 0, 0, 0, 0, 0}};
            bacpy(&localAddr_.l2_bdaddr, &bdaddr_any);
        }
        close(sock);
    }

    char addr_str[18];
    ba2str(&localAddr_.l2_bdaddr, addr_str);
    LOG_DEBUG("Local address set to: {}, PSM: {}", addr_str, localAddr_.l2_psm);
}

void BluetoothAdapter::SetupRemoteAddr() {
    if (localAddr_.l2_family == 0) { // local address uninitialized
        LOG_ERROR("The local address must be set prior to remote. This is in order "
                  "to obtain the local MAC address for defining the remote port.");
    }
    char localMac[18];
    ba2str(&localAddr_.l2_bdaddr, localMac);
    const uint16_t localPort = GenerateUniquePort(localMac);

    memset(&remoteAddr_, 0, sizeof(remoteAddr_));
    remoteAddr_.l2_family = AF_BLUETOOTH;
    remoteAddr_.l2_psm    = htobs(localPort);
    str2ba(btDeviceInfo_->mac_address, &remoteAddr_.l2_bdaddr);

    char addr_str[18];
    ba2str(&remoteAddr_.l2_bdaddr, addr_str);
    LOG_DEBUG("Remote address set to: {}, PSM: {}", addr_str, remoteAddr_.l2_psm);
}

uint16_t BluetoothAdapter::GenerateUniquePort(const char* mac_address) {
    const uint16_t PORT_RANGE_START = 0x1002;
    const uint16_t PORT_RANGE_END   = 0x1100;
    const uint16_t PORT_RANGE_SIZE  = (PORT_RANGE_END - PORT_RANGE_START) / 2 + 1;
    unsigned char  hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(mac_address), strlen(mac_address), hash);

    // Convert the first 2 bytes of the hash to an integer
    uint16_t hash_value = (hash[0] << 8) | hash[1];

    // Map the hash to our port range and ensure it's odd
    uint16_t port = PORT_RANGE_START + (hash_value % PORT_RANGE_SIZE) * 2 + 1;

    return port;
}

#endif
