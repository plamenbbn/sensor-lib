#ifndef BRATISLAVA_SOCKET_H
#define BRATISLAVA_SOCKET_H

#ifdef __cplusplus
extern "C" {
#endif

#include "sensing_typedef.h"

#include <stddef.h>

#define DEFAULT_CONN_TIMEOUT_MS 10000 // Default connection timeout, in milliseconds
#define DEFAULT_RECV_TIMEOUT_MS 5000  // Default messaging recv timeout, in milliseconds

/* Opaque struct declaration */
typedef struct BratislavaSocket BratislavaSocket;

/**
 * Fetch the BratislavaSocket corresponding to a BratislavaLink.
 *
 * @return a socket pointer, if one exists; a null pointer otherwise
 */
BratislavaSocket* bratislavaSocket(const BratislavaLink link);
/**
 * Initiate a connection on a Bratislava socket.
 *
 * @param bsock the socket to connect
 * @return 0 if a connection already exists or has been successfully established; -1 otherwise, and errno will be set
 */
int bratislavaConn(BratislavaSocket* bsock);

/**
 * Write bytes to the socket.
 *
 * NB: bratislavaConn must have been called successfully on the socket previously.
 *
 * @param bsock socket to write to
 * @param buf buffer to write from
 * @param len number of bytes to write from the buffer
 * @return number of bytes sent, or -1 for errors, in which case errno will be set
 */
int bratislavaSend(BratislavaSocket* bsock, const void* buf, size_t len);

/**
 * Read bytes from the socket.
 *
 * NB: bratislavaConn must have been called successfully on the socket previously.
 *
 * NB: if a negative value is returned, the connection may be broken and you must call bratislavaDestroy.
 *
 * @param bsock socket to read from
 * @param buf buffer to read into
 * @param len maximum number of bytes the buffer can hold
 * @return number of bytes read, or -1 for errors, in which case errno will be set
 */
int bratislavaRecv(BratislavaSocket* bsock, void* buf, size_t len);

/**
 * Set the connection timeout on a socket.
 *
 * @param bsock the socket to update
 * @param milliseconds connection timeout, in milliseconds.
 * @return 0 for success, or -1 for errors, in which case errno will be set
 */
int bratislavaConnTimeout(BratislavaSocket* bsock, unsigned int milliseconds);

/**
 * Set the receive timeout on a socket.
 *
 * @param bsock the socket to update
 * @param milliseconds receive timeout, in milliseconds.
 * @return 0 for success, or -1 for errors, in which case errno will be set
 */
int bratislavaRecvTimeout(BratislavaSocket* bsock, unsigned int milliseconds);

/**
 * Tear down a Bratislava connection on a socket.
 *
 * @param bsock the socket to tear down a connection on
 */
void bratislavaDestroy(BratislavaSocket* bsock);

/**
 * Get the name and statistics for a Bratislava Link. Links are a more ergonomic
 * alternative to raw sockets.
 */
BratislavaLinkInfo bratislavaGetLinkInfo(const BratislavaLink* blink);

/**
 * Write bytes to a Bratislava Link.
 *
 * A convenience function for creating a socket, connecting it, and then reading
 * bytes from it.
 *
 * @param blink the link to write to
 * @param buf buffer to write from
 * @param len number of bytes to write from the buffer
 * @return number of bytes sent, or -1 for errors, in which case errno will be set
 */
int bratislavaLinkRecv(const BratislavaLink* blink, void* buf, size_t len);

/**
 * Read bytes from a Bratislava Link.
 *
 * A convenience function for creating a socket, connecting it, and then sending
 * bytes to it.
 *
 * @param blink the link to read from
 * @param buf buffer to read into
 * @param len number of bytes to read into the buffer
 * @return number of bytes read, or -1 for errors, in which case errno will be set
 */
int bratislavaLinkSend(const BratislavaLink* blink, const void* buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif // BRATISLAVA_SOCKET_H
