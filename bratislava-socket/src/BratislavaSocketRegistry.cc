#include "BratislavaSocketRegistry.h"

#include <anduril/util/Logger.h>

#include <algorithm>

BratislavaSocketRegistry::BratislavaSocketRegistry() {
    InitializeLocalPorts();
}

BratislavaSocketRegistry::~BratislavaSocketRegistry() {}

BratislavaSocketRegistry& BratislavaSocketRegistry::GetInstance() {
    static BratislavaSocketRegistry instance;
    return instance;
}

void BratislavaSocketRegistry::Add(BratislavaSocket* socket) {
    std::lock_guard<std::recursive_mutex> lock(mutex_recursive_);
    CreateConfig(socket);
    sockets_.push_back(socket);
}

bool BratislavaSocketRegistry::Remove(BratislavaSocket* socket) {
    std::lock_guard<std::recursive_mutex> lock(mutex_recursive_);
    auto                                  it = std::find(sockets_.begin(), sockets_.end(), socket);
    if (it != sockets_.end()) {
        ReleaseLocalPort(*it);
        Disconnect(*it);
        RemoveConfig(*it);
        sockets_.erase(it);
        return true;
    }
    return false;
}

bool BratislavaSocketRegistry::Exists(BratislavaSocket* socket) {
    std::lock_guard<std::recursive_mutex> lock(mutex_recursive_);
    auto                                  it = std::find(sockets_.begin(), sockets_.end(), socket);
    if (it != sockets_.end()) {
        return true;
    }
    return false;
}

bool BratislavaSocketRegistry::Exists(const std::string& device_id) {
    std::unique_lock<std::recursive_mutex> lock(mutex_recursive_);
    BratislavaSocketConfig                 config;
    return GetConfig(device_id, config) && config.socket != nullptr;
}

void BratislavaSocketRegistry::CreateConfig(BratislavaSocket* socket) {
    std::string device_id = GetDeviceID(socket);

    std::unique_lock<std::shared_mutex> lock(mutex_shared_config_);
    auto                                it = device_id_to_config_.find(device_id);

    if (it != device_id_to_config_.end()) {
        // Config already exists, just update it. We already hold the unique lock,
        // so use the lock-free variant to avoid re-locking a non-recursive mutex.
        BratislavaSocketConfig updatedConfig = *(it->second);
        updatedConfig.socket                 = socket;
        updatedConfig.device_id              = device_id;
        UpdateConfigLocked(updatedConfig);

        sock_to_config_[socket] = it->second;
    } else {
        // Config doesn't exist, create new one
        BratislavaSocketConfig* newConfig = new BratislavaSocketConfig();
        newConfig->socket                 = socket;
        newConfig->device_id              = device_id;
        sock_to_config_[socket]           = newConfig;
        device_id_to_config_[device_id]   = newConfig;
    }
}

void BratislavaSocketRegistry::CreateConfig(const std::string& device_id) {
    std::unique_lock<std::shared_mutex> lock(mutex_shared_config_);
    auto                                it = device_id_to_config_.find(device_id);

    if (it == device_id_to_config_.end()) {
        BratislavaSocketConfig* config  = new BratislavaSocketConfig();
        config->device_id               = device_id;
        device_id_to_config_[device_id] = config;
    }
}

void BratislavaSocketRegistry::RemoveConfig(BratislavaSocket* socket) {
    std::unique_lock<std::shared_mutex> lock(mutex_shared_config_);
    auto                                it = sock_to_config_.find(socket);
    if (it != sock_to_config_.end()) {
        device_id_to_config_.erase(it->second->device_id);
        delete it->second;
        sock_to_config_.erase(it);
    }
}

void BratislavaSocketRegistry::RemoveUnusedConfig(const std::string& device_id) {
    std::unique_lock<std::shared_mutex> lock(mutex_shared_config_);
    auto                                it = device_id_to_config_.find(device_id);
    if (it != device_id_to_config_.end() && !(it->second->socket)) {
        delete it->second;
        device_id_to_config_.erase(it);
    }
}

bool BratislavaSocketRegistry::GetConfig(BratislavaSocket* socket, BratislavaSocketConfig& config) {
    std::shared_lock<std::shared_mutex> lock(mutex_shared_config_);
    auto                                it = sock_to_config_.find(socket);
    if (it == sock_to_config_.end()) {
        return false;
    }
    config = *(it->second);
    return true;
}

bool BratislavaSocketRegistry::GetConfig(const std::string& device_id, BratislavaSocketConfig& config) {
    std::shared_lock<std::shared_mutex> lock(mutex_shared_config_);
    auto                                it = device_id_to_config_.find(device_id);
    if (it == device_id_to_config_.end()) {
        return false;
    }
    config = *(it->second);
    return true;
}

bool BratislavaSocketRegistry::UpdateConfig(const BratislavaSocketConfig& config) {
    std::unique_lock<std::shared_mutex> lock(mutex_shared_config_);
    return UpdateConfigLocked(config);
}

