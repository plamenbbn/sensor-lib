#include "wifi_scanner_bridge.h"

#include "wifi_typedef.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <linux/nl80211.h>
#include <net/if.h>
#include <netlink/genl/ctrl.h>
#include <netlink/genl/genl.h>
#include <netlink/handlers.h>
#include <netlink/msg.h>
#include <netlink/netlink.h>

namespace {

struct WifiNetwork {
    std::string ssid;
    std::string bssid;
    double signal_dbm = 0.0;
    bool has_signal = false;
};

struct ScanResultState {
    std::vector<WifiNetwork>* networks;
};

std::string normalize_mac_address(const std::string& input) {
    std::string hex;
    hex.reserve(12);
    for (char ch : input) {
        if (std::isxdigit(static_cast<unsigned char>(ch))) {
            hex.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
            if (hex.size() == 12) {
                break;
            }
        }
    }

    if (hex.size() < 12) {
        return std::string();
    }

    std::string normalized;
    normalized.reserve(17);
    for (size_t i = 0; i < 12; i += 2) {
        if (!normalized.empty()) {
            normalized.push_back(':');
        }
        normalized.push_back(hex[i]);
        normalized.push_back(hex[i + 1]);
    }
    return normalized;
}

std::string mac_to_string(const uint8_t* mac) {
    char buf[18];
    std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buf;
}

double convert_signal_mbm(const int32_t mbm) {
    if (mbm == 0) {
        return 0.0;
    }
    return static_cast<double>(mbm) / 100.0;
}

const uint8_t* find_ssid_ie(const uint8_t* ies, const int len, int& out_len) {
    int pos = 0;
    while (pos + 2 <= len) {
        const uint8_t id = ies[pos];
        const uint8_t ie_len = ies[pos + 1];
        if (pos + 2 + ie_len > len) {
            break;
        }
        if (id == 0) {
            out_len = ie_len;
            return ies + pos + 2;
        }
        pos += 2 + ie_len;
    }
    return nullptr;
}

int scan_result_handler(struct nl_msg* msg, void* arg) {
    auto* state = static_cast<ScanResultState*>(arg);
    auto* hdr = static_cast<genlmsghdr*>(nlmsg_data(nlmsg_hdr(msg)));

    struct nlattr* tb[NL80211_ATTR_MAX + 1];
    nla_parse(tb, NL80211_ATTR_MAX, genlmsg_attrdata(hdr, 0), genlmsg_attrlen(hdr, 0), nullptr);

    if (tb[NL80211_ATTR_BSS] == nullptr) {
        return NL_SKIP;
    }

    struct nlattr* bss[NL80211_BSS_MAX + 1];
    nla_parse_nested(bss, NL80211_BSS_MAX, tb[NL80211_ATTR_BSS], nullptr);

    WifiNetwork net;
    if (bss[NL80211_BSS_BSSID] != nullptr) {
        auto* mac = static_cast<uint8_t*>(nla_data(bss[NL80211_BSS_BSSID]));
        net.bssid = normalize_mac_address(mac_to_string(mac));
    }
    if (net.bssid.empty()) {
        return NL_SKIP;
    }

    if (bss[NL80211_BSS_INFORMATION_ELEMENTS] != nullptr) {
        auto* ies = static_cast<uint8_t*>(nla_data(bss[NL80211_BSS_INFORMATION_ELEMENTS]));
        const int ies_len = nla_len(bss[NL80211_BSS_INFORMATION_ELEMENTS]);
        int ssid_len = 0;
        const uint8_t* ssid = find_ssid_ie(ies, ies_len, ssid_len);
        if ((ssid != nullptr) && (ssid_len > 0)) {
            net.ssid.assign(reinterpret_cast<const char*>(ssid), static_cast<size_t>(ssid_len));
        }
    }

    if (bss[NL80211_BSS_SIGNAL_MBM] != nullptr) {
        const int32_t mbm = static_cast<int32_t>(nla_get_s32(bss[NL80211_BSS_SIGNAL_MBM]));
        net.signal_dbm = convert_signal_mbm(mbm);
        net.has_signal = true;
    }

    state->networks->push_back(net);
    return NL_OK;
}

int finish_handler(struct nl_msg*, void* arg) {
    auto* done = static_cast<int*>(arg);
    *done = 0;
    return NL_SKIP;
}

int error_handler(struct sockaddr_nl*, struct nlmsgerr* err, void* arg) {
    auto* result = static_cast<int*>(arg);
    *result = err->error;
    return NL_SKIP;
}

instrument_api_status_t translate_nl_error(const int err) {
    switch (-err) {
    case EPERM:
    case EACCES:
    case EOPNOTSUPP:
    case ENODEV:
    case EINVAL:
        return INSTRUMENT_API_NOT_SUPPORTED;
    default:
        return INSTRUMENT_API_ERROR;
    }
}

instrument_api_status_t trigger_scan(struct nl_sock* sock, const int nl80211_id, const int ifindex) {
    std::unique_ptr<nl_msg, decltype(&nlmsg_free)> msg(nlmsg_alloc(), nlmsg_free);
    if (!msg) {
        return INSTRUMENT_API_ERROR;
    }

    genlmsg_put(msg.get(), 0, 0, nl80211_id, 0, 0, NL80211_CMD_TRIGGER_SCAN, 0);
    nla_put_u32(msg.get(), NL80211_ATTR_IFINDEX, static_cast<uint32_t>(ifindex));
    nla_put(msg.get(), NL80211_ATTR_SCAN_SSIDS, 0, "");

    int err = nl_send_auto(sock, msg.get());
    if (err < 0) {
        return translate_nl_error(err);
    }

    int done = 1;
    int callback_err = 0;
    std::unique_ptr<nl_cb, decltype(&nl_cb_put)> cb(nl_cb_alloc(NL_CB_DEFAULT), nl_cb_put);
    if (!cb) {
        return INSTRUMENT_API_ERROR;
    }

    nl_cb_set(cb.get(), NL_CB_FINISH, NL_CB_CUSTOM, finish_handler, &done);
    nl_cb_set(cb.get(), NL_CB_ACK, NL_CB_CUSTOM, finish_handler, &done);
    nl_cb_err(cb.get(), NL_CB_CUSTOM, error_handler, &callback_err);

    while (done != 0) {
        err = nl_recvmsgs(sock, cb.get());
        if (err < 0) {
            return translate_nl_error(err);
        }
        if (callback_err != 0) {
            return translate_nl_error(callback_err);
        }
    }

    return INSTRUMENT_API_SUCCESS;
}

instrument_api_status_t fetch_scan_results(struct nl_sock* sock,
                                           const int nl80211_id,
                                           const int ifindex,
                                           std::vector<WifiNetwork>& networks) {
    std::unique_ptr<nl_msg, decltype(&nlmsg_free)> msg(nlmsg_alloc(), nlmsg_free);
    if (!msg) {
        return INSTRUMENT_API_ERROR;
    }

    genlmsg_put(msg.get(), 0, 0, nl80211_id, 0, NLM_F_DUMP, NL80211_CMD_GET_SCAN, 0);
    nla_put_u32(msg.get(), NL80211_ATTR_IFINDEX, static_cast<uint32_t>(ifindex));

    ScanResultState state{&networks};
    std::unique_ptr<nl_cb, decltype(&nl_cb_put)> cb(nl_cb_alloc(NL_CB_DEFAULT), nl_cb_put);
    if (!cb) {
        return INSTRUMENT_API_ERROR;
    }

    nl_cb_set(cb.get(), NL_CB_VALID, NL_CB_CUSTOM, scan_result_handler, &state);

    int err = nl_send_auto(sock, msg.get());
    if (err < 0) {
        return translate_nl_error(err);
    }

    int recv_err = 1;
    while (recv_err > 0) {
        recv_err = nl_recvmsgs(sock, cb.get());
    }
    if (recv_err < 0) {
        return translate_nl_error(recv_err);
    }

    return INSTRUMENT_API_SUCCESS;
}

} // namespace

