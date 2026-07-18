#ifndef BRATISLAVA_SOCKET_COMPAT_H
#define BRATISLAVA_SOCKET_COMPAT_H

#include <arpa/inet.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#define LOG_DEBUG(...) ((void)0)
#define LOG_INFO(...) ((void)0)
#define LOG_ERROR(...) ((void)0)

inline std::string bratislavaErrnoString(const int err = errno) {
    const char* text = std::strerror(err);
    return text != nullptr ? std::string(text) : std::string("unknown error");
}

inline std::string bratislavaIpv4String(const in_addr& address) {
    char buffer[INET_ADDRSTRLEN] = {0};
    if (inet_ntop(AF_INET, &address, buffer, sizeof(buffer)) == nullptr) {
        return {};
    }
    return std::string(buffer);
}

template <size_t N>
inline void bratislavaCopyFixed(char (&dst)[N], const char* src) {
    if (N == 0U) {
        return;
    }
    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }
    std::snprintf(dst, N, "%s", src);
}

#endif
