#include "WifiAdapter.h"

#include <anduril/util/ErrnoToString.h>
#include <anduril/util/IpAddrFormat.h>
#include <anduril/util/LogFormatters.h>
#include <anduril/util/Logger.h>

#include <BratislavaSocketRegistry.h>

#include <string>
#include <vector>

WifiAdapter::WifiAdapter(BratislavaSocket* bsock) : bsock_(bsock), gateway_(nullptr) {
    wifiDeviceInfo_    = &bsock->link.wifiDeviceInfo;
    linkTelemetryJSON_ = bsock->link;
    deviceType_        = GetWifiDeviceType();
    if (!GetMacAddress(mac_address_, sizeof(mac_address_))) {
        LOG_ERROR("Unable to get MAC address");
    }
    if (!GetSSID(ssid_, sizeof(ssid_))) {
        LOG_ERROR("Unable to get SSID");
    }
    CreateConnectionRequestSocket();
    CreateListeningSocket();
    CreateSocket();
}

WifiAdapter::~WifiAdapter() {
    close(sockfd_request_);
    close(sockfd_listen_);
    CloseSocket();
    LOG_DEBUG("Wi-Fi socket closed.");
    anduril::telemetry::success(anduril::telemetry::EventOwner::SENSOR_INTERFACE_API,
                                anduril::telemetry::EventType::BRATISLAVA_SOCKET_DESTROYED,
                                linkTelemetryJSON_,
                                telemetryDeviceType_);
}

int WifiAdapter::disconn() {
    connected_ = false;
    CloseSocket();
    CreateSocket();
    if (sockfd_ != -1) {
        return 0;
    }
    return -1;
}

int WifiAdapter::conn() {
    connected_ = false;
    LOG_DEBUG("Starting connection process. Device MAC: {}, Timeout: {} ms, Device Type: {}",
              wifiDeviceInfo_->mac_address,
              conn_timeout_.count(),
              deviceType_);

    auto start_time = std::chrono::steady_clock::now();

    switch (deviceType_) {
    case WIFI_ROUTER:
        LOG_DEBUG("Starting ListenForPeer (Server)");
        remote_port_ = BratislavaSocketRegistry::GetInstance().GetRemotePort(wifiDeviceInfo_->mac_address);
        SetupLocalAddr();
        SetupRemoteAddr();
        ListenForPeer(start_time);
        break;
    case WIFI_ADAPTER:
        LOG_DEBUG("Sending connection request (Client).");
        BratislavaSocketRegistry::GetInstance().AddLocalPort(wifiDeviceInfo_->mac_address);
        SetupLocalAddr();
        if (!SendConnectionRequest(start_time)) {
            return -1;
        }
        SetupRemoteAddr();
        LOG_DEBUG("Starting ConnectToPeer (Client)");
        ConnectToPeer(start_time);
        break;
    default:
        LOG_ERROR("[-] Invalid device type when connecting to remote device.");
    }

    if (!connected_) {
        LOG_ERROR("Failed to establish connection. Device MAC: {}", wifiDeviceInfo_->mac_address);
        anduril::telemetry::failure(anduril::telemetry::EventOwner::SENSOR_INTERFACE_API,
                                    anduril::telemetry::EventType::BRATISLAVA_SOCKET_CONNECTED,
                                    linkTelemetryJSON_,
                                    telemetryDeviceType_,
                                    "Failed to establish connection");
        return -1;
    }

    BratislavaSocketRegistry::GetInstance().Connect(bsock_);
    ConfigureSocketForComms();
    LOG_INFO("Connection established. Device MAC: {}", wifiDeviceInfo_->mac_address);
    anduril::telemetry::success(anduril::telemetry::EventOwner::SENSOR_INTERFACE_API,
                                anduril::telemetry::EventType::BRATISLAVA_SOCKET_CONNECTED,
                                linkTelemetryJSON_,
                                telemetryDeviceType_);
    return 0;
}

