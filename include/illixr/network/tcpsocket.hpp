#pragma once
#if defined(_WIN32) || defined(_WIN64)
    #define _WINSOCKAPI_
    #define WIN32_LEAN_AND_MEAN
#endif
#include <stdexcept>
#if defined(_WIN32) || defined(_WIN64)
// clang-format off
    #include <WinSock2.h>  // Must come FIRST
    #include <ws2def.h>
    #include <ws2tcpip.h>
    #include <mstcpip.h>
    #include <WindNS.h>
    #include <iphlpapi.h>
    #include <icmpapi.h>
    #include <nldef.h>
// clang-format on
    #pragma comment(lib, "Ws2_32.lib")
    #pragma comment(lib, "Iphlpapi.lib")
    #define BYTE_TYPE   int
    #define SOCKET_TYPE SOCKET
#else
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #if defined(__linux__)
// The kernel UAPI struct tcp_info (linux/tcp.h) has the RFC4898 byte-count fields
// (tcpi_bytes_sent/tcpi_bytes_received) used by mptcp_subflow_stats() below; glibc's own
// netinet/tcp.h ships an older, smaller struct tcp_info under the same name, so only one of
// the two headers can be included.
        #include <linux/tcp.h>
    #else
        #include <netinet/tcp.h>
    #endif
    #include <sys/socket.h>
    #include <unistd.h>
    #define BYTE_TYPE   ssize_t
    #define SOCKET_TYPE int
#endif

#if defined(__linux__)
    #include <linux/mptcp.h>
#endif

#include "illixr/export.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Only meaningful on Linux, but defined unconditionally so TCPSocket::Protocol::MPTCP
// compiles everywhere; callers must not request it on non-Linux platforms.
#if !defined(IPPROTO_MPTCP)
    #define IPPROTO_MPTCP 262
#endif

namespace ILLIXR::network {

#if defined(__linux__)
// Per-subflow snapshot returned by TCPSocket::mptcp_subflow_stats(). Byte counters are
// cumulative since the subflow was established (as reported by the kernel's tcp_info).
struct MptcpSubflowStats {
    std::string local_addr;  // "ip:port"
    std::string remote_addr; // "ip:port"
    uint64_t    bytes_sent;
    uint64_t    bytes_received;
};
#endif

class MY_EXPORT_API TCPSocket {
public:
    // Underlying transport protocol to request from the OS. DEFAULT lets the OS pick
    // the default protocol for SOCK_STREAM (plain TCP).
    enum class Protocol { DEFAULT = 0, MPTCP = IPPROTO_MPTCP };

    explicit TCPSocket(Protocol protocol = Protocol::DEFAULT) {
#if defined(_WIN32) || defined(_WIN64)
        static bool initialized = false;
        if (!initialized) {
            WSAData wsa_data;
            if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
                throw std::runtime_error("WSAStartup failed.");
            }
            initialized = true;
        }
#endif
        fd_ = socket(AF_INET, SOCK_STREAM, static_cast<int>(protocol));
#if defined(_WIN32) || defined(_WIN64)
        if (fd_ == INVALID_SOCKET)
            throw std::runtime_error("startup failed");
#else
        if (fd_ < 0) {
            throw std::runtime_error(std::string("socket() failed: ") + strerror(errno));
        }
#endif
    }

    [[maybe_unused]] explicit TCPSocket(int fd) {
        fd_ = fd;
    }

    // Destructor
    // Close the file descriptor
    ~TCPSocket() {
#if defined(_WIN32) || defined(_WIN64)
        closesocket(fd_);
#else
        close(fd_);
#endif
    }

    // Bind socket to a specified local ip and port
    void socket_bind(const std::string& ip, int port) const {
        sockaddr_in local_addr;
        local_addr.sin_family      = AF_INET;
        local_addr.sin_port        = htons(port);
        local_addr.sin_addr.s_addr = inet_addr(ip.c_str());
        if (bind(fd_, (struct sockaddr*) &local_addr, sizeof(local_addr)) < 0) {
            throw std::runtime_error("Bind failed");
        }
    }

    // Listen for a connection. It is typically called from the server socket.
    void socket_listen(const int backlog = 16) const {
        if (listen(fd_, backlog) < 0) {
            throw std::runtime_error("Listen failed");
        }
    }

    // Connect the socket to a specified peer addr
    void socket_connect(const std::string& ip, int port) const {
        sockaddr_in peer_addr;
        peer_addr.sin_family      = AF_INET;
        peer_addr.sin_port        = htons(port);
        peer_addr.sin_addr.s_addr = inet_addr(ip.c_str());
        if (connect(fd_, (struct sockaddr*) &peer_addr, sizeof(peer_addr)) < 0)
            throw std::runtime_error("Connect failed");
    }

    // Accept connect from the client. It is typically called from the server socket.
#if defined(_WIN32) || defined(_WIN64)
    SOCKET
#else
    int
#endif
    socket_accept() const {
#if defined(_WIN32) || defined(_WIN64)
        SOCKET
#else
        int
#endif
        fd = accept(fd_, nullptr, nullptr);
#if defined(_WIN32) || defined(_WIN64)
        if (fd == INVALID_SOCKET)
            throw std::runtime_error("Accept failed");
#endif
        return fd;
    }

