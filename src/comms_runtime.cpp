#include "comms_runtime.h"

#include "BratislavaSocket.h"
#include "bluetooth_scanner.h"
#include "comms_typedef.h"

#include <arpa/inet.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <linux/wireless.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdarg>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <list>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

constexpr uint16_t kUdpPort = 37020;
constexpr uint16_t kBluetoothPsm = 0x1001;
constexpr int kServerPollTimeoutMs = 500;
constexpr int kClientTimeoutMs = 3000;
constexpr int kBluetoothScanSeconds = 5;
constexpr int kStartupCommandTimeoutMs = 250;
constexpr auto kDiscoveryInterval = std::chrono::seconds(5);
constexpr const char* kHelloPayload = "Hello Pi World";
constexpr const char* kReplyPrefix = "Hello Back from ";

bool comms_debug_enabled() {
    static const bool enabled = []() {
        const char* const value = std::getenv("SENSOR_LIB_COMMS_DEBUG");
        return (value != nullptr) && (value[0] != '\0') && (std::strcmp(value, "0") != 0);
    }();
    return enabled;
}

bool prefer_bluetooth_first() {
    static const bool enabled = []() {
        const char* const value = std::getenv("SENSOR_LIB_COMMS_PREFER_BLUETOOTH");
        return (value != nullptr) && (value[0] != '\0') && (std::strcmp(value, "0") != 0);
    }();
    return enabled;
}

bool skip_wifi_comms_scan() {
    static const bool enabled = []() {
        const char* const value = std::getenv("SENSOR_LIB_COMMS_SKIP_WIFI");
        return (value != nullptr) && (value[0] != '\0') && (std::strcmp(value, "0") != 0);
    }();
    return enabled;
}

bool skip_bluetooth_comms_scan() {
    static const bool enabled = []() {
        const char* const value = std::getenv("SENSOR_LIB_COMMS_SKIP_BLUETOOTH");
        return (value != nullptr) && (value[0] != '\0') && (std::strcmp(value, "0") != 0);
    }();
    return enabled;
}

