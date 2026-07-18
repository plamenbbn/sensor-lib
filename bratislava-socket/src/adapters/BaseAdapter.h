#ifndef BASE_ADAPTER_H
#define BASE_ADAPTER_H

#include <cstddef>

class BaseAdapter {
public:
    virtual int conn()                                    = 0;
    virtual int disconn()                                 = 0;
    virtual int send(const void* buf, size_t len)         = 0;
    virtual int recv(void* buf, size_t len)               = 0;
    virtual int setConnTimeout(unsigned int milliseconds) = 0;
    virtual int setRecvTimeout(unsigned int milliseconds) = 0;

    // Wake any thread currently blocked in send()/recv(). The default routes to disconn(),
    // which tears the connection down. Adapters that can wake blocked I/O WITHOUT closing
    // the fd should override this: closing the fd while another thread is still in
    // recvfrom()/sendto() is a use-after-close race, so such adapters must shutdown() here
    // and leave the fd for the destructor to close once in-flight I/O has drained.
    virtual void interrupt() { disconn(); }

    virtual ~BaseAdapter() {} // Virtual destructor for proper cleanup
};

#endif // BASE_ADAPTER_H