bool BratislavaSocketRegistry::UpdateConfigLocked(const BratislavaSocketConfig& config) {
    // Try to find the config using the socket
    auto it_sock = sock_to_config_.find(config.socket);
    if (it_sock != sock_to_config_.end()) {
        *(it_sock->second) = config;
        return true;
    }

    // If not found by socket, try to find by device ID
    auto it_id = device_id_to_config_.find(config.device_id);
    if (it_id != device_id_to_config_.end()) {
        *(it_id->second) = config;
        return true;
    }

    // Config not found in either map
    return false;
}

bool BratislavaSocketRegistry::Connect(BratislavaSocket* socket) {
    std::unique_lock<std::shared_mutex> lock(mutex_shared_registry_);
    BratislavaSocketConfig              config;
    if (!GetConfig(socket, config)) {
        return false;
    }
    config.connected = true;
    return UpdateConfig(config);
}

bool BratislavaSocketRegistry::Disconnect(const std::string& device_id) {
    LOG_DEBUG("Disconnecting Bratislava Socket : {}", device_id);
    std::unique_lock<std::shared_mutex> lock(mutex_shared_registry_);
    BratislavaSocketConfig              config;
    if (!GetConfig(device_id, config)) {
        return false;
    }
    config.connected = false;
    return UpdateConfig(config);
}

bool BratislavaSocketRegistry::Disconnect(BratislavaSocket* socket) {
    LOG_DEBUG("Disconnecting Bratislava Socket");
    std::unique_lock<std::shared_mutex> lock(mutex_shared_registry_);
    BratislavaSocketConfig              config;
    if (!GetConfig(socket, config)) {
        return false;
    }
    config.connected = false;
    return UpdateConfig(config);
}

bool BratislavaSocketRegistry::IsConnected(const std::string& device_id) {
    std::shared_lock<std::shared_mutex> lock(mutex_shared_registry_);
    BratislavaSocketConfig              config;
    if (!GetConfig(device_id, config)) {
        return false;
    }
    return config.connected;
}

bool BratislavaSocketRegistry::IsConnected(BratislavaSocket* socket) {
    std::shared_lock<std::shared_mutex> lock(mutex_shared_registry_);
    BratislavaSocketConfig              config;
    if (!GetConfig(socket, config)) {
        return false;
    }
    return config.connected;
}

bool BratislavaSocketRegistry::AddIP(const std::string& device_id, const std::string& device_ip) {
    std::unique_lock<std::shared_mutex> lock(mutex_shared_registry_);
    BratislavaSocketConfig              config;
    if (!GetConfig(device_id, config)) {
        return false;
    }
    config.ip = device_ip;
    return UpdateConfig(config);
}

bool BratislavaSocketRegistry::GetIP(const std::string& device_id, std::string& device_ip) {
    std::shared_lock<std::shared_mutex> lock(mutex_shared_registry_);
    BratislavaSocketConfig              config;
    if (!GetConfig(device_id, config)) {
        return false;
    }
    device_ip = config.ip;
    return true;
}

bool BratislavaSocketRegistry::AddRemotePort(const std::string& device_id, const int& port) {
    std::unique_lock lock(mutex_shared_registry_);
    if (port < 0) {
        LOG_ERROR("Remote Port assignment {} for device ID {} is invalid.", port, device_id);
        return false;
    }
    BratislavaSocketConfig config;
    if (!GetConfig(device_id, config)) {
        LOG_ERROR("Failed to acquire remote port for Bratislava Socket with device ID {}.", device_id);
        return false;
    }
    config.remote_port = port;
    return UpdateConfig(config);
}

int BratislavaSocketRegistry::GetRemotePort(const std::string& device_id) {
    std::shared_lock       lock(mutex_shared_registry_);
    BratislavaSocketConfig config;
    if (!GetConfig(device_id, config)) {
        return -1;
    }
    if (config.remote_port == -1) {
        LOG_ERROR("Failed to find remote port for Device Identifier: {}", device_id);
    }
    LOG_DEBUG("Got Remote Port {} for Device Identifier {}", config.remote_port, device_id);
    return config.remote_port;
}

bool BratislavaSocketRegistry::AddLocalPort(const std::string& device_id) {
    std::unique_lock lock(mutex_shared_registry_);
    const int        port = AcquireLocalPort();
    if (port != -1) {
        BratislavaSocketConfig config;
        if (!GetConfig(device_id, config)) {
            return false;
        }
        config.local_port = port;
        return UpdateConfig(config);
    } else {
        LOG_ERROR("Failed to acquire local port for Bratislava Socket {}", device_id);
    }
    return false;
}

int BratislavaSocketRegistry::GetLocalPort(const std::string& device_id) {
    std::shared_lock       lock(mutex_shared_registry_);
    BratislavaSocketConfig config;
    if (!GetConfig(device_id, config)) {
        return -1;
    }
    if (config.local_port == -1) {
        LOG_ERROR("Failed to find local port for Device Identifier: {}", device_id);
    }
    LOG_DEBUG("Got Local Port {} for Device Identifier {}", config.local_port, device_id);
    return config.local_port;
}