void comms_debug(const char* const format, ...) {
    if (!comms_debug_enabled()) {
        return;
    }

    std::fprintf(stderr, "[comms-debug] ");
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

bool run_command_with_timeout(const char* command, int timeout_ms) {
    if ((command == nullptr) || (timeout_ms <= 0)) {
        return false;
    }

    const pid_t child_pid = fork();
    if (child_pid < 0) {
        return false;
    }

    if (child_pid == 0) {
        execl("/bin/sh", "sh", "-c", command, (char*)nullptr);
        _exit(127);
    }

    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t wait_rc = waitpid(child_pid, &status, WNOHANG);
        if (wait_rc == child_pid) {
            return WIFEXITED(status) && (WEXITSTATUS(status) == 0);
        }
        if ((wait_rc < 0) && (errno != EINTR)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    (void)kill(child_pid, SIGTERM);
    for (int i = 0; i < 20; ++i) {
        const pid_t wait_rc = waitpid(child_pid, &status, WNOHANG);
        if (wait_rc == child_pid) {
            return false;
        }
        if ((wait_rc < 0) && (errno != EINTR)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    (void)kill(child_pid, SIGKILL);
    while ((waitpid(child_pid, &status, 0) < 0) && (errno == EINTR)) {
    }
    return false;
}

struct WifiCandidate {
    WifiDeviceInfoBase device{};
    std::string interface_name;
    std::string ip_address;
};

struct ActiveLink {
    BratislavaLink link{};
    std::string key;
};

struct CallbackState {
    BluetoothDeviceDiscoveredCallback bluetooth_device = nullptr;
    WifiDeviceDiscoveredCallback wifi_device = nullptr;
    LinkDiscoveredCallback link_discovered = nullptr;
    LinkDroppedCallback link_dropped = nullptr;
};

std::string trim_copy(const std::string& value) {
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1U);
}

std::string normalize_mac(const std::string& value) {
    std::string output;
    output.reserve(value.size());
    for (unsigned char ch : value) {
        output.push_back(static_cast<char>(std::toupper(ch)));
    }
    return output;
}

void copy_string(char* dst, size_t dst_size, const std::string& value) {
    if ((dst == nullptr) || (dst_size == 0U)) {
        return;
    }
    std::snprintf(dst, dst_size, "%s", value.c_str());
}

bool path_exists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::exists(path, error);
}

bool interface_is_wireless(const std::string& interface_name) {
    return path_exists(std::filesystem::path("/sys/class/net") / interface_name / "wireless");
}

bool interface_is_usable(const std::string& interface_name) {
    if (interface_name == "lo") {
        return false;
    }

    std::ifstream input(std::filesystem::path("/sys/class/net") / interface_name / "operstate");
    std::string operstate;
    if (input && std::getline(input, operstate)) {
        operstate = trim_copy(operstate);
        if (operstate == "down") {
            return false;
        }
    }

    return true;
}

std::vector<std::string> list_wireless_interfaces() {
    std::vector<std::string> interfaces;
    for (const auto& entry : std::filesystem::directory_iterator("/sys/class/net")) {
        const std::string interface_name = entry.path().filename().string();
        if (!interface_is_usable(interface_name) || !interface_is_wireless(interface_name)) {
            continue;
        }
        interfaces.push_back(interface_name);
    }
    std::sort(interfaces.begin(), interfaces.end());
    return interfaces;
}

std::string local_interface_mac(const std::string& interface_name) {
    std::ifstream input(std::filesystem::path("/sys/class/net") / interface_name / "address");
    std::string mac_address;
    if (!input || !std::getline(input, mac_address)) {
        return {};
    }
    return normalize_mac(trim_copy(mac_address));
}

std::string arp_mac_for_ip(const std::string& interface_name, const std::string& ip_address) {
    if (interface_name.empty() || ip_address.empty()) {
        return {};
    }

    std::ifstream input("/proc/net/arp");
    if (!input) {
        return {};
    }

    std::string line;
    std::getline(input, line);
    while (std::getline(input, line)) {
        std::istringstream parser(line);
        std::string arp_ip_address;
        std::string hw_type;
        std::string flags;
        std::string mac_address;
        std::string mask;
        std::string arp_interface_name;
        if (!(parser >> arp_ip_address >> hw_type >> flags >> mac_address >> mask >> arp_interface_name)) {
            continue;
        }
        if ((arp_ip_address != ip_address) || (arp_interface_name != interface_name)) {
            continue;
        }
        mac_address = normalize_mac(mac_address);
        if (mac_address == "00:00:00:00:00:00") {
            return {};
        }
        return mac_address;
    }

    return {};
}

std::string interface_name_from_index(unsigned int interface_index) {
    if (interface_index == 0U) {
        return {};
    }

    char name[IF_NAMESIZE] = {0};
    if (if_indextoname(interface_index, name) == nullptr) {
        return {};
    }
    return name;
}

std::optional<in_addr> local_interface_ipv4(const std::string& interface_name) {
    if (interface_name.empty()) {
        return std::nullopt;
    }

    ifaddrs* addresses = nullptr;
    if (getifaddrs(&addresses) != 0) {
        return std::nullopt;
    }

    std::optional<in_addr> result;
    for (ifaddrs* entry = addresses; entry != nullptr; entry = entry->ifa_next) {
        if ((entry->ifa_name == nullptr) || (entry->ifa_addr == nullptr)) {
            continue;
        }
        if (interface_name != entry->ifa_name) {
            continue;
        }
        if (entry->ifa_addr->sa_family != AF_INET) {
            continue;
        }

        result = reinterpret_cast<sockaddr_in*>(entry->ifa_addr)->sin_addr;
        break;
    }

    freeifaddrs(addresses);
    return result;
}

void set_receive_timeout(int socket_fd, int timeout_ms) {
    timeval timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

bool starts_with_reply_prefix(const std::string& payload) {
    return payload.rfind(kReplyPrefix, 0) == 0;
}

std::string make_link_id(const std::string& prefix, const std::string& mac_address) {
    std::string compact;
    compact.reserve(mac_address.size());
    for (char ch : mac_address) {
        if (ch != ':') {
            compact.push_back(ch);
        }
    }

    std::string link_id = prefix + compact;
    if (link_id.size() > 19U) {
        link_id.resize(19U);
    }
    return link_id;
}

std::vector<WifiCandidate> discover_wifi_candidates() {
    std::vector<WifiCandidate> candidates;
    const auto wireless_interfaces = list_wireless_interfaces();
    if (wireless_interfaces.empty()) {
        return candidates;
    }

    const std::unordered_set<std::string> interface_set(wireless_interfaces.begin(), wireless_interfaces.end());
    std::ifstream input("/proc/net/arp");
    if (!input) {
        return candidates;
    }

    std::string line;
    std::getline(input, line);
    std::unordered_set<std::string> seen;

    while (std::getline(input, line)) {
        std::istringstream parser(line);
        std::string ip_address;
        std::string hw_type;
        std::string flags;
        std::string mac_address;
        std::string mask;
        std::string interface_name;
        if (!(parser >> ip_address >> hw_type >> flags >> mac_address >> mask >> interface_name)) {
            continue;
        }
        mac_address = normalize_mac(mac_address);
        if ((mac_address == "00:00:00:00:00:00") || (interface_set.find(interface_name) == interface_set.end())) {
            continue;
        }

        const std::string key = interface_name + "|" + mac_address;
        if (!seen.insert(key).second) {
            continue;
        }

        WifiCandidate candidate;
        candidate.interface_name = interface_name;
        candidate.ip_address = ip_address;
        candidate.device.type = WIFI_ADAPTER;
        candidate.device.rssi.valid = false;
        copy_string(candidate.device.mac_address, sizeof(candidate.device.mac_address), mac_address);
        copy_string(candidate.device.ssid, sizeof(candidate.device.ssid), ip_address);
        comms_debug("wifi candidate: if=%s ip=%s mac=%s",
                    candidate.interface_name.c_str(),
                    candidate.ip_address.c_str(),
                    candidate.device.mac_address);
        candidates.push_back(candidate);
    }

    std::sort(candidates.begin(), candidates.end(), [](const WifiCandidate& left, const WifiCandidate& right) {
        if (left.interface_name != right.interface_name) {
            return left.interface_name < right.interface_name;
        }
        return std::strcmp(left.device.mac_address, right.device.mac_address) < 0;
    });
    return candidates;
}

class CommsRuntime {
  public:
    explicit CommsRuntime(const InstrumentAPIConfig& config) : config_(config) {}

    instrument_api_status_t start() {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) {
            return INSTRUMENT_API_SUCCESS;
        }

        try {
            configure_bluetooth_accept_incoming_connections();
            udp_server_thread_ = std::thread([this]() { udp_server_loop(); });
            bluetooth_server_thread_ = std::thread([this]() { bluetooth_server_loop(); });
            discovery_thread_ = std::thread([this]() { discovery_loop(); });
        } catch (...) {
            running_ = false;
            stop();
            return INSTRUMENT_API_ERROR;
        }
        return INSTRUMENT_API_SUCCESS;
    }

    void stop() {
        running_.exchange(false);
        discovery_cv_.notify_all();

        if (udp_server_thread_.joinable()) {
            udp_server_thread_.join();
        }
        if (bluetooth_server_thread_.joinable()) {
            bluetooth_server_thread_.join();
        }
        if (discovery_thread_.joinable()) {
            discovery_thread_.join();
        }

        std::vector<BratislavaLinkInfo> dropped;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const ActiveLink& link : active_links_) {
                BratislavaLinkInfo info{};
                copy_string(info.linkID, sizeof(info.linkID), link.link.linkID);
                copy_string(info.devID, sizeof(info.devID), link.link.devID);
                info.linkType = link.link.linkType;
                dropped.push_back(info);
            }
            active_links_.clear();
            active_link_keys_.clear();
        }

        LinkDroppedCallback drop_callback = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            drop_callback = callbacks_.link_dropped;
        }
        if (drop_callback != nullptr) {
            for (const BratislavaLinkInfo& info : dropped) {
                drop_callback(&info);
            }
        }
    }

    instrument_api_status_t register_callback(InstrumentType instrument_type,
                                              CallbackType callback_type,
                                              InstrumentInputType input_data) {
        if (input_data == nullptr) {
            return INSTRUMENT_API_ERROR;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        switch (instrument_type) {
        case INSTRUMENT_BLUETOOTH:
            if (callback_type != DEVICE_DISCOVERED) {
                return INSTRUMENT_API_NOT_SUPPORTED;
            }
            callbacks_.bluetooth_device = *static_cast<BluetoothDeviceDiscoveredCallback*>(input_data);
            return INSTRUMENT_API_SUCCESS;
        case INSTRUMENT_WIFI:
            if (callback_type != DEVICE_DISCOVERED) {
                return INSTRUMENT_API_NOT_SUPPORTED;
            }
            callbacks_.wifi_device = *static_cast<WifiDeviceDiscoveredCallback*>(input_data);
            return INSTRUMENT_API_SUCCESS;
        case INSTRUMENT_COMMS:
            if (callback_type == LINK_DISCOVERED) {
                callbacks_.link_discovered = *static_cast<LinkDiscoveredCallback*>(input_data);
                return INSTRUMENT_API_SUCCESS;
            }
            if (callback_type == LINK_DROPPED) {
                callbacks_.link_dropped = *static_cast<LinkDroppedCallback*>(input_data);
                return INSTRUMENT_API_SUCCESS;
            }
            return INSTRUMENT_API_NOT_SUPPORTED;
        default:
            return INSTRUMENT_API_NOT_SUPPORTED;
        }
    }

    instrument_api_status_t unregister_callback(InstrumentType instrument_type,
                                                CallbackType callback_type,
                                                InstrumentInputType input_data) {
        (void)input_data;
        std::lock_guard<std::mutex> lock(mutex_);
        switch (instrument_type) {
        case INSTRUMENT_BLUETOOTH:
            if (callback_type != DEVICE_DISCOVERED) {
                return INSTRUMENT_API_NOT_SUPPORTED;
            }
            callbacks_.bluetooth_device = nullptr;
            return INSTRUMENT_API_SUCCESS;
        case INSTRUMENT_WIFI:
            if (callback_type != DEVICE_DISCOVERED) {
                return INSTRUMENT_API_NOT_SUPPORTED;
            }
            callbacks_.wifi_device = nullptr;
            return INSTRUMENT_API_SUCCESS;
        case INSTRUMENT_COMMS:
            if (callback_type == LINK_DISCOVERED) {
                callbacks_.link_discovered = nullptr;
                return INSTRUMENT_API_SUCCESS;
            }
            if (callback_type == LINK_DROPPED) {
                callbacks_.link_dropped = nullptr;
                return INSTRUMENT_API_SUCCESS;
            }
            return INSTRUMENT_API_NOT_SUPPORTED;
        default:
            return INSTRUMENT_API_NOT_SUPPORTED;
        }
    }

    instrument_api_status_t discover_links(InstrumentType instrument_type,
                                           InstrumentOutputType output_data,
                                           uint32_t* output_len) {
        if ((output_data == nullptr) || (output_len == nullptr)) {
            return INSTRUMENT_API_ERROR;
        }

        auto** output_links = static_cast<BratislavaLink**>(output_data);
        uint32_t count = 0U;
        std::lock_guard<std::mutex> lock(mutex_);
        for (ActiveLink& active_link : active_links_) {
            if ((instrument_type == INSTRUMENT_BLUETOOTH) && (active_link.link.instrumentType != INSTRUMENT_BLUETOOTH)) {
                continue;
            }
            if ((instrument_type == INSTRUMENT_WIFI) && (active_link.link.instrumentType != INSTRUMENT_WIFI)) {
                continue;
            }
            if ((instrument_type != INSTRUMENT_COMMS) &&
                (instrument_type != INSTRUMENT_WIFI) &&
                (instrument_type != INSTRUMENT_BLUETOOTH)) {
                return INSTRUMENT_API_NOT_SUPPORTED;
            }
            if (count >= COMMS_MAX_LINKS) {
                break;
            }
            output_links[count++] = &active_link.link;
        }
        *output_len = count;
        return INSTRUMENT_API_SUCCESS;
    }

  private:
    void configure_bluetooth_accept_incoming_connections() {
        (void)run_command_with_timeout("command -v rfkill >/dev/null 2>&1 && rfkill unblock bluetooth >/dev/null 2>&1",
                                       kStartupCommandTimeoutMs);
        (void)run_command_with_timeout("command -v btmgmt >/dev/null 2>&1 && btmgmt power on >/dev/null 2>&1",
                                       kStartupCommandTimeoutMs);
        (void)run_command_with_timeout(
            "command -v btmgmt >/dev/null 2>&1 && btmgmt connectable on >/dev/null 2>&1",
            kStartupCommandTimeoutMs);
        (void)run_command_with_timeout("command -v btmgmt >/dev/null 2>&1 && btmgmt bondable on >/dev/null 2>&1",
                                       kStartupCommandTimeoutMs);
        (void)run_command_with_timeout("command -v btmgmt >/dev/null 2>&1 && btmgmt pairable on >/dev/null 2>&1",
                                       kStartupCommandTimeoutMs);
        (void)run_command_with_timeout("command -v btmgmt >/dev/null 2>&1 && btmgmt ssp on >/dev/null 2>&1",
                                       kStartupCommandTimeoutMs);
        (void)run_command_with_timeout(
            "command -v bluetoothctl >/dev/null 2>&1 && "
            "printf 'power on\\ndiscoverable on\\npairable on\\nquit\\n' | bluetoothctl >/dev/null 2>&1",
            kStartupCommandTimeoutMs);
    }

    void discovery_loop() {
        while (running_) {
            scan_once();
            std::unique_lock<std::mutex> lock(discovery_mutex_);
            discovery_cv_.wait_for(lock, kDiscoveryInterval, [this]() { return !running_.load(); });
        }
    }

    void scan_once() {
        if (prefer_bluetooth_first()) {
            if (!skip_bluetooth_comms_scan()) {
                scan_bluetooth_links();
            }
            if (!skip_wifi_comms_scan()) {
                scan_wifi_links();
            }
            return;
        }

        if (!skip_wifi_comms_scan()) {
            scan_wifi_links();
        }
        if (!skip_bluetooth_comms_scan()) {
            scan_bluetooth_links();
        }
    }

    void scan_bluetooth_links() {
        BluetoothDeviceInfoBase devices[BLUETOOTH_MAX_DEVICES];
        uint32_t device_count = 0U;
        const instrument_api_status_t status =
            bluetooth_discover_devices_backend(nullptr, 0U, devices, &device_count);
        if (status != INSTRUMENT_API_SUCCESS) {
            return;
        }

        for (uint32_t index = 0U; index < device_count; ++index) {
            const BluetoothDeviceInfoBase device = devices[index];
            const std::string key = std::string("bt|") + device.mac_address;

            BluetoothDeviceDiscoveredCallback device_callback = nullptr;
            bool should_attempt = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (seen_bluetooth_devices_.insert(key).second) {
                    device_callback = callbacks_.bluetooth_device;
                }
                should_attempt = active_link_keys_.find(key) == active_link_keys_.end();
            }

            if (device_callback != nullptr) {
                device_callback(&device);
            }
            if (!should_attempt) {
                continue;
            }

            ActiveLink active_link;
            if (attempt_bluetooth_handshake(device, &active_link)) {
                publish_link(std::move(active_link));
            }
        }
    }

    void scan_wifi_links() {
        const auto candidates = discover_wifi_candidates();
        for (const WifiCandidate& candidate : candidates) {
            const std::string key = std::string("wifi|") + candidate.device.mac_address;

            WifiDeviceDiscoveredCallback device_callback = nullptr;
            bool should_attempt = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (seen_wifi_devices_.insert(key).second) {
                    device_callback = callbacks_.wifi_device;
                }
                should_attempt = active_link_keys_.find(key) == active_link_keys_.end();
            }

            if (device_callback != nullptr) {
                device_callback(&candidate.device);
            }
            if (!should_attempt) {
                continue;
            }

            ActiveLink active_link;
            if (attempt_wifi_handshake(candidate, &active_link)) {
                publish_link(std::move(active_link));
            }
        }
    }

    bool attempt_wifi_handshake(const WifiCandidate& candidate, ActiveLink* active_link) {
        comms_debug("attempt wifi handshake start: if=%s ip=%s mac=%s",
                    candidate.interface_name.c_str(),
                    candidate.ip_address.c_str(),
                    candidate.device.mac_address);
        const int socket_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (socket_fd < 0) {
            comms_debug("attempt wifi handshake socket() failed: errno=%d", errno);
            return false;
        }

        set_receive_timeout(socket_fd, kClientTimeoutMs);

        const auto local_ip = local_interface_ipv4(candidate.interface_name);
        if (local_ip.has_value()) {
            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_addr = *local_ip;
            local.sin_port = htons(0);
            if (bind(socket_fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
                comms_debug("attempt wifi handshake bind(%s) failed: errno=%d",
                            candidate.interface_name.c_str(),
                            errno);
                close(socket_fd);
                return false;
            }
            char local_buffer[INET_ADDRSTRLEN] = {0};
            (void)inet_ntop(AF_INET, &local.sin_addr, local_buffer, sizeof(local_buffer));
            comms_debug("attempt wifi handshake bound local ip=%s", local_buffer);
        } else {
            comms_debug("attempt wifi handshake no local IPv4 for if=%s", candidate.interface_name.c_str());
        }

        sockaddr_in remote{};
        remote.sin_family = AF_INET;
        remote.sin_port = htons(kUdpPort);
        if (inet_pton(AF_INET, candidate.ip_address.c_str(), &remote.sin_addr) != 1) {
            comms_debug("attempt wifi handshake inet_pton failed for ip=%s", candidate.ip_address.c_str());
            close(socket_fd);
            return false;
        }

        if (connect(socket_fd, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) != 0) {
            comms_debug("attempt wifi handshake connect(%s:%u) failed: errno=%d",
                        candidate.ip_address.c_str(),
                        (unsigned)kUdpPort,
                        errno);
            close(socket_fd);
            return false;
        }
        comms_debug("attempt wifi handshake connected remote=%s:%u",
                    candidate.ip_address.c_str(),
                    (unsigned)kUdpPort);

        if (send(socket_fd, kHelloPayload, std::strlen(kHelloPayload), 0) < 0) {
            comms_debug("attempt wifi handshake send failed: errno=%d", errno);
            close(socket_fd);
            return false;
        }
        comms_debug("attempt wifi handshake sent hello");

        char buffer[512] = {0};
        const ssize_t received = recv(socket_fd, buffer, sizeof(buffer) - 1, 0);
        if (received <= 0) {
            comms_debug("attempt wifi handshake recv failed/empty: rc=%zd errno=%d", received, errno);
            close(socket_fd);
            return false;
        }
        buffer[received] = '\0';
        comms_debug("attempt wifi handshake received reply='%s'", buffer);

        if (!starts_with_reply_prefix(buffer)) {
            comms_debug("attempt wifi handshake reply prefix mismatch");
            close(socket_fd);
            return false;
        }

        close(socket_fd);

        std::memset(&active_link->link, 0, sizeof(active_link->link));
        active_link->key = std::string("wifi|") + candidate.device.mac_address;
        active_link->link.linkType = LINK_TYPE_BLUE;
        active_link->link.instrumentType = INSTRUMENT_WIFI;
        copy_string(active_link->link.linkID, sizeof(active_link->link.linkID), make_link_id("wifi", candidate.device.mac_address));
        copy_string(active_link->link.devID, sizeof(active_link->link.devID), candidate.device.mac_address);
        active_link->link.wifiDeviceInfo = candidate.device;
        return true;
    }

    bool attempt_bluetooth_handshake(const BluetoothDeviceInfoBase& device, ActiveLink* active_link) {
        const int socket_fd = socket(AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_CLOEXEC, BTPROTO_L2CAP);
        if (socket_fd < 0) {
            return false;
        }

        set_receive_timeout(socket_fd, kClientTimeoutMs);

        sockaddr_l2 remote{};
        remote.l2_family = AF_BLUETOOTH;
        remote.l2_psm = htobs(kBluetoothPsm);
        if (str2ba(device.mac_address, &remote.l2_bdaddr) != 0) {
            close(socket_fd);
            return false;
        }

        if (connect(socket_fd, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) != 0) {
            close(socket_fd);
            return false;
        }

        if (send(socket_fd, kHelloPayload, std::strlen(kHelloPayload), 0) < 0) {
            close(socket_fd);
            return false;
        }

        char buffer[512] = {0};
        const ssize_t received = recv(socket_fd, buffer, sizeof(buffer) - 1, 0);
        if (received <= 0) {
            close(socket_fd);
            return false;
        }
        buffer[received] = '\0';

        if (!starts_with_reply_prefix(buffer)) {
            close(socket_fd);
            return false;
        }

        close(socket_fd);

        std::memset(&active_link->link, 0, sizeof(active_link->link));
        active_link->key = std::string("bt|") + device.mac_address;
        active_link->link.linkType = LINK_TYPE_GREEN;
        active_link->link.instrumentType = INSTRUMENT_BLUETOOTH;
        copy_string(active_link->link.linkID, sizeof(active_link->link.linkID), make_link_id("bt", device.mac_address));
        copy_string(active_link->link.devID, sizeof(active_link->link.devID), device.mac_address);
        active_link->link.bluetoothDeviceInfo = device;
        return true;
    }

    bool publish_incoming_bluetooth_link(const std::string& mac_address, const std::string& name) {
        if (mac_address.empty()) {
            comms_debug("incoming bluetooth link publish skipped: empty mac");
            return false;
        }

        ActiveLink active_link;
        std::memset(&active_link.link, 0, sizeof(active_link.link));
        active_link.key = std::string("bt|") + mac_address;
        active_link.link.linkType = LINK_TYPE_GREEN;
        active_link.link.instrumentType = INSTRUMENT_BLUETOOTH;
        copy_string(active_link.link.linkID, sizeof(active_link.link.linkID), make_link_id("bt", mac_address));
        copy_string(active_link.link.devID, sizeof(active_link.link.devID), mac_address);
        active_link.link.bluetoothDeviceInfo.major = UNCATEGORIZED;
        active_link.link.bluetoothDeviceInfo.rssi.valid = false;
        copy_string(active_link.link.bluetoothDeviceInfo.mac_address,
                    sizeof(active_link.link.bluetoothDeviceInfo.mac_address),
                    mac_address);
        copy_string(active_link.link.bluetoothDeviceInfo.name,
                    sizeof(active_link.link.bluetoothDeviceInfo.name),
                    name);

        comms_debug("publishing incoming bluetooth link: mac=%s name=%s", mac_address.c_str(), name.c_str());
        publish_link(std::move(active_link));
        return true;
    }

    bool publish_incoming_wifi_link(const std::string& interface_name,
                                    const std::string& ip_address,
                                    const std::string& mac_address) {
        if (interface_name.empty() || ip_address.empty() || mac_address.empty()) {
            comms_debug("incoming wifi link publish skipped: if=%s ip=%s mac=%s",
                        interface_name.c_str(),
                        ip_address.c_str(),
                        mac_address.c_str());
            return false;
        }

        ActiveLink active_link;
        std::memset(&active_link.link, 0, sizeof(active_link.link));
        active_link.key = std::string("wifi|") + mac_address;
        active_link.link.linkType = LINK_TYPE_BLUE;
        active_link.link.instrumentType = INSTRUMENT_WIFI;
        copy_string(active_link.link.linkID, sizeof(active_link.link.linkID), make_link_id("wifi", mac_address));
        copy_string(active_link.link.devID, sizeof(active_link.link.devID), mac_address);
        active_link.link.wifiDeviceInfo.type = WIFI_ADAPTER;
        active_link.link.wifiDeviceInfo.rssi.valid = false;
        copy_string(active_link.link.wifiDeviceInfo.mac_address,
                    sizeof(active_link.link.wifiDeviceInfo.mac_address),
                    mac_address);
        copy_string(active_link.link.wifiDeviceInfo.ssid,
                    sizeof(active_link.link.wifiDeviceInfo.ssid),
                    ip_address);

        comms_debug("publishing incoming wifi link: if=%s ip=%s mac=%s",
                    interface_name.c_str(),
                    ip_address.c_str(),
                    mac_address.c_str());
        publish_link(std::move(active_link));
        return true;
    }

    void publish_link(ActiveLink active_link) {
        LinkDiscoveredCallback callback = nullptr;
        BratislavaLink* published_link = nullptr;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!active_link_keys_.insert(active_link.key).second) {
                return;
            }

            active_links_.push_back(std::move(active_link));
            published_link = &active_links_.back().link;
            callback = callbacks_.link_discovered;

            if (bratislavaSocket(*published_link) == nullptr) {
                active_link_keys_.erase(active_links_.back().key);
                active_links_.pop_back();
                return;
            }
        }

        if (callback != nullptr) {
            callback(published_link);
        }
    }

    void udp_server_loop() {
        const int socket_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (socket_fd < 0) {
            return;
        }

        int reuse = 1;
        (void)setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        int packet_info = 1;
        (void)setsockopt(socket_fd, IPPROTO_IP, IP_PKTINFO, &packet_info, sizeof(packet_info));

        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        local.sin_port = htons(kUdpPort);

        if (bind(socket_fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
            close(socket_fd);
            return;
        }

        set_receive_timeout(socket_fd, kServerPollTimeoutMs);

        while (running_) {
            std::array<char, 512> buffer{};
            sockaddr_in remote{};
            char control[CMSG_SPACE(sizeof(in_pktinfo))] = {0};

            iovec io_vector{};
            io_vector.iov_base = buffer.data();
            io_vector.iov_len = buffer.size() - 1;

            msghdr message{};
            message.msg_name = &remote;
            message.msg_namelen = sizeof(remote);
            message.msg_iov = &io_vector;
            message.msg_iovlen = 1;
            message.msg_control = control;
            message.msg_controllen = sizeof(control);

            const ssize_t received = recvmsg(socket_fd, &message, 0);
            if (received < 0) {
                if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
                    continue;
                }
                comms_debug("udp server recvmsg failed: errno=%d", errno);
                break;
            }

            buffer[static_cast<size_t>(received)] = '\0';
            unsigned int incoming_ifindex = 0U;
            std::string interface_name;
            for (cmsghdr* control_message = CMSG_FIRSTHDR(&message);
                 control_message != nullptr;
                 control_message = CMSG_NXTHDR(&message, control_message)) {
                if ((control_message->cmsg_level == IPPROTO_IP) &&
                    (control_message->cmsg_type == IP_PKTINFO) &&
                    (control_message->cmsg_len >= CMSG_LEN(sizeof(in_pktinfo)))) {
                    const auto* packet_info_data = reinterpret_cast<const in_pktinfo*>(CMSG_DATA(control_message));
                    incoming_ifindex = packet_info_data->ipi_ifindex;
                    interface_name = interface_name_from_index(packet_info_data->ipi_ifindex);
                    break;
                }
            }

            char remote_ip[INET_ADDRSTRLEN] = {0};
            (void)inet_ntop(AF_INET, &remote.sin_addr, remote_ip, sizeof(remote_ip));
            comms_debug("udp server got '%s' from %s:%u on if=%s",
                        buffer.data(),
                        remote_ip,
                        (unsigned)ntohs(remote.sin_port),
                        interface_name.c_str());

            if (std::strcmp(buffer.data(), kHelloPayload) == 0) {
                const std::string remote_mac = arp_mac_for_ip(interface_name, remote_ip);
                if (!remote_mac.empty()) {
                    (void)publish_incoming_wifi_link(interface_name, remote_ip, remote_mac);
                } else {
                    comms_debug("udp server could not resolve ARP for %s on if=%s",
                                remote_ip,
                                interface_name.c_str());
                }
            }

            const std::string reply = std::string(kReplyPrefix) + local_interface_mac(interface_name);
            char reply_control[CMSG_SPACE(sizeof(in_pktinfo))] = {0};
            iovec reply_iov{};
            reply_iov.iov_base = const_cast<char*>(reply.data());
            reply_iov.iov_len = reply.size();

            msghdr reply_message{};
            reply_message.msg_name = &remote;
            reply_message.msg_namelen = message.msg_namelen;
            reply_message.msg_iov = &reply_iov;
            reply_message.msg_iovlen = 1;

            if (incoming_ifindex != 0U) {
                reply_message.msg_control = reply_control;
                reply_message.msg_controllen = sizeof(reply_control);
                cmsghdr* const reply_cmsg = CMSG_FIRSTHDR(&reply_message);
                if (reply_cmsg != nullptr) {
                    reply_cmsg->cmsg_level = IPPROTO_IP;
                    reply_cmsg->cmsg_type = IP_PKTINFO;
                    reply_cmsg->cmsg_len = CMSG_LEN(sizeof(in_pktinfo));

                    auto* const reply_info = reinterpret_cast<in_pktinfo*>(CMSG_DATA(reply_cmsg));
                    std::memset(reply_info, 0, sizeof(*reply_info));
                    reply_info->ipi_ifindex = static_cast<int>(incoming_ifindex);
                    reply_message.msg_controllen = reply_cmsg->cmsg_len;
                }
            }

            (void)sendmsg(socket_fd, &reply_message, 0);
            comms_debug("udp server replied '%s'", reply.c_str());
        }

        close(socket_fd);
    }

    void bluetooth_server_loop() {
        const int server_fd = socket(AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_CLOEXEC, BTPROTO_L2CAP);
        if (server_fd < 0) {
            return;
        }

        sockaddr_l2 local{};
        local.l2_family = AF_BLUETOOTH;
        local.l2_psm = htobs(kBluetoothPsm);

        if (bind(server_fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
            close(server_fd);
            return;
        }

        if (listen(server_fd, 4) != 0) {
            close(server_fd);
            return;
        }

        set_receive_timeout(server_fd, kServerPollTimeoutMs);

        while (running_) {
            sockaddr_l2 remote{};
            socklen_t remote_len = sizeof(remote);
            const int client_fd = accept(server_fd, reinterpret_cast<sockaddr*>(&remote), &remote_len);
            if (client_fd < 0) {
                if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
                    continue;
                }
                break;
            }

            set_receive_timeout(client_fd, kClientTimeoutMs);

            std::array<char, 512> buffer{};
            const ssize_t received = recv(client_fd, buffer.data(), buffer.size() - 1, 0);
            if (received > 0) {
                char remote_address[18] = {0};
                ba2str(&remote.l2_bdaddr, remote_address);
                std::string remote_mac = normalize_mac(remote_address);

                if (std::strcmp(buffer.data(), kHelloPayload) == 0) {
                    (void)publish_incoming_bluetooth_link(remote_mac, "");
                }

                sockaddr_l2 local_socket{};
                socklen_t local_len = sizeof(local_socket);
                std::string local_mac;
                if (getsockname(client_fd, reinterpret_cast<sockaddr*>(&local_socket), &local_len) == 0) {
                    char address[18] = {0};
                    ba2str(&local_socket.l2_bdaddr, address);
                    local_mac = normalize_mac(address);
                }
                const std::string reply = std::string(kReplyPrefix) + local_mac;
                (void)send(client_fd, reply.data(), reply.size(), 0);
            }

            close(client_fd);
        }

        close(server_fd);
    }

    InstrumentAPIConfig config_{};
    std::atomic<bool> running_{false};
    std::thread udp_server_thread_;
    std::thread bluetooth_server_thread_;
    std::thread discovery_thread_;
    std::mutex mutex_;
    std::mutex discovery_mutex_;
    std::condition_variable discovery_cv_;
    CallbackState callbacks_{};
    std::list<ActiveLink> active_links_;
    std::unordered_set<std::string> active_link_keys_;
    std::unordered_set<std::string> seen_bluetooth_devices_;
    std::unordered_set<std::string> seen_wifi_devices_;
};

