/* socket.cpp                                 -*- C++ -*-
   Rémi Attab (remi.attab@gmail.com), 18 Nov 2013
   FreeBSD-style copyright and disclaimer apply

   Socket abstraction implementation
*/

#include "socket.h"
#include "utils.h"

#include <string>
#include <cstring>
#include <cassert>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

namespace slick {


/******************************************************************************/
/* INTERFACE IT                                                               */
/******************************************************************************/

struct InterfaceIt
{
    InterfaceIt(const char* host, Port port) :
        first(nullptr), cur(nullptr)
    {
        struct addrinfo hints;
        std::memset(&hints, 0, sizeof hints);

        hints.ai_flags = host ? AI_PASSIVE : 0;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        assert(!host || port);
        std::string portStr = std::to_string(port);

        int ret = getaddrinfo(host, portStr.c_str(), &hints, &first);
        if (ret) {
            SLICK_CHECK_ERRNO(ret != EAI_SYSTEM, "InterfaceIt.getaddrinfo");
            throw std::logic_error("error: " + std::to_string(ret));
        }

        cur = first;
    }

    ~InterfaceIt()
    {
        if (first) freeaddrinfo(first);
    }

    explicit operator bool() const { return cur; }
    void operator++ () { cur = cur->ai_next; }
    void operator++ (int) { cur = cur->ai_next; }

    const struct addrinfo& operator* () const { return *cur; }
    const struct addrinfo* operator-> () const { return cur; }


private:
    struct addrinfo* first;
    struct addrinfo* cur;
};


/******************************************************************************/
/* SOCKET                                                                     */
/******************************************************************************/


Socket::
Socket(Socket&& other) :
    fd_(other.fd_),
    addr(std::move(other.addr)),
    addrlen(std::move(other.addrlen))
{
    other.fd_ = -1;
}


Socket&
Socket::
operator=(Socket&& other)
{
    fd_ = other.fd_;
    other.fd_ = -1;

    addr = std::move(other.addr);
    addrlen = std::move(other.addrlen);

    return *this;
}


Socket::
Socket(const std::string& host, PortRange ports, int flags) :
    fd_(-1)
{
    assert(!host.empty());
    Port port = ports.first;

    for (InterfaceIt it(host.c_str(), port); it; it++) {
        int fd = socket(it->ai_family, it->ai_socktype | flags, it->ai_protocol);
        if (fd < 0) continue;

        FdGuard guard(fd);

        int ret = connect(fd, it->ai_addr, it->ai_addrlen);
        if (ret < 0 && errno != EINPROGRESS) continue;

        fd_ = guard.release();

        addrlen = it->ai_addrlen;
        std::memcpy(&addr, &it->ai_addr, sizeof addr);
        break;
    }

    if (fd_ < 0) throw std::runtime_error("ERROR: no valid interface");
    init();
}


Socket
Socket::
accept(int fd, int flags)
{
    Socket socket;

    socket.fd_ = accept4(fd, &socket.addr, &socket.addrlen, flags);
    if (socket.fd_ < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return std::move(socket);
    SLICK_CHECK_ERRNO(socket.fd_ >= 0, "Socket.accept");

    socket.init();
    return std::move(socket);
}

void
Socket::
init()
{
    int val = true;
    int ret = setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &val, sizeof val);
    SLICK_CHECK_ERRNO(!ret, "Socket.setsockopt.TCP_NODELAY");
}

Socket::
~Socket()
{
    if (fd_ < 0) return;

    int ret = shutdown(fd_, SHUT_RDWR);
    SLICK_CHECK_ERRNO(!ret, "Socket.shutdown");

    ret = close(fd_);
    SLICK_CHECK_ERRNO(!ret, "Socket.close");
}

int 
Socket::
error() const
{
    int error = 0;
    socklen_t errlen = sizeof error;

    int ret = getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &errlen);
    SLICK_CHECK_ERRNO(!ret, "Socket.getsockopt.error");

    return error;
}

void 
Socket::
throwError() const
{
    int err = error();
    if (err) throw std::runtime_error(checkErrnoString(err, "Socket.error"));
}


/******************************************************************************/
/* PASSIVE SOCKET                                                             */
/******************************************************************************/

PassiveSockets::
PassiveSockets(PortRange ports, int flags)
{
    Port port = ports.first; // \todo Need to support multiple ports.

    for (InterfaceIt it(nullptr, port); it; it++) {
        int fd = socket(it->ai_family, it->ai_socktype | flags, it->ai_protocol);
        if (fd < 0) continue;

        FdGuard guard(fd);


        int ret = bind(fd, it->ai_addr, it->ai_addrlen);
        if (ret < 0) continue;

        ret = listen(fd, 1U << 8);
        if (ret < 0) continue;

        fds_.push_back(guard.release());
    }

    if (fds_.empty()) throw std::runtime_error("ERROR: no valid interface");
}

PassiveSockets::
~PassiveSockets()
{
    for (int fd : fds_) close(fd);
}


} // slick

