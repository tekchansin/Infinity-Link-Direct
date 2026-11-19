// Sockets on POSIX and on Windows (Winsock 2): the few calls the library makes, spelled once for both. Socket handles
// are kept as int everywhere (a Winsock SOCKET fits; INVALID_SOCKET becomes -1).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#ifndef SHUT_RDWR
#define SHUT_RDWR SD_BOTH
#endif
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ilink::net {

#ifdef _WIN32
inline void init() {
    static const bool ok = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
    (void)ok;
}
inline int closeSocket(int fd) { return closesocket((SOCKET)fd); }
inline std::string error() {
    char b[256] = "";
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, (DWORD)WSAGetLastError(), 0, b, sizeof b, nullptr);
    std::string s = b;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}
inline int pollIn(int fd, int ms) { WSAPOLLFD p{(SOCKET)fd, POLLRDNORM, 0}; return WSAPoll(&p, 1, ms); }
inline long sendBytes(int fd, const void* b, size_t n) { return send((SOCKET)fd, (const char*)b, (int)n, 0); }
inline long recvBytes(int fd, void* b, size_t n) { return recv((SOCKET)fd, (char*)b, (int)n, 0); }
inline int setTtl(int fd, int ttl) { DWORD v = (DWORD)ttl; return setsockopt((SOCKET)fd, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&v, sizeof v); }
// connect, giving up after ms (a blocking connect to a host that went away waited the system's ~20–75 s)
inline bool connectWithin(int fd, const sockaddr* a, size_t n, int ms) {
    u_long on = 1, off = 0;
    ioctlsocket((SOCKET)fd, FIONBIO, &on);
    int rc = ::connect((SOCKET)fd, a, (int)n);
    if (rc != 0 && WSAGetLastError() == WSAEWOULDBLOCK) {
        WSAPOLLFD p{(SOCKET)fd, POLLWRNORM, 0};
        int e = 0, l = sizeof e;
        rc = WSAPoll(&p, 1, ms) == 1 && getsockopt((SOCKET)fd, SOL_SOCKET, SO_ERROR, (char*)&e, &l) == 0 && !e ? 0 : -1;
    }
    ioctlsocket((SOCKET)fd, FIONBIO, &off);
    return rc == 0;
}
inline void recvTimeout(int fd, int ms) { DWORD t = (DWORD)ms; setsockopt((SOCKET)fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof t); }  // 0 = none
#else
inline void init() {}
inline int closeSocket(int fd) { return ::close(fd); }
inline std::string error() { return strerror(errno); }
inline int pollIn(int fd, int ms) { pollfd p{fd, POLLIN, 0}; return ::poll(&p, 1, ms); }
inline long sendBytes(int fd, const void* b, size_t n) {
#ifdef MSG_NOSIGNAL
    return ::send(fd, b, n, MSG_NOSIGNAL);
#else
    return ::send(fd, b, n, 0);  // SO_NOSIGPIPE is set on the socket instead (macOS)
#endif
}
inline long recvBytes(int fd, void* b, size_t n) { return ::recv(fd, b, n, 0); }
inline int setTtl(int fd, int ttl) { unsigned char v = (unsigned char)ttl; return setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &v, sizeof v); }
// connect, giving up after ms (a blocking connect to a host that went away waited the system's ~20–75 s)
inline bool connectWithin(int fd, const sockaddr* a, size_t n, int ms) {
    const int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int rc = ::connect(fd, a, (socklen_t)n);
    if (rc != 0 && errno == EINPROGRESS) {
        pollfd p{fd, POLLOUT, 0};
        int e = 0;
        socklen_t l = sizeof e;
        rc = ::poll(&p, 1, ms) == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &l) == 0 && !e ? 0 : -1;
    }
    fcntl(fd, F_SETFL, fl);
    return rc == 0;
}
inline void recvTimeout(int fd, int ms) { timeval t{ms / 1000, (ms % 1000) * 1000}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof t); }  // 0 = none
#endif

inline int setInt(int fd, int level, int name, int v) { return setsockopt(fd, level, name, (const char*)&v, sizeof v); }
inline void noSigpipe(int fd) {
#ifdef SO_NOSIGPIPE
    setInt(fd, SOL_SOCKET, SO_NOSIGPIPE, 1);
#else
    (void)fd;
#endif
}

