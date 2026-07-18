#ifndef BRATISLAVA_SOCKET_IMPL_H
#define BRATISLAVA_SOCKET_IMPL_H

#include <shared_mutex>

// sensing_typedef.h transitively pulls in bluetooth_typedef.h and
// wifi_typedef.h via BratislavaLink's union members; including those
// directly here is unnecessary.
#include "sensing_typedef.h"

#include <adapters/BaseAdapter.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// BratislavaSocket definition with public members.
struct BratislavaSocket {
    BratislavaLink    link{};
    BaseAdapter*      adapter{};
    std::shared_mutex mutex_shared_bsock_;
};

void bratislavaDisconnect(BratislavaSocket* bsock);
void bratislavaSocketInit();

#ifdef __cplusplus
}
#endif

#endif // BRATISLAVA_SOCKET_IMPL_H