int WifiAdapter::send(const void* buf, size_t len) {
    if (!BratislavaSocketRegistry::GetInstance().IsConnected(bsock_)) {
        LOG_DEBUG("Attempted to send message on a disconnected Wi-Fi socket.");
        anduril::telemetry::failure(anduril::telemetry::EventOwner::SENSOR_INTERFACE_API,
                                    anduril::telemetry::EventType::BRATISLAVA_SOCKET_SEND_MESSAGE,
                                    nlohmann::json{{"length", len}, {"link", linkTelemetryJSON_}},
                                    telemetryDeviceType_,
                                    "EPIPE: Bratislava socket is no longer connected.");
        errno = EPIPE;
        return -1;
    }

    LOG_DEBUG("Sending Wi-Fi socket message.");
    const auto status = ::send(sockfd_, buf, len, MSG_NOSIGNAL);

    if (status < 0) {
        auto errormsg = std::string("Wi-Fi send error: ") + anduril::util::errnoToString();
        LOG_ERROR(errormsg);
        anduril::telemetry::failure(anduril::telemetry::EventOwner::SENSOR_INTERFACE_API,
                                    anduril::telemetry::EventType::BRATISLAVA_SOCKET_SEND_MESSAGE,
                                    nlohmann::json{{"length", len}, {"link", linkTelemetryJSON_}},
                                    telemetryDeviceType_,
                                    errormsg);
        return static_cast<int>(status);
    }
    anduril::telemetry::success(anduril::telemetry::EventOwner::SENSOR_INTERFACE_API,
                                anduril::telemetry::EventType::BRATISLAVA_SOCKET_SEND_MESSAGE,
                                nlohmann::json{{"length", len}, {"link", linkTelemetryJSON_}},
                                telemetryDeviceType_);
    LOG_DEBUG("Wi-Fi socket message sent.");
    return static_cast<int>(status);
}

int WifiAdapter::recv(void* buf, size_t len) {
    if (!BratislavaSocketRegistry::GetInstance().IsConnected(bsock_)) {
        LOG_DEBUG("Attempted to receive message on a disconnected Wi-Fi socket.");
        anduril::telemetry::failure(anduril::telemetry::EventOwner::SENSOR_INTERFACE_API,
                                    anduril::telemetry::EventType::BRATISLAVA_SOCKET_RECEIVE_MESSAGE,
                                    nlohmann::json{{"length", len}, {"link", linkTelemetryJSON_}},
                                    telemetryDeviceType_,
                                    "EPIPE: Bratislava socket is no longer connected.");
        errno = EPIPE;
        return -1;
    }

    LOG_DEBUG("Receiving Wi-Fi socket message.");
    const auto bytes_read = ::recv(sockfd_, buf, len, MSG_NOSIGNAL);

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
            std::string errormsg = std::string("Wi-Fi recv error: ") + anduril::util::errnoToString();
            LOG_ERROR(errormsg);
            anduril::telemetry::failure(anduril::telemetry::EventOwner::SENSOR_INTERFACE_API,
                                        anduril::telemetry::EventType::BRATISLAVA_SOCKET_RECEIVE_MESSAGE,
                                        nlohmann::json{{"length", len}, {"link", linkTelemetryJSON_}},
                                        telemetryDeviceType_,
                                        errormsg);
        }
        errno = failure_errno;
        return static_cast<int>(bytes_read);
    }

    if (bytes_read == 0) {
        std::string debugmsg = "Zero read on Wi-Fi socket, connection closed by peer";
        LOG_DEBUG(debugmsg);
        anduril::telemetry::failure(anduril::telemetry::EventOwner::SENSOR_INTERFACE_API,
                                    anduril::telemetry::EventType::BRATISLAVA_SOCKET_RECEIVE_MESSAGE,
                                    nlohmann::json{{"length", len}, {"link", linkTelemetryJSON_}},
                                    telemetryDeviceType_,
                                    debugmsg);
        return static_cast<int>(bytes_read);
    }

    anduril::telemetry::success(anduril::telemetry::EventOwner::SENSOR_INTERFACE_API,
                                anduril::telemetry::EventType::BRATISLAVA_SOCKET_RECEIVE_MESSAGE,
                                nlohmann::json{{"length", bytes_read}, {"link", linkTelemetryJSON_}},
                                telemetryDeviceType_);

    LOG_DEBUG("Wi-Fi socket message received.");
    return static_cast<int>(bytes_read);
}

