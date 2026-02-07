#include "socket_utils.h"
#include <cstring>
#include <unistd.h>
#include <stdexcept>

// SocketBase实现
SocketBase::SocketBase() : sock_fd_(-1), is_connected_(false) {
    memset(&peer_addr_, 0, sizeof(peer_addr_));
}

SocketBase::~SocketBase() { close(); }

void SocketBase::initAddr(const std::string& ip, uint16_t port) {
    peer_addr_.sin_family = AF_INET;
    peer_addr_.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &peer_addr_.sin_addr) <= 0) {
        throw std::invalid_argument("Invalid IP: " + ip);
    }
}

std::string SocketBase::getPeerIP() const {
    char ip_buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &peer_addr_.sin_addr, ip_buf, INET_ADDRSTRLEN);
    return ip_buf;
}

uint16_t SocketBase::getPeerPort() const { return ntohs(peer_addr_.sin_port); }

void SocketBase::close() {
    if (sock_fd_ >= 0) ::close(sock_fd_);
    sock_fd_ = -1;
    is_connected_ = false;
}

// TCPSocket实现
bool TCPSocket::connect(const std::string& ip, uint16_t port) {
    sock_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd_ < 0) return false;
    initAddr(ip, port);
    if (::connect(sock_fd_, (struct sockaddr*)&peer_addr_, sizeof(peer_addr_)) < 0) {
        close();
        return false;
    }
    is_connected_ = true;
    return true;
}

bool TCPSocket::listen(uint16_t port, int backlog) {
    sock_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd_ < 0) return false;
    struct sockaddr_in local_addr{};
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    local_addr.sin_port = htons(port);
    if (::bind(sock_fd_, (struct sockaddr*)&local_addr, sizeof(local_addr)) < 0 || ::listen(sock_fd_, backlog) < 0) {
        close();
        return false;
    }
    return true;
}

TCPSocket* TCPSocket::accept() {
    struct sockaddr_in client_addr{};
    socklen_t len = sizeof(client_addr);
    int fd = ::accept(sock_fd_, (struct sockaddr*)&client_addr, &len);
    if (fd < 0) return nullptr;
    auto sock = new TCPSocket();
    sock->sock_fd_ = fd;
    sock->peer_addr_ = client_addr;
    sock->is_connected_ = true;
    return sock;
}

ssize_t TCPSocket::sendData(const void* data, size_t len) {
    if (!is_connected_ || sock_fd_ < 0 || !data) return -1;
    size_t sent = 0;
    const char* buf = static_cast<const char*>(data);
    while (sent < len) {
        ssize_t ret = ::send(sock_fd_, buf + sent, len - sent, 0);
        if (ret < 0) return -1;
        sent += ret;
    }
    return sent;
}

ssize_t TCPSocket::recvData(void* buf, size_t buf_len) {
    if (!is_connected_ || sock_fd_ < 0 || !buf) return -1;
    size_t recved = 0;
    char* buffer = static_cast<char*>(buf);
    while (recved < buf_len) {
        ssize_t ret = ::recv(sock_fd_, buffer + recved, buf_len - recved, 0);
        if (ret <= 0) return ret < 0 ? -1 : recved;
        recved += ret;
    }
    return recved;
}

// UDPSocket实现
bool UDPSocket::bind(uint16_t port) {
    sock_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd_ < 0) return false;
    struct sockaddr_in local_addr{};
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    local_addr.sin_port = htons(port);
    if (::bind(sock_fd_, (struct sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
        close();
        return false;
    }
    return true;
}

void UDPSocket::setPeer(const std::string& ip, uint16_t port) { initAddr(ip, port); }

ssize_t UDPSocket::sendData(const void* data, size_t len) {
    if (sock_fd_ < 0 || !data) return -1;
    return ::sendto(sock_fd_, data, len, 0, (struct sockaddr*)&peer_addr_, sizeof(peer_addr_));
}

ssize_t UDPSocket::recvData(void* buf, size_t buf_len) {
    if (sock_fd_ < 0 || !buf) return -1;
    struct sockaddr_in sender_addr{};
    socklen_t len = sizeof(sender_addr);
    ssize_t ret = ::recvfrom(sock_fd_, buf, buf_len, 0, (struct sockaddr*)&sender_addr, &len);
    if (ret > 0) peer_addr_ = sender_addr;
    return ret;
}