#ifndef BRATISLAVA_SOCKET_REGISTRY_H
#define BRATISLAVA_SOCKET_REGISTRY_H

#include "BratislavaSocket.h"
#include "BratislavaSocketImpl.h"

#include <atomic>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

/**
 * @brief Internal metadata containing information about sockets and links
 * that is not exposed to the user.
 */
struct BratislavaSocketConfig {
    BratislavaSocket* socket;    // May be nullptr
    std::string       device_id; // e.g. a MAC address
    std::string       ip;        // virtual machine IP address (in simulation mode)
    int               sockfd      = -1;
    int               local_port  = -1;
    int               remote_port = -1;
    bool              connected   = false;

    BratislavaSocketConfig() = default;
};

/**
 * @brief Registry for bridging the Sensing API and Communications API.
 *
 * Stores BratislavaSocketConfig objects and provides methods to
 * query and modify them.
 *
 * Additionally manages tracking active/inactive links, and managing
 * local port allocations.
 *
 * The Sensing API uses the methods that take a device identifier
 * (e.g. a MAC address) as an argument.
 *
 * The Communications API uses the methods that take a BratislavaSocket
 * pointer as an argument.
 */
class BratislavaSocketRegistry {
public:
    static BratislavaSocketRegistry& GetInstance();
    void                             Add(BratislavaSocket* socket);
    bool                             Remove(BratislavaSocket* socket);
    bool                             Exists(BratislavaSocket* socket);
    bool                             Exists(const std::string& device_id);

    void CreateConfig(BratislavaSocket* socket);
    void CreateConfig(const std::string& device_id);
    void RemoveConfig(BratislavaSocket* socket);
    void RemoveUnusedConfig(const std::string& device_id);
    bool GetConfig(BratislavaSocket* socket, BratislavaSocketConfig& config);
    bool GetConfig(const std::string& device_id, BratislavaSocketConfig& config);
    bool UpdateConfig(const BratislavaSocketConfig& config);

    bool Connect(BratislavaSocket* socket);
    bool Disconnect(const std::string& device_id);
    bool Disconnect(BratislavaSocket* socket);
    bool IsConnected(const std::string& device_id);
    bool IsConnected(BratislavaSocket* socket);

    bool AddIP(const std::string& device_id, const std::string& device_ip);
    bool GetIP(const std::string& device_id, std::string& device_ip);
    bool AddRemotePort(const std::string& device_id, const int& port);
    int  GetRemotePort(const std::string& device_id);
    bool AddLocalPort(const std::string& device_id);
    int  GetLocalPort(const std::string& device_id);
    void InitializeLocalPorts();
    int  AcquireLocalPort();
    bool ReleaseLocalPort(int port);
    bool ReleaseLocalPort(const std::string& device_id);
    bool ReleaseLocalPort(BratislavaSocket* socket);

    std::string GetDeviceID(BratislavaSocket* socket);

    int             AddLink(BratislavaLink*& link);
    int             RemoveLink(std::string& link_id);
    int             RemoveLink(BratislavaLink* link);
    int             GetLinks(std::vector<BratislavaLink*>& links);
    BratislavaLink* GetLink(std::string& link_id);

    void Cleanup();

    BratislavaSocketRegistry();
    ~BratislavaSocketRegistry();

private:
    // Lock-free core of UpdateConfig.
    // PRECONDITION: caller must already hold a unique_lock on mutex_shared_config_.
    bool UpdateConfigLocked(const BratislavaSocketConfig& config);

    std::vector<BratislavaSocket*>                                 sockets_;
    std::unordered_map<std::string, BratislavaLink*>               active_links_;
    std::unordered_map<std::string, BratislavaLink*>               inactive_links_;
    std::unordered_map<std::string, BratislavaSocketConfig*>       device_id_to_config_;
    std::unordered_map<BratislavaSocket*, BratislavaSocketConfig*> sock_to_config_;
    std::set<int>                                                  available_local_ports_;
    std::shared_mutex                                              mutex_ports_;
    mutable std::recursive_mutex                                   mutex_recursive_;
    mutable std::shared_mutex                                      mutex_shared_registry_;
    mutable std::shared_mutex                                      mutex_shared_config_;

    static constexpr int MIN_PORT = 40000;
    static constexpr int MAX_PORT = 40256;

    // Non-copyable and non-movable
    BratislavaSocketRegistry(const BratislavaSocketRegistry&)            = delete;
    BratislavaSocketRegistry& operator=(const BratislavaSocketRegistry&) = delete;
};

#endif // BRATISLAVA_SOCKET_REGISTRY_H