    // Allow the reuse of local addresses
    void socket_set_reuseaddr() const {
#if defined(_WIN32) || defined(_WIN64)
        int enable = 1;
        if (int ret = setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, (char*) &enable, sizeof(enable)) < 0) {
            int err = WSAGetLastError();
            throw std::runtime_error("Reuseaddr failed");
        }
#else
        const int enable = 1;
        setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int));
#endif
    }

    // Read data from the socket
    [[nodiscard]] std::string read_data(const size_t limit = BUFFER_SIZE) const {
        char      buffer[BUFFER_SIZE];
        BYTE_TYPE bytes_read =
#if defined(_WIN32) || defined(_WIN64)
            recv(fd_, buffer, static_cast<int>(min(BUFFER_SIZE, limit)), 0);
#else
            read(fd_, buffer, static_cast<int>(std::min(BUFFER_SIZE, limit)));
#endif
        if (bytes_read <= 0)
            return {};
        return std::string(buffer, bytes_read);
    }

    // Write data to the socket
    void write_data(const std::string& buffer) {
        auto it = buffer.begin();
        do {
            it = write_helper(it, buffer.end());
        } while (it != buffer.end());
    }

    // Disable naggle algorithm. This allows socket to flush data immediately after calling write().
    void enable_no_delay() const {
#if defined(_WIN32) || defined(_WIN64)
        int enable = 1;
        if (int ret = setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, (char*) &enable, sizeof(enable)) < 0)
            throw std::runtime_error("No delay failed");
#else
        const int enable = 1;
        setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(int));
#endif
    }

    /* accessors */
    [[maybe_unused]] [[nodiscard]] std::string local_address() const {
        sockaddr_in local_address;
        socklen_t   size = sizeof(local_address);
        getsockname(fd_, (struct sockaddr*) &local_address, &size);
        return sockaddr_in_to_string(local_address);
    }

    [[nodiscard]] std::string peer_address() const {
        sockaddr_in peer_address;
        socklen_t   size = sizeof(peer_address);
        getpeername(fd_, (struct sockaddr*) &peer_address, &size);
        return sockaddr_in_to_string(peer_address);
    }

#if defined(__linux__)
    // Query per-subflow statistics via getsockopt(SOL_MPTCP, MPTCP_FULL_INFO) (kernel >=
    // 5.19). Returns an empty vector if this isn't an MPTCP socket, the kernel doesn't
    // support the call, or the connection hasn't been established yet.
    [[nodiscard]] std::vector<MptcpSubflowStats> mptcp_subflow_stats() const {
        constexpr uint32_t MAX_SUBFLOWS = 8;

        mptcp_subflow_info sf_info[MAX_SUBFLOWS]{};
        tcp_info            tcp_infos[MAX_SUBFLOWS]{};

        mptcp_full_info full_info{};
        full_info.size_tcpinfo_user = sizeof(tcp_info);
        full_info.size_sfinfo_user  = sizeof(mptcp_subflow_info);
        full_info.size_arrays_user  = MAX_SUBFLOWS;
        full_info.subflow_info      = reinterpret_cast<__aligned_u64>(sf_info);
        full_info.tcp_info          = reinterpret_cast<__aligned_u64>(tcp_infos);

        socklen_t len = sizeof(full_info);
        if (getsockopt(fd_, SOL_MPTCP, MPTCP_FULL_INFO, &full_info, &len) < 0) {
            return {};
        }

        std::vector<MptcpSubflowStats> result;
        uint32_t                       count = std::min(full_info.num_subflows, MAX_SUBFLOWS);
        result.reserve(count);
        for (uint32_t i = 0; i < count; i++) {
            MptcpSubflowStats stats;
            stats.local_addr     = sockaddr_in_to_string(sf_info[i].addrs.sin_local);
            stats.remote_addr    = sockaddr_in_to_string(sf_info[i].addrs.sin_remote);
            stats.bytes_sent     = tcp_infos[i].tcpi_bytes_sent;
            stats.bytes_received = tcp_infos[i].tcpi_bytes_received;
            result.push_back(std::move(stats));
        }
        return result;
    }
#endif

private:
    [[nodiscard]] static std::string sockaddr_in_to_string(const sockaddr_in& addr) {
#if defined(_WIN32) || defined(_WIN64)
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
#else
        return std::string(inet_ntoa(addr.sin_addr)) + ":" + std::to_string(ntohs(addr.sin_port));
#endif
    }

    [[nodiscard]] std::string::const_iterator write_helper(const std::string::const_iterator& begin,
                                                           const std::string::const_iterator& end) const {
        BYTE_TYPE bytes_written =
#if defined(_WIN32) || defined(_WIN64)
            send(fd_, &*begin, static_cast<int>(end - begin), 0);
        if (bytes_written < 0)
            throw std::runtime_error("Write failed");
#else
            write(fd_, &*begin, end - begin);
#endif
        return begin + bytes_written;
    }

    SOCKET_TYPE fd_;
    /* maximum size of a read */
    static constexpr size_t BUFFER_SIZE = 1024 * 256;
};

} // namespace ILLIXR::network