int WifiAdapter::setConnTimeout(const unsigned int milliseconds) {
    conn_timeout_ = std::chrono::milliseconds(milliseconds);
    anduril::telemetry::success(anduril::telemetry::EventOwner::SENSOR_INTERFACE_API,
                                anduril::telemetry::EventType::BRATISLAVA_SOCKET_SET_CONN_TIMEOUT,
                                linkTelemetryJSON_,
                                telemetryDeviceType_);
    return 0;
}

int WifiAdapter::setRecvTimeout(const unsigned int milliseconds) {
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

void WifiAdapter::ConfigureSocketForConnection(const int& sockfd, const std::chrono::milliseconds& timeout) {
    // Set socket to blocking mode
    const int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags & ~O_NONBLOCK);

    constexpr int reuse = 1;
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
    struct timeval tv {};

    tv.tv_sec  = timeout.count() / 1000;
    tv.tv_usec = (timeout.count() % 1000) * 1000;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) < 0 ||
        setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) < 0) {
        LOG_ERROR(std::string("setsockopt timeout error: ") + anduril::util::errnoToString());
    }
}

void WifiAdapter::ConfigureSocketForComms() {
    // Set socket to blocking mode
    const int flags = fcntl(sockfd_, F_GETFL, 0);
    fcntl(sockfd_, F_SETFL, flags & ~O_NONBLOCK);

    // Set socket timeout
    struct timeval recv_delay {};

    recv_delay.tv_sec  = recv_timeout_.count() / 1000;
    recv_delay.tv_usec = (recv_timeout_.count() % 1000) * 1000;
    const int status   = setsockopt(sockfd_, SOL_SOCKET, SO_RCVTIMEO, &recv_delay, sizeof(recv_delay));
    if (status < 0) {
        LOG_ERROR(std::string("setsockopt reset to default error: ") + anduril::util::errnoToString());
        anduril::telemetry::failure(anduril::telemetry::EventOwner::SENSOR_INTERFACE_API,
                                    anduril::telemetry::EventType::BRATISLAVA_SOCKET_SET_RECV_TIMEOUT,
                                    linkTelemetryJSON_,
                                    telemetryDeviceType_,
                                    "Failure resetting socket timeout for receiving messages.");
    }
}

void WifiAdapter::CreateSocket() {
    sockfd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd_ < 0) {
        LOG_ERROR("Failed to create Wi-Fi socket.");
        anduril::telemetry::failure(anduril::telemetry::EventOwner::SENSOR_INTERFACE_API,
                                    anduril::telemetry::EventType::BRATISLAVA_SOCKET_CREATED,
                                    linkTelemetryJSON_,
                                    telemetryDeviceType_,
                                    "Failed to create Wi-Fi socket.");
    }
}

void WifiAdapter::CloseSocket() {
    if (sockfd_ >= 0) {
        close(sockfd_);
        sockfd_ = -1;
    }
}

bool WifiAdapter::CreateConnectionRequestSocket() {
    // Initialize a socket for connection requests
    sockfd_request_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd_request_ < 0) {
        LOG_ERROR("Failed to create connection request socket. Error: {}", anduril::util::errnoToString());
        return false;
    }
    return true;
}

bool WifiAdapter::CreateListeningSocket() {
    // Initialize a socket to listen for incoming connections
    sockfd_listen_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd_listen_ < 0) {
        LOG_ERROR("Failed to create listening socket. Error: {}", anduril::util::errnoToString());
        return false;
    }
    return true;
}