extern "C" instrument_api_status_t wifi_discover_devices_nl80211(const char* ifname,
                                                                 InstrumentOutputType output_data,
                                                                 uint32_t* output_len) {
    if ((ifname == nullptr) || (output_data == nullptr) || (output_len == nullptr)) {
        return INSTRUMENT_API_ERROR;
    }

    const int ifindex = if_nametoindex(ifname);
    if (ifindex == 0) {
        return INSTRUMENT_API_NOT_SUPPORTED;
    }

    std::unique_ptr<nl_sock, decltype(&nl_socket_free)> sock(nl_socket_alloc(), nl_socket_free);
    if (!sock) {
        return INSTRUMENT_API_ERROR;
    }

    if (genl_connect(sock.get()) != 0) {
        return INSTRUMENT_API_ERROR;
    }

    const int nl80211_id = genl_ctrl_resolve(sock.get(), "nl80211");
    if (nl80211_id < 0) {
        return INSTRUMENT_API_NOT_SUPPORTED;
    }

    const instrument_api_status_t trigger_status = trigger_scan(sock.get(), nl80211_id, ifindex);
    if ((trigger_status != INSTRUMENT_API_SUCCESS) && (trigger_status != INSTRUMENT_API_NOT_SUPPORTED)) {
        return trigger_status;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::vector<WifiNetwork> networks;
    const instrument_api_status_t fetch_status = fetch_scan_results(sock.get(), nl80211_id, ifindex, networks);
    if (fetch_status != INSTRUMENT_API_SUCCESS) {
        return fetch_status;
    }

    auto* devices = static_cast<WifiDeviceInfoBase*>(output_data);
    const uint32_t max_count = WIFI_MAX_DEVICES;
    const uint32_t count = static_cast<uint32_t>(std::min<size_t>(networks.size(), max_count));

    for (uint32_t i = 0; i < count; ++i) {
        const WifiNetwork& net = networks[i];
        std::memset(&devices[i], 0, sizeof(devices[i]));
        devices[i].type = WIFI_ROUTER;
        std::snprintf(devices[i].mac_address, sizeof(devices[i].mac_address), "%s", net.bssid.c_str());
        std::snprintf(devices[i].ssid, sizeof(devices[i].ssid), "%s", net.ssid.c_str());
        devices[i].rssi.valid = net.has_signal;
        devices[i].rssi.value_dbm = static_cast<int16_t>(net.signal_dbm);
    }

    *output_len = count;
    return (count > 0U) ? INSTRUMENT_API_SUCCESS : INSTRUMENT_API_NOT_SUPPORTED;
}
