#pragma once
// Minimal BSD-socket portability for the engine's loopback HTTP server (127.0.0.1 only).
//
//   POSIX (macOS, Linux): the usual headers. Windows: Winsock2, initialised once on first use.
//   Sockets stay `int` in the engine (Winsock handles fit in practice and are cast back at the call).
//
//   rig::net::ensureInit()          WSAStartup once (no-op elsewhere); call before the first socket().
//   rig::net::closeSocket(fd)       close() / closesocket()
//   rig::net::shutdownListener(fd)  wake a blocked accept() before closing (Linux/Windows need it)
//   rig::net::kSendFlags            MSG_NOSIGNAL on Linux (no SIGPIPE on a closed peer); 0 elsewhere
//   rig::net::noSigPipe(fd)         SO_NOSIGPIPE on Apple (per socket, never process-wide); no-op elsewhere
//   rig::net::setRecvTimeout(fd,ms) SO_RCVTIMEO (timeval on POSIX, DWORD ms on Windows)
//   rig::net::sendBytes(fd,p,n)     one send() with the flags above; returns bytes sent or <= 0
#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mutex>
#pragma comment(lib, "Ws2_32.lib")
#include <BaseTsd.h>
#ifndef RIG_SSIZE_T_DEFINED
#define RIG_SSIZE_T_DEFINED
using ssize_t = SSIZE_T;
#endif
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace rig::net {

#if defined(__linux__)
inline constexpr int kSendFlags = MSG_NOSIGNAL;
#else
inline constexpr int kSendFlags = 0;
#endif

inline bool ensureInit() {
#ifdef _WIN32
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        WSADATA d;
        ok = WSAStartup(MAKEWORD(2, 2), &d) == 0;
    });
    return ok;
#else
    return true;
#endif
}

inline int closeSocket(int fd) {
#ifdef _WIN32
    return ::closesocket(static_cast<SOCKET>(fd));
#else
    return ::close(fd);
#endif
}

// Wakes a thread blocked in accept() on this listening socket before it is closed: on Linux (and Windows) close()
// alone leaves accept() blocked forever. Harmless on macOS (ENOTCONN).
inline void shutdownListener(int fd) {
#ifdef _WIN32
    ::shutdown(static_cast<SOCKET>(fd), SD_BOTH);
#else
    ::shutdown(fd, SHUT_RDWR);
#endif
}

inline void noSigPipe(int fd) {
#if defined(SO_NOSIGPIPE)
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
    (void)fd;
#endif
}

inline void setRecvTimeout(int fd, int ms) {
#ifdef _WIN32
    const DWORD t = static_cast<DWORD>(ms);
    ::setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
#else
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

inline void setSendTimeout(int fd, int ms) {
#ifdef _WIN32
    const DWORD t = static_cast<DWORD>(ms);
    ::setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
#else
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

inline ssize_t sendBytes(int fd, const void* p, std::size_t n) {
#ifdef _WIN32
    return ::send(static_cast<SOCKET>(fd), static_cast<const char*>(p), static_cast<int>(n), 0);
#else
    return ::send(fd, p, n, kSendFlags);
#endif
}

inline ssize_t recvBytes(int fd, void* p, std::size_t n) {
#ifdef _WIN32
    return ::recv(static_cast<SOCKET>(fd), static_cast<char*>(p), static_cast<int>(n), 0);
#else
    return ::recv(fd, p, n, 0);
#endif
}

} // namespace rig::net