void BratislavaSocketRegistry::InitializeLocalPorts() {
    std::unique_lock lock(mutex_ports_);
    LOG_DEBUG("Initializing Ports...");
    for (int port = MIN_PORT; port <= MAX_PORT; ++port) {
        available_local_ports_.insert(port);
    }
}

int BratislavaSocketRegistry::AcquireLocalPort() {
    std::unique_lock lock(mutex_ports_);
    if (available_local_ports_.empty()) {
        return -1; // No ports available
    }
    const int port = *available_local_ports_.begin();
    available_local_ports_.erase(available_local_ports_.begin());

    LOG_DEBUG("Acquired Local Port {}", port);

    return port;
}

bool BratislavaSocketRegistry::ReleaseLocalPort(int port) {
    std::unique_lock lock(mutex_ports_);
    if (port >= MIN_PORT && port <= MAX_PORT) {
        available_local_ports_.insert(port);
    } else {
        return false;
    }
    LOG_DEBUG("Released Local Port {}", port);
    return true;
}

bool BratislavaSocketRegistry::ReleaseLocalPort(const std::string& device_id) {
    std::unique_lock       lock(mutex_shared_registry_);
    BratislavaSocketConfig config;
    if (!GetConfig(device_id, config)) {
        return false;
    }
    if (config.local_port == -1) {
        LOG_DEBUG("Attempted to release a local port that was never allocated.");
        return false;
    }
    return ReleaseLocalPort(config.local_port);
}

bool BratislavaSocketRegistry::ReleaseLocalPort(BratislavaSocket* socket) {
    std::unique_lock       lock(mutex_shared_registry_);
    BratislavaSocketConfig config;
    if (!GetConfig(socket, config)) {
        return false;
    }
    if (config.local_port == -1) {
        LOG_DEBUG("Attempted to release a local port that was never allocated.");
        return false;
    }
    return ReleaseLocalPort(config.local_port);
}

std::string BratislavaSocketRegistry::GetDeviceID(BratislavaSocket* socket) {
    std::string device_id;
    switch (socket->link.instrumentType) {
    case INSTRUMENT_BLUETOOTH: {
        device_id = std::string(socket->link.bluetoothDeviceInfo.mac_address);
        break;
    }
    case INSTRUMENT_WIFI: {
        device_id = std::string(socket->link.wifiDeviceInfo.mac_address);
        break;
    }
    default:
        break;
    }
    return device_id;
}

int BratislavaSocketRegistry::AddLink(BratislavaLink*& link) {
    std::unique_lock lock(mutex_shared_registry_);
    std::string      id(link->linkID);
    if (auto search = active_links_.find(id); search != active_links_.end()) {
        if (active_links_[id] != link) {
            // Device id already has a link in active list
            delete link;
            link = active_links_[id];
        }

        // Link exists and is in active list
        // Do nothing
        return 0;
    }

    if (auto search = inactive_links_.find(id); search != inactive_links_.end()) {
        if (inactive_links_[id] != link) {
            // Device id already has a link in inactive list
            delete link;
            link = inactive_links_[id];
        }

        active_links_[id] = inactive_links_[id];
        inactive_links_.erase(search);

        return 0;
    }

    // Link is new, add to list
    active_links_[id] = link;
    return 0;
}

int BratislavaSocketRegistry::RemoveLink(std::string& link_id) {
    std::unique_lock lock(mutex_shared_registry_);
    if (auto search = active_links_.find(link_id); search != active_links_.end()) {
        inactive_links_[link_id] = active_links_[link_id];
        active_links_.erase(search);
    }
    return 0;
}

int BratislavaSocketRegistry::RemoveLink(BratislavaLink* link) {
    std::string id(link->linkID);
    return RemoveLink(id);
}

int BratislavaSocketRegistry::GetLinks(std::vector<BratislavaLink*>& links) {
    std::shared_lock lock(mutex_shared_registry_);
    for (auto it = active_links_.begin(); it != active_links_.end(); ++it) {
        links.push_back(it->second);
    }
    return 0;
}

BratislavaLink* BratislavaSocketRegistry::GetLink(std::string& link_id) {
    std::shared_lock lock(mutex_shared_registry_);
    if (inactive_links_.find(link_id) != inactive_links_.end()) {
        return inactive_links_[link_id];
    }

    if (active_links_.find(link_id) != active_links_.end()) {
        return active_links_[link_id];
    }

    return nullptr;
}

void BratislavaSocketRegistry::Cleanup() {
    std::lock_guard<std::recursive_mutex> lock(mutex_recursive_);
    while (sockets_.size() != 0) {
        BratislavaSocket* socket = sockets_.back();
        bratislavaDestroy(socket);
    }
}