// SPEC §9: a peer on a private network — IPv4 loopback, RFC 1918 (10/8, 172.16/12, 192.168/16), link-local 169.254/16;
// IPv6 loopback, ULA fc00::/7, link-local fe80::/10, or one of those IPv4 addresses mapped (::ffff:a.b.c.d)
inline bool privateAddress(const sockaddr* sa) {
    auto v4 = [](const uint8_t* p) {
        return p[0] == 10 || p[0] == 127 || (p[0] == 172 && (p[1] & 0xF0) == 16) || (p[0] == 192 && p[1] == 168) ||
               (p[0] == 169 && p[1] == 254);
    };
    if (!sa) return false;
    if (sa->sa_family == AF_INET) return v4((const uint8_t*)&((const sockaddr_in*)sa)->sin_addr);
    if (sa->sa_family != AF_INET6) return false;
    const uint8_t* p = (const uint8_t*)&((const sockaddr_in6*)sa)->sin6_addr;
    static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF}, loop[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    if (memcmp(p, mapped, 12) == 0) return v4(p + 12);
    return memcmp(p, loop, 16) == 0 || (p[0] & 0xFE) == 0xFC || (p[0] == 0xFE && (p[1] & 0xC0) == 0x80);
}

// every IPv4 interface that can carry mDNS: up, multicast, not loopback — its address and netmask (network order)
struct Iface { std::string name; in_addr addr{}, mask{}; };
inline std::vector<Iface> ipv4Ifaces() {
    std::vector<Iface> out;
#ifdef _WIN32
    ULONG size = 16384;
    std::string buf(size, '\0');
    auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(&buf[0]);
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr, a, &size) != NO_ERROR) return out;
    for (; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || (a->Flags & IP_ADAPTER_NO_MULTICAST)) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            Iface f;
            f.name = a->AdapterName;
            f.addr = ((sockaddr_in*)u->Address.lpSockaddr)->sin_addr;
            ULONG m = 0;
            ConvertLengthToIpv4Mask(u->OnLinkPrefixLength, &m);
            f.mask.s_addr = m;
            out.push_back(f);
        }
    }
#else
    ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) != 0) return out;
    for (ifaddrs* i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK) ||
            !(i->ifa_flags & IFF_MULTICAST))
            continue;
        Iface f;
        f.name = i->ifa_name;
        f.addr = ((sockaddr_in*)i->ifa_addr)->sin_addr;
        if (i->ifa_netmask) f.mask = ((sockaddr_in*)i->ifa_netmask)->sin_addr;
        out.push_back(f);
    }
    freeifaddrs(ifs);
#endif
    return out;
}

// the first IPv4 of an interface that is up and not loopback (Windows: one with a gateway — the LAN, not a VPN or a
// virtual switch)
inline std::string localIPv4() {
    std::string ip;
#ifdef _WIN32
    ULONG size = 16384;
    std::string buf(size, '\0');
    auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(&buf[0]);
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr, a, &size) != NO_ERROR) return ip;
    for (; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || !a->FirstUnicastAddress) continue;
        char b[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &((sockaddr_in*)a->FirstUnicastAddress->Address.lpSockaddr)->sin_addr, b, sizeof b);
        if (ip.empty() || a->FirstGatewayAddress) ip = b;
        if (a->FirstGatewayAddress) break;
    }
#else
    ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) != 0) return ip;
    for (ifaddrs* i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
        char b[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &((sockaddr_in*)i->ifa_addr)->sin_addr, b, sizeof b);
        ip = b;
        break;
    }
    freeifaddrs(ifs);
#endif
    return ip;
}

// The calling thread's name, for ps / top / a profiler (Linux keeps 15 characters). Never on a process's first thread:
// its name is the process's own (pidof, pkill -x).
inline void nameThread(const char* n) {
#if defined(_WIN32)
    (void)n;
#elif defined(__APPLE__)
    pthread_setname_np(n);
#else
    pthread_setname_np(pthread_self(), n);
#endif
}

}  // namespace ilink::net
