#include "BratislavaSocket.h"

#include "BratislavaSocketImpl.h"

#include <BratislavaSocketRegistry.h>
#include <adapters/BaseAdapter.h>

#ifdef SIM_BUILD
#include "adapters/simulation/TcpSimulationAdapter.h"

#include <adapters/simulation/SimulationAdapter.h>

#else
#include <adapters/bluetooth/BluetoothAdapter.h>
#include <adapters/wifi/WifiAdapter.h>

#endif

#include <anduril/util/CharArrayUtils.h>
#include <anduril/util/Logger.h>

#include <optional>
#include <shared_mutex>

// Global initializer, evaluated once at load; we never call setenv, so this is not a real race.
const char* g_brat_socket_protocol = std::getenv("BRAT_SOCKET_PROTOCOL"); // NOLINT(concurrency-mt-unsafe)
const bool  g_use_tcp              = (g_brat_socket_protocol && std::strcmp(g_brat_socket_protocol, "TCP") == 0);

// The global bsock mutexes guard the registry (is this bsock pointer still valid?).
// The per-socket mutexes guard a bsock's internal state.
// Do not hold the global mutexes during socket I/O.
//
// The pin* helpers atomically verify the bsock is registered and hand off to the
// per-socket lock; they return nullopt if the socket has already been destroyed.

std::mutex        g_bsock_creation_mutex; // serializes registry mutations
std::shared_mutex g_bsock_deletion_mutex; // readers pin bsock; destroy is exclusive

static std::optional<std::shared_lock<std::shared_mutex>> pinSharedIfExists(BratislavaSocket* bsock) {
    std::shared_lock<std::shared_mutex> global_lock(g_bsock_deletion_mutex);
    if (!BratislavaSocketRegistry::GetInstance().Exists(bsock)) {
        errno = EIDRM;
        return std::nullopt;
    }
    return std::shared_lock<std::shared_mutex>(bsock->mutex_shared_bsock_);
}

static std::optional<std::unique_lock<std::shared_mutex>> pinUniqueIfExists(BratislavaSocket* bsock) {
    std::shared_lock<std::shared_mutex> global_lock(g_bsock_deletion_mutex);
    if (!BratislavaSocketRegistry::GetInstance().Exists(bsock)) {
        errno = EIDRM;
        return std::nullopt;
    }
    return std::unique_lock<std::shared_mutex>(bsock->mutex_shared_bsock_);
}

void bratislavaSocketInit() {
#ifdef SIM_BUILD
    if (g_use_tcp) {
        // Initialize the SocketClientManager singleton thread to listen for
        // connections. Local variable doesn't matter and gets dropped
        std::shared_ptr<SocketClientManager> listener = SocketClientManager::Instance();
    }
#endif
}

extern "C" BratislavaSocket* bratislavaSocket(BratislavaLink link) {
    std::string            ID(link.linkID);
    BratislavaSocketConfig blink_config;

    // Lock the global mutex to ensure thread-safe socket creation
    std::shared_lock<std::shared_mutex> global_lock_deletion(g_bsock_deletion_mutex);
    std::unique_lock<std::mutex>        global_lock_creation(g_bsock_creation_mutex);

    if (BratislavaSocketRegistry::GetInstance().Exists(ID)) {
        if (BratislavaSocketRegistry::GetInstance().GetConfig(ID, blink_config)) {
            BratislavaSocket* bsock = blink_config.socket;
            if (bsock) {
                return bsock;
            }
        }
        LOG_ERROR("bratislavaSocket registered for this link but doesn't exist");
        return nullptr;
    }

    auto* bratislavaSocket = new BratislavaSocket;

    // Initialize private members
    bratislavaSocket->link = link;

#ifdef SIM_BUILD
    if (g_use_tcp) {
        std::string remoteIpAddress;
        if (!BratislavaSocketRegistry::GetInstance().GetIP(bratislavaSocket->link.linkID, remoteIpAddress)) {
            LOG_ERROR("Failed to create socket, because no remote address has been assigned to device {}",
                      bratislavaSocket->link.linkID);
            return nullptr;
        }

        std::stringstream remoteEndpoint;
        remoteEndpoint << remoteIpAddress << ":" << 40000;
        bratislavaSocket->adapter =
            new TcpSimulationAdapter(remoteEndpoint.str(), SocketClientManager::Instance(), bratislavaSocket);
    } else {
        bratislavaSocket->adapter = new SimulationAdapter(bratislavaSocket);
    }

#else // DEVICE_BUILD
    switch (bratislavaSocket->link.instrumentType) {
    case INSTRUMENT_BLUETOOTH:
        bratislavaSocket->adapter = new BluetoothAdapter(bratislavaSocket);
        break;
    case INSTRUMENT_WIFI:
        bratislavaSocket->adapter = new WifiAdapter(bratislavaSocket);
        break;
    default:
        LOG_ERROR("bratislavaSocket does not support the provided instrument type.");
        return nullptr;
    }
#endif

    BratislavaSocketRegistry::GetInstance().Add(bratislavaSocket);
    return bratislavaSocket;
}

