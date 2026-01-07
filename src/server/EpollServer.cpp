#include <sys/epoll.h>
#include <unistd.h>
#include <vector>
#include <map>
#include "protocol.h"

class EpollServer {
private:
    int epollFd;
    int listenFd;
    std::map<int, ClientConnection> clients; // socket fd -> 客户端连接
    static const int MAX_EVENTS = 1024;
    
public:
    EpollServer(int port) {
        // 创建epoll实例
        epollFd = epoll_create1(0);
        if (epollFd == -1) {
            perror("epoll_create1");
            exit(EXIT_FAILURE);
        }
        
        // 创建监听socket并添加到epoll
        listenFd = createListenSocket(port);
        addFdToEpoll(listenFd, EPOLLIN);
    }
    
    void run() {
        struct epoll_event events[MAX_EVENTS];
        while (true) {
            int nfds = epoll_wait(epollFd, events, MAX_EVENTS, -1);
            if (nfds == -1) {
                perror("epoll_wait");
                continue;
            }
            
            for (int i = 0; i < nfds; ++i) {
                if (events[i].data.fd == listenFd) {
                    // 处理新连接
                    handleNewConnection();
                } else if (events[i].events & EPOLLIN) {
                    // 处理可读事件
                    handleReadEvent(events[i].data.fd);
                } else if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                    // 处理连接关闭或错误
                    handleDisconnect(events[i].data.fd);
                }
            }
        }
    }
    
private:
    void addFdToEpoll(int fd, uint32_t events) {
        struct epoll_event ev;
        ev.events = events;
        ev.data.fd = fd;
        if (epoll_ctl(epollFd, EPOLL_CTL_ADD, fd, &ev) == -1) {
            perror("epoll_ctl: add");
            exit(EXIT_FAILURE);
        }
    }
    
    void handleNewConnection() {
        // 接受新连接并添加到epoll
        struct sockaddr_in clientAddr;
        socklen_t clientLen = sizeof(clientAddr);
        int connFd = accept(listenFd, (struct sockaddr*)&clientAddr, &clientLen);
        if (connFd == -1) {
            perror("accept");
            return;
        }
        
        // 设置非阻塞模式
        setNonBlocking(connFd);
        addFdToEpoll(connFd, EPOLLIN | EPOLLET);
        
        // 初始化客户端连接
        clients[connFd] = ClientConnection(connFd, clientAddr);
    }
    
    void handleReadEvent(int fd) {
        // 读取并处理客户端数据
        auto it = clients.find(fd);
        if (it == clients.end()) return;
        
        ClientConnection& client = it->second;
        ssize_t n = client.readData();
        
        if (n <= 0) {
            // 连接关闭
            handleDisconnect(fd);
            return;
        }
        
        // 处理完整的数据包
        while (client.hasCompletePacket()) {
            Packet packet = client.extractPacket();
            processPacket(fd, packet);
        }
    }
    
    void handleDisconnect(int fd) {
        // 处理客户端断开连接
        auto it = clients.find(fd);
        if (it != clients.end()) {
            int userId = it->second.getUserId();
            std::string nickname = it->second.getNickname();
            
            // 通知其他用户该用户下线
            notifyUserOffline(userId, nickname);
            
            clients.erase(it);
        }
        close(fd);
        epoll_ctl(epollFd, EPOLL_CTL_DEL, fd, nullptr);
    }

    void checkHeartbeats() {
        time_t now = time(nullptr);
        for (auto it = clients.begin(); it != clients.end(); ) {
            if (now - it->second.getLastActiveTime() > 15) { // 15秒未收到心跳
                // 认为客户端已下线
                handleDisconnect(it->first);
                it = clients.erase(it);
            } else {
                ++it;
            }
        }
    }
};