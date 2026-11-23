#pragma once

#include <cstring>
#include <string>

// Cross-platform socket shim: POSIX BSD sockets (Linux/macOS) vs Windows Winsock2.
// Sockets are stored as plain `int` throughout the codebase; on Win64 a SOCKET is a
// 64-bit handle, but the values are small kernel-table indices that fit an int in
// practice (and INVALID_SOCKET casts to -1, matching the `fd < 0` idiom). Keeping
// `int` avoids threading a socket typedef through the server and client.

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #define TAK_POLL WSAPoll
#else
  #include <fcntl.h>
  #include <netdb.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <poll.h>
  #include <sys/socket.h>
  #include <sys/types.h>
  #include <unistd.h>
  #include <cerrno>
  #define TAK_POLL ::poll
#endif

// Linux defines MSG_NOSIGNAL; Windows and macOS don't (macOS uses SO_NOSIGPIPE
// per-socket instead, set in setupSocket). 0 is a safe no-op flag on both.
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace tak::net {

// Initialise the socket subsystem once (WSAStartup on Windows; no-op elsewhere).
// Idempotent; call before any socket use (connect/listen/pickFreePort).
inline void netStartup() {
#ifdef _WIN32
    static bool done = false;
    if (!done) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); done = true; }
#endif
}

inline int sockClose(int fd) {
#ifdef _WIN32
    return ::closesocket(fd);
#else
    return ::close(fd);
#endif
}

inline void sockSetNonBlock(int fd) {
#ifdef _WIN32
    u_long m = 1; ioctlsocket(fd, FIONBIO, &m);
#else
    int fl = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#endif
}

inline int sockErr() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

inline bool sockWouldBlock(int e) {
#ifdef _WIN32
    return e == WSAEWOULDBLOCK;
#else
    return e == EAGAIN || e == EWOULDBLOCK;
#endif
}

inline bool sockInProgress(int e) {
#ifdef _WIN32
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
    return e == EINPROGRESS;
#endif
}

inline bool sockInterrupted(int e) {
#ifdef _WIN32
    return e == WSAEINTR;
#else
    return e == EINTR;
#endif
}

// Human-readable name for a socket error code, for diagnostics ("recv failed:
// Connection reset by peer" tells RST from a graceful FIN from a timeout).
inline std::string sockErrStr(int e) {
#ifdef _WIN32
    switch (e) {
        case WSAECONNRESET:   return "connection reset by peer";
        case WSAECONNABORTED: return "connection aborted";
        case WSAETIMEDOUT:    return "connection timed out";
        case WSAENETRESET:    return "network reset";
        case WSAENOBUFS:      return "no buffer space";
        case WSAESHUTDOWN:    return "socket shut down";
        default: return "winsock error " + std::to_string(e);
    }
#else
    return std::strerror(e);
#endif
}

}  // namespace tak::net