extern "C" BratislavaLinkInfo bratislavaGetLinkInfo(const BratislavaLink* blink) {
    BratislavaLinkInfo linkInfo;
    if (!blink) {
        LOG_ERROR("Null link passed to bratislavaGetLinkInfo");
        errno = EINVAL;
        return linkInfo;
    }

    anduril::util::copyFixedStr(linkInfo.linkID, blink->linkID);
    anduril::util::copyFixedStr(linkInfo.devID, blink->devID);

    linkInfo.linkType = blink->linkType;

    linkInfo.latency     = blink->latency;
    linkInfo.throughput  = blink->throughput;
    linkInfo.messageLoss = blink->messageLoss;

    return linkInfo;
}

extern "C" int bratislavaConn(BratislavaSocket* bsock) {
    if (!bsock) {
        LOG_ERROR("Null socket passed to bratislavaConn");
        errno = EINVAL;
        return -1;
    }

    auto lock = pinUniqueIfExists(bsock);
    if (!lock) {
        return -1;
    }

    if (BratislavaSocketRegistry::GetInstance().IsConnected(bsock)) {
        return 0;
    }
    return bsock->adapter->conn();
}

extern "C" int bratislavaSend(BratislavaSocket* bsock, const void* buf, size_t len) {
    if (!bsock) {
        LOG_ERROR("Null socket passed to bratislavaSend");
        errno = EINVAL;
        return -1;
    }

    auto lock = pinSharedIfExists(bsock);
    if (!lock) {
        return -1;
    }

    if (!BratislavaSocketRegistry::GetInstance().IsConnected(bsock)) {
        LOG_ERROR("Bratislava Socket is no longer connected");
        errno = EIDRM;
        return -1;
    }

    auto* adapter = bsock->adapter;
    if (!adapter) {
        LOG_ERROR("Bratislava send adapter is not initialized. This socket cannot be used.");
        errno = EIDRM;
        return -1;
    }
    int status = adapter->send(buf, len);
    return status;
}

extern "C" int bratislavaLinkSend(const BratislavaLink* blink, const void* buf, size_t len) {
    BratislavaSocket* bsock = bratislavaSocket(*blink);
    if (!bsock) {
        LOG_ERROR("Link without associated channel");
        errno = EINVAL;
        return -1;
    }

    if (0 != bratislavaConn(bsock)) {
        LOG_ERROR("Link is no longer connected");
        errno = EINVAL;
        return -1;
    }

    auto lock = pinSharedIfExists(bsock);
    if (!lock) {
        return -1;
    }

    auto* adapter = bsock->adapter;
    if (!adapter) {
        LOG_ERROR("Bratislava send adapter is not initialized. This link cannot be used.");
        errno = EIDRM;
        return -1;
    }
    int status = adapter->send(buf, len);
    return status;
}

extern "C" int bratislavaRecv(BratislavaSocket* bsock, void* buf, size_t len) {
    if (!bsock) {
        LOG_ERROR("Null socket passed to bratislavaRecv");
        errno = EINVAL;
        return -1;
    }

    auto lock = pinSharedIfExists(bsock);
    if (!lock) {
        return -1;
    }

    if (!BratislavaSocketRegistry::GetInstance().IsConnected(bsock)) {
        errno = EIDRM;
        return -1;
    }

    auto* adapter = bsock->adapter;
    if (!adapter) {
        LOG_ERROR("Bratislava recv adapter is not initialized. This socket cannot be used.");
        errno = EIDRM;
        return -1;
    }
    int status = adapter->recv(buf, len);
    return status;
}