std::mutex g_runtime_mutex;
std::unique_ptr<CommsRuntime> g_runtime;
size_t g_runtime_refcount = 0U;

CommsRuntime* get_runtime() {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    return g_runtime.get();
}

} // namespace

extern "C" instrument_api_status_t comms_runtime_initialize(const InstrumentAPIConfig* config) {
    if (config == nullptr) {
        return INSTRUMENT_API_ERROR;
    }

    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    if (g_runtime == nullptr) {
        g_runtime = std::make_unique<CommsRuntime>(*config);
        const instrument_api_status_t status = g_runtime->start();
        if (status != INSTRUMENT_API_SUCCESS) {
            g_runtime.reset();
            return status;
        }
    }
    ++g_runtime_refcount;
    return INSTRUMENT_API_SUCCESS;
}

extern "C" void comms_runtime_shutdown(void) {
    std::unique_ptr<CommsRuntime> runtime_to_stop;
    {
        std::lock_guard<std::mutex> lock(g_runtime_mutex);
        if (g_runtime_refcount == 0U) {
            return;
        }
        --g_runtime_refcount;
        if ((g_runtime_refcount == 0U) && (g_runtime != nullptr)) {
            runtime_to_stop = std::move(g_runtime);
        }
    }

    if (runtime_to_stop != nullptr) {
        runtime_to_stop->stop();
    }
}

extern "C" instrument_api_status_t comms_runtime_register_callback(InstrumentType instrument_type,
                                                                   CallbackType callback_type,
                                                                   InstrumentInputType input_data) {
    CommsRuntime* runtime = get_runtime();
    if (runtime == nullptr) {
        return INSTRUMENT_API_ERROR;
    }
    return runtime->register_callback(instrument_type, callback_type, input_data);
}

extern "C" instrument_api_status_t comms_runtime_unregister_callback(InstrumentType instrument_type,
                                                                     CallbackType callback_type,
                                                                     InstrumentInputType input_data) {
    CommsRuntime* runtime = get_runtime();
    if (runtime == nullptr) {
        return INSTRUMENT_API_ERROR;
    }
    return runtime->unregister_callback(instrument_type, callback_type, input_data);
}

extern "C" instrument_api_status_t comms_runtime_discover_links(InstrumentType instrument_type,
                                                                InstrumentOutputType output_data,
                                                                uint32_t* output_len) {
    CommsRuntime* runtime = get_runtime();
    if (runtime == nullptr) {
        return INSTRUMENT_API_ERROR;
    }
    return runtime->discover_links(instrument_type, output_data, output_len);
}
