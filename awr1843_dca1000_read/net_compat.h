#pragma once
// net_compat.h — 跨平台 socket 兼容薄层
//
// 目的：把 Windows(winsock2) 与 POSIX(BSD socket) 的差异收口到一个头文件，
// 让 UdpReceiver / UDPController 用同一套名字编译。纯类型/API 映射，无业务逻辑。
//
// 设计约定：
//   * SOCKET / INVALID_SOCKET / SOCKET_ERROR 三个名字两端统一（Windows 原生就有，
//     POSIX 侧用 typedef/constexpr 补齐——注意用 typedef 而非 #define，避免与
//     其它头文件里 `int` 展开冲突）。
//   * 统一辅助函数：net_startup/net_cleanup(仅 Windows 有意义)、net_close、
//     net_last_error、net_would_block、net_set_nonblocking。
//   * 不改变任何收发/重组语义，只替换平台相关调用。

#ifdef _WIN32
    #ifndef _WINSOCK_DEPRECATED_NO_WARNINGS
        #define _WINSOCK_DEPRECATED_NO_WARNINGS
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>

    // POSIX 侧补齐 winsock 的名字。用 typedef/constexpr，不用 #define SOCKET int，
    // 以免多个头文件重复展开出 `typedef int int;` 之类的冲突。
    typedef int SOCKET;
    static constexpr SOCKET INVALID_SOCKET = -1;
    static constexpr int    SOCKET_ERROR   = -1;
#endif

#include <cstdint>

namespace netcompat {

// Winsock 需要 WSAStartup/WSACleanup；POSIX 上是空操作。
// 返回 true 表示可用。多次调用安全（Windows 侧 winsock 内部有引用计数）。
inline bool net_startup() {
#ifdef _WIN32
    WSADATA wsaData;
    return WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
#else
    return true;
#endif
}

inline void net_cleanup() {
#ifdef _WIN32
    WSACleanup();
#endif
}

// 关闭 socket，跨平台。
inline int net_close(SOCKET s) {
#ifdef _WIN32
    return closesocket(s);
#else
    return ::close(s);
#endif
}

// 取最近一次 socket 错误码，跨平台。
inline int net_last_error() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

// 判断错误码是否为“非阻塞下暂无数据”（EWOULDBLOCK/WSAEWOULDBLOCK）。
inline bool net_would_block(int err) {
#ifdef _WIN32
    return err == WSAEWOULDBLOCK;
#else
    return err == EWOULDBLOCK || err == EAGAIN;
#endif
}

// 设置 socket 为非阻塞/阻塞。成功返回 true。
inline bool net_set_nonblocking(SOCKET s, bool nonblocking) {
#ifdef _WIN32
    unsigned long mode = nonblocking ? 1UL : 0UL;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags == -1) return false;
    if (nonblocking) flags |= O_NONBLOCK;
    else             flags &= ~O_NONBLOCK;
    return fcntl(s, F_SETFL, flags) != -1;
#endif
}

} // namespace netcompat