bool WifiAdapter::WaitForSocket(const int& sockfd, const std::chrono::milliseconds timeout) {
    pollfd pfd{};

    pfd.fd     = sockfd;
    pfd.events = POLLOUT;

    // Wait for the socket to be ready for writing
    const int result = poll(&pfd, 1, static_cast<int>(timeout.count()));

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

bool WifiAdapter::SendConnectionRequest(const std::chrono::time_point<std::chrono::steady_clock>& start_time) {
    constexpr int MAX_RETRIES  = 5;
    int           retry_count  = 0;
    bool          is_connected = false;

    // Get IP address of the router
    if (GetAccessPointIP() < 0) {
        LOG_DEBUG("Unable to set remote address for access point");
        return false;
    }

    // Configure remote request address for connection requests
    struct sockaddr_in remoteRequestAddr {};

    memset(&remoteRequestAddr, 0, sizeof(remoteRequestAddr));
    remoteRequestAddr.sin_family = AF_INET;
    remoteRequestAddr.sin_port   = htons(BRATISLAVA_PORT);
    if (inet_pton(AF_INET, gateway_, &remoteRequestAddr.sin_addr) != 1) {
        LOG_ERROR("[-] Invalid IP address format for gateway: {}", gateway_);
        return false;
    }

    LOG_DEBUG("Attempting to connect to the server ({}:{})", gateway_, ntohs(remoteRequestAddr.sin_port));

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

        // Attempt to connect
        if (connect(sockfd_request_,
                    reinterpret_cast<struct sockaddr*>(&remoteRequestAddr),
                    sizeof(remoteRequestAddr)) == 0) {
            LOG_DEBUG("Connection request with {}:{} is successful", gateway_, ntohs(remoteRequestAddr.sin_port));
            is_connected = true;
            break;
        }

        // Handle connection status
        int err = errno;
        LOG_DEBUG("Connection attempt failed. Error: {} ({})", anduril::util::errnoToString(err), err);

        switch (err) {
        case EISCONN:
            LOG_DEBUG("Connection request has already succeeded.");
            is_connected = true;
            break;
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
            LOG_INFO("Connection request timed out for remote address: {}:{}", gateway_, remoteRequestAddr.sin_port);
            return false;
        default:
            LOG_ERROR("Unexpected error in connect. Errno: {}", err);
            return false;
        }

        if (is_connected) {
            break;
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

    if (is_connected) {
        LOG_DEBUG("Connection request socket has connected to the remote server. Sending device information.");
        if (::send(sockfd_request_, mac_address_, sizeof(mac_address_), MSG_NOSIGNAL) < 0) {
            LOG_ERROR("[-] Failed to send MAC address");
            return false;
        }
        if (::send(sockfd_request_, ssid_, sizeof(ssid_), MSG_NOSIGNAL) < 0) {
            LOG_ERROR("[-] Failed to send SSID");
            return false;
        }

        const uint8_t network_port = htons(localAddr_.sin_port);
        if (::send(sockfd_request_, &network_port, sizeof(network_port), MSG_NOSIGNAL) < 0) {
            LOG_ERROR("[-] Failed to send local port to remote server");
            return false;
        }

        std::string localPortString = std::to_string(ntohs(localAddr_.sin_port));
        LOG_DEBUG("Sent Client MAC: {};  SSID: {}; Port: {}; to the remote server.",
                  mac_address_,
                  ssid_,
                  localPortString);

        if (::recv(sockfd_request_, &remote_port_, sizeof(remote_port_), MSG_NOSIGNAL) <= 0) {
            LOG_ERROR("[-] Failed to receive port assignment from access point");
            return false;
        }
        remote_port_ = ntohs(remote_port_);

        LOG_DEBUG("Received port assignment for dedicated comms channel: {}", remote_port_);

        if (remote_port_ < 0) {
            LOG_ERROR("[-] Invalid port assignment for comms channel.");
            return false;
        }

        return true;
    }

    // Failed after MAX_RETRIES attempts
    LOG_DEBUG("Max retries reached. Giving up on connection attempt.");
    return false;
}

void WifiAdapter::ConnectToPeer(const std::chrono::time_point<std::chrono::steady_clock>& start_time) {
    LOG_DEBUG("ConnectToPeer: Configuring socket for connection.");
    constexpr int MAX_RETRIES = 5;
    int           retry_count = 0;

    while (retry_count < MAX_RETRIES) {
        std::chrono::milliseconds remaining_time = GetRemainingConnectionTime(start_time);
        if (remaining_time <= std::chrono::milliseconds(0)) {
            LOG_INFO("Connection attempt timed out for peer {}.", wifiDeviceInfo_->mac_address);
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

        auto        remoteAddrString = anduril::util::ipv4String(remoteAddr_.sin_addr);
        std::string remotePortString = std::to_string(ntohs(remoteAddr_.sin_port));
        LOG_DEBUG("ConnectToPeer: Attempting to connect to {}:{}", remoteAddrString, remotePortString);

        if (connect(sockfd_, reinterpret_cast<struct sockaddr*>(&remoteAddr_), sizeof(remoteAddr_)) == 0) {
            connected_ = true;
            LOG_DEBUG("Successfully connected to peer: {}:{}", remoteAddrString, remotePortString);
            return;
        }

        int err = errno;
        LOG_DEBUG("Connection attempt failed. Error: {} ({})", anduril::util::errnoToString(err), err);

        switch (err) {
        case EISCONN:
            LOG_DEBUG("Already connected to {}:{}", remoteAddrString, remotePortString);
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
            LOG_INFO("Connection attempt timed out for peer: {}:{}", remoteAddrString, remotePortString);
            return;
        default:
            LOG_ERROR("Unexpected error in connect. Errno: {}", err);
            return;
        }

        retry_count++;

        remaining_time = GetRemainingConnectionTime(start_time);
        if (remaining_time <= std::chrono::milliseconds(0)) {
            LOG_INFO("Connection attempt timed out during retry for peer {}.", wifiDeviceInfo_->mac_address);
            return;
        }

        // Calculate backoff time scaled to remaining time
        auto backoff_time = std::min(remaining_time / 2, // Use at most half of the remaining time
                                     std::chrono::milliseconds(100 * (1 << retry_count)) // Exponential backoff
        );

        LOG_DEBUG("Retrying in {} ms. Remaining time: {} ms", backoff_time.count(), remaining_time.count());
        std::this_thread::sleep_for(backoff_time);
    }

    LOG_DEBUG("Max retries reached. Giving up on connection attempt to peer {}.", wifiDeviceInfo_->mac_address);
}

void WifiAdapter::ListenForPeer(const std::chrono::time_point<std::chrono::steady_clock>& start_time) {
    const std::chrono::milliseconds remaining_time = GetRemainingConnectionTime(start_time);
    if (remaining_time <= std::chrono::milliseconds(0)) {
        LOG_INFO("Listen attempt timed out for peer {}.", wifiDeviceInfo_->mac_address);
        return;
    }
    ConfigureSocketForConnection(sockfd_listen_, remaining_time);

    auto        localAddrString = anduril::util::ipv4String(localAddr_.sin_addr);
    std::string localPortString = std::to_string(ntohs(localAddr_.sin_port));
    LOG_DEBUG("ListenForPeer: Binding to {}:{}", localAddrString, localPortString);

    if (bind(sockfd_listen_, reinterpret_cast<struct sockaddr*>(&localAddr_), sizeof(localAddr_)) < 0) {
        LOG_ERROR("Failed to bind listening socket. Error: {}", anduril::util::errnoToString());
        return;
    }
    LOG_DEBUG("ListenForPeer: Bound listening socket.");

    if (listen(sockfd_listen_, BACKLOG) < 0) {
        LOG_ERROR("Failed to listen on socket. Error: {}", anduril::util::errnoToString());
        return;
    }

    LOG_DEBUG("ListenForPeer: Waiting for incoming connections. Timeout: {} ms", conn_timeout_.count());

    struct sockaddr_in remote_addr = {};
    socklen_t          opt         = sizeof(remote_addr);

    const int client_sock = accept(sockfd_listen_, reinterpret_cast<struct sockaddr*>(&remote_addr), &opt);
    if (client_sock >= 0) {
        sockfd_                      = client_sock;
        connected_                   = true;
        auto        remoteAddrString = anduril::util::ipv4String(remote_addr.sin_addr);
        std::string remotePortString = std::to_string(ntohs(remote_addr.sin_port));
        LOG_DEBUG("Accepted connection from {}:{}. New socket FD: {}", remoteAddrString, remotePortString, client_sock);
    } else if (errno == ETIMEDOUT || errno == EAGAIN || errno == EWOULDBLOCK) {
        LOG_INFO("Accept operation timed out. No incoming connections within the timeout period.");
        close(client_sock);
    } else {
        LOG_ERROR("Error in accept: {}. Errno: {}", anduril::util::errnoToString(), errno);
        close(client_sock);
    }
}

void WifiAdapter::SetupLocalAddr() {
    const int local_port = BratislavaSocketRegistry::GetInstance().GetLocalPort(wifiDeviceInfo_->mac_address);
    memset(&localAddr_, 0, sizeof(localAddr_));
    localAddr_.sin_family      = AF_INET;
    localAddr_.sin_port        = htons(local_port);
    localAddr_.sin_addr.s_addr = htonl(INADDR_ANY);
}

void WifiAdapter::SetupRemoteAddr() {
    memset(&remoteAddr_, 0, sizeof(remoteAddr_));
    remoteAddr_.sin_family = AF_INET;
    remoteAddr_.sin_port   = htons(remote_port_);

    if (remote_port_ < 0) {
        LOG_ERROR("Configured remote address with invalid port: {}", remote_port_);
    }

    switch (deviceType_) {
    case WIFI_ADAPTER:
        if (inet_pton(AF_INET, gateway_, &remoteAddr_.sin_addr) != 1) {
            LOG_ERROR("[-] Invalid IP address format for gateway: {}", gateway_);
            return;
        }
        break;
    case WIFI_ROUTER:
        remoteAddr_.sin_addr.s_addr = inet_addr(GetLocalIP());
        break;
    default:
        LOG_ERROR("[-] Invalid device type when configuring remote address.");
    }

    auto        remoteAddrString = anduril::util::ipv4String(remoteAddr_.sin_addr);
    std::string remotePortString = std::to_string(ntohs(remoteAddr_.sin_port));
    LOG_DEBUG("Remote address set to: {}:{}", remoteAddrString, remotePortString);
}

WifiAdapter::NMSafeWrapper<NMClient> WifiAdapter::GetNMClient() {
    GError*   error  = nullptr;
    NMClient* client = nm_client_new(nullptr, &error);

    if (!client) {
        LOG_ERROR("[-] Error: Could not create NMClient: {}", error->message);
        g_error_free(error);
        return {};
    }

    return NMSafeWrapper(client);
}

NMDevice* WifiAdapter::GetNMDevice(NMClient* client) {
    const GPtrArray* devices     = nm_client_get_devices(client);
    NMDevice*        wifi_device = nullptr;

    for (guint i = 0; i < devices->len; ++i) {
        auto* device = NM_DEVICE(g_ptr_array_index(devices, i));
        if (NM_IS_DEVICE_WIFI(device)) {
            const NMDeviceState state = nm_device_get_state(device);
            if (state == NM_DEVICE_STATE_ACTIVATED) {
                return device; // Return immediately if an activated device is found
            }
            wifi_device = device;
        }
    }

    // Return connected device if found, otherwise disconnected device
    if (wifi_device)
        return wifi_device;

    LOG_ERROR("[-] Error: No Wi-Fi device found.");
    return nullptr;
}

WifiDeviceType WifiAdapter::GetWifiDeviceType() {
    auto       client = GetNMClient();
    const auto device = GetNMDevice(client.Ref());
    switch (nm_device_wifi_get_mode(NM_DEVICE_WIFI(device))) {
    case NM_802_11_MODE_UNKNOWN:
        LOG_ERROR("[-] Error: Unknown Wi-Fi device mode");
        return WIFI_UNKNOWN;
    case NM_802_11_MODE_AP:
        LOG_DEBUG("Wi-Fi Device Mode: Router");
        return WIFI_ROUTER;
    case NM_802_11_MODE_ADHOC:
    case NM_802_11_MODE_INFRA:
    case NM_802_11_MODE_MESH:
        LOG_DEBUG("Wi-Fi Device Mode: Adapter");
        return WIFI_ADAPTER;
    }
    return WIFI_UNKNOWN;
}

std::chrono::milliseconds
WifiAdapter::GetRemainingConnectionTime(const std::chrono::time_point<std::chrono::steady_clock>& start_time) const {
    const std::chrono::steady_clock::time_point current_time = std::chrono::steady_clock::now();
    const auto elapsed_time = std::chrono::duration_cast<std::chrono::milliseconds>(current_time - start_time);
    const std::chrono::milliseconds remaining_time = conn_timeout_ - elapsed_time;
    return remaining_time;
}

const char* WifiAdapter::GetLocalIP() {
    auto        client  = GetNMClient();
    const auto  device  = GetNMDevice(client.Ref());
    const char* ip_addr = nullptr;

    if (NMIPConfig* ip_config = nm_device_get_ip4_config(device)) {
        const GPtrArray* addresses = nm_ip_config_get_addresses(ip_config);
        if (addresses && addresses->len > 0) {
            auto* address = static_cast<NMIPAddress*>(g_ptr_array_index(addresses, 0));
            ip_addr       = nm_ip_address_get_address(address);
        }
    }

    if (!ip_addr) {
        LOG_ERROR("AP IP address not found.");
    }

    return ip_addr;
}

int WifiAdapter::GetAccessPointIP() {
    auto       client = GetNMClient();
    const auto device = GetNMDevice(client.Ref());

    // Get the active connection of the Wi-Fi device
    NMActiveConnection* active_connection = nm_device_get_active_connection(device);
    if (!active_connection) {
        LOG_ERROR("Error: No active Wi-Fi connection found.");
        return -1;
    }

    // Get the IP configuration of the active connection
    NMIPConfig* ip_config = nm_active_connection_get_ip4_config(active_connection);
    if (!ip_config) {
        LOG_DEBUG("Error: No IPv4 configuration found.");
        return -1;
    }

    // Get the gateway (which is typically the access point's IP address)
    const char* gateway = nm_ip_config_get_gateway(ip_config);
    if (gateway) {
        SetGateway(gateway);
    } else {
        LOG_ERROR("Error: No gateway IP address found.");
        return -1;
    }

    return 0;
}

void WifiAdapter::SetGateway(const char* gateway) {
    // Free the old gateway address if it was set
    if (gateway_) {
        free(gateway_);
        gateway_ = nullptr;
    }

    // Duplicate the input string and store it in the member variable
    if (gateway) {
        gateway_ = strdup(gateway); // Allocate and copy the string
    }
}

bool WifiAdapter::GetSSID(char* ssid, size_t ssid_size) {
    if (ssid == nullptr || ssid_size < 256) {
        LOG_ERROR("Invalid buffer or buffer size");
        return false;
    }

    auto       client = GetNMClient();
    const auto device = GetNMDevice(client.Ref());

    const char*    ssid_str;
    bool           found = false;
    NMAccessPoint* access_point =
        nm_device_wifi_get_access_point_by_path(NM_DEVICE_WIFI(device), nm_device_get_path(device));
    if (!access_point) {
        ssid_str = g_get_host_name();
    } else {
        GBytes*     ssid_bytes = nm_access_point_get_ssid(access_point);
        gsize       ssid_len;
        const auto* ssid_data = static_cast<const guint8*>(g_bytes_get_data(ssid_bytes, &ssid_len));
        ssid_str              = nm_utils_ssid_to_utf8(ssid_data, ssid_len);
    }

    if (ssid_str) {
        strncpy(ssid, ssid_str, ssid_size - 1);
        ssid[ssid_size - 1] = '\0';
        found               = true;
    }

    return found;
}

bool WifiAdapter::GetMacAddress(char* mac_addr, size_t mac_addr_size) {
    if (mac_addr == nullptr || mac_addr_size < 18) {
        LOG_ERROR("Invalid buffer or buffer size");
        return false;
    }

    auto       client = GetNMClient();
    const auto device = GetNMDevice(client.Ref());

    bool found = false;
    if (const char* hw_addr = nm_device_get_hw_address(device)) {
        strncpy(mac_addr, hw_addr, mac_addr_size - 1);
        mac_addr[mac_addr_size - 1] = '\0';
        found                       = true;
    }

    return found;
}