extern "C" int bratislavaLinkRecv(const BratislavaLink* blink, void* buf, size_t len) {
    BratislavaSocket* bsock = bratislavaSocket(*blink);
    if (!bsock) {
        LOG_ERROR("Link without associated channel");
        errno = EINVAL;
        return -1;
    }

    if (0 != bratislavaConn(bsock)) {
        LOG_ERROR("Link is no longer connected");
        errno = EINVAL;
        return -1;
    }

    auto lock = pinSharedIfExists(bsock);
    if (!lock) {
        return -1;
    }

    auto* adapter = bsock->adapter;
    if (!adapter) {
        LOG_ERROR("Bratislava recv adapter is not initialized. This link cannot be used.");
        errno = EIDRM;
        return -1;
    }
    int status = adapter->recv(buf, len);
    return status;
}

extern "C" int bratislavaConnTimeout(BratislavaSocket* bsock, unsigned int milliseconds) {
    if (!bsock) {
        LOG_ERROR("bratislavaConnTimeout invoked with a null socket");
        errno = EINVAL;
        return -1;
    }

    auto lock = pinSharedIfExists(bsock);
    if (!lock) {
        return -1;
    }

    return bsock->adapter->setConnTimeout(milliseconds);
}

extern "C" int bratislavaRecvTimeout(BratislavaSocket* bsock, unsigned int milliseconds) {
    if (!bsock) {
        LOG_ERROR("bratislavaRecvTimeout invoked with a null socket");
        errno = EINVAL;
        return -1;
    }

    auto lock = pinSharedIfExists(bsock);
    if (!lock) {
        return -1;
    }

    return bsock->adapter->setRecvTimeout(milliseconds);
}

extern "C" void bratislavaDestroy(BratislavaSocket* bsock) {
    if (!bsock) {
        LOG_ERROR("bratislavaDestroy invoked with a null socket");
        errno = EINVAL;
        return;
    }

    // Wake any blocked reads/writes on this socket so they unwind and release the
    // per-socket lock below. interrupt() only shuts the socket down (it does NOT close
    // the fd): closing here, concurrently with an in-flight recvfrom()/sendto() under a
    // shared lock, is a use-after-close race on the kernel fd. The fd is closed later by
    // the adapter destructor, under the exclusive per-socket lock, once I/O has drained.
    {
        std::shared_lock<std::shared_mutex> global_lock_deletion(g_bsock_deletion_mutex);
        if (!BratislavaSocketRegistry::GetInstance().Exists(bsock)) {
            errno = EIDRM;
            return;
        }
        bsock->adapter->interrupt();
    }

    // Lock order matches pin* and bratislavaSocket() so the orderings cannot cycle.
    // The per-socket lock drains in-flight I/O on this bsock before deletion. The global
    // locks then protect the registry mutation.
    std::unique_lock<std::shared_mutex> global_lock_deletion(g_bsock_deletion_mutex);
    std::unique_lock<std::shared_mutex> per_socket_lock(bsock->mutex_shared_bsock_);
    std::unique_lock<std::mutex>        global_lock_creation(g_bsock_creation_mutex);
    if (!BratislavaSocketRegistry::GetInstance().Exists(bsock)) {
        errno = EIDRM;
        return;
    }

    if (BratislavaSocketRegistry::GetInstance().Remove(bsock)) {
        delete bsock->adapter;
        bsock->adapter = (BaseAdapter*)0xFFFFFFFFFFFFFFFF;

        // Release before delete; bsock's mutex would otherwise unlock a freed object.
        per_socket_lock.unlock();
        delete bsock;
    } else {
        LOG_ERROR("Failed to destroy Bratislava socket. Failed to remove socket from registry");
        errno = EIDRM;
        return;
    }
}

extern "C" void bratislavaDisconnect(BratislavaSocket* bsock) {
    if (!bsock) {
        LOG_ERROR("bratislavaDisconnect invoked with a null socket");
        errno = EINVAL;
        return;
    }

    // disconn() no longer closes the fd -- it only shutdown()s it to wake blocked I/O,
    // which is safe concurrently with an in-flight recvfrom()/sendto(). So the shared
    // lock suffices, and a disconnect can no longer be starved waiting for a blocked
    // read to time out. The deferred close()/recreate happens in conn() under the
    // exclusive lock.
    auto lock = pinSharedIfExists(bsock);
    if (!lock) {
        return;
    }

    if (BratislavaSocketRegistry::GetInstance().Disconnect(bsock)) {
        bsock->adapter->disconn();
    } else {
        LOG_ERROR("Failed to disconnect Bratislava socket.");
        errno = EIDRM;
        return;
    }
}
