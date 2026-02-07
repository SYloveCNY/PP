#ifndef SOCKET_UTILS_H
#define SOCKET_UTILS_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string>
#include <cstdint>
#include <vector>

// 消息类型枚举
enum class MsgType : uint8_t {
    TEXT = 0,
    IMAGE = 1,
    FILE = 2
};

// 传输方式枚举
enum class TransportType : uint8_t {
    TCP = 0,
    UDP = 1
};

// 协议头（注意字节序转换）
struct MsgHeader {
    uint16_t magic_num;    // 0x5A5A
    MsgType msg_type;      
    TransportType transport;
    uint16_t meta_len;     
    uint32_t data_len;     

    // 转网络字节序（大端）
    void toNetworkOrder() {
        magic_num = htons(magic_num);
        meta_len = htons(meta_len);
        data_len = htonl(data_len);
    }

    // 转主机字节序
    void toHostOrder() {
        magic_num = ntohs(magic_num);
        meta_len = ntohs(meta_len);
        data_len = ntohl(data_len);
    }
};

// Socket基础类（抽象通用方法）
class SocketBase {
protected:
    int sock_fd_;          
    struct sockaddr_in peer_addr_; 
    bool is_connected_;    

    void initAddr(const std::string& ip, uint16_t port);
public:
    SocketBase();
    virtual ~SocketBase();
    std::string getPeerIP() const;
    uint16_t getPeerPort() const;
    virtual ssize_t sendData(const void* data, size_t len) = 0;
    virtual ssize_t recvData(void* buf, size_t buf_len) = 0;
    virtual void close();
};

// TCP Socket子类（可靠传输）
class TCPSocket : public SocketBase {
public:
    bool connect(const std::string& ip, uint16_t port);
    bool listen(uint16_t port, int backlog = 5);
    TCPSocket* accept();
    ssize_t sendData(const void* data, size_t len) override;
    ssize_t recvData(void* buf, size_t buf_len) override;
};

// UDP Socket子类（低延迟）
class UDPSocket : public SocketBase {
public:
    bool bind(uint16_t port);
    void setPeer(const std::string& ip, uint16_t port);
    ssize_t sendData(const void* data, size_t len) override;
    ssize_t recvData(void* buf, size_t buf_len) override;
};

#endif // SOCKET_UTILS_H