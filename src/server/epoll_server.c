
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#define PORT 8080
#define MAX_EVENTS 10

int main() {
    int listen_fd, epoll_fd, nfds, conn_fd;
    struct epoll_event ev, events[MAX_EVENTS];
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    // 创建监听socket
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(PORT);
    bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
    listen(listen_fd, 5);

    // 创建epoll实例
    epoll_fd = epoll_create1(0);
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);

    while (1) {
        nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == listen_fd) {
                // 新连接
                conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
                ev.events = EPOLLIN | EPOLLET;
                ev.data.fd = conn_fd;
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn_fd, &ev);
            } else {
                // 数据读取
                char buffer[1024];
                int n = read(events[i].data.fd, buffer, sizeof(buffer));
                if (n > 0) {
                    write(events[i].data.fd, buffer, n);
                } else {
                    close(events[i].data.fd);
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, events[i].data.fd, NULL);
                }
            }
        }
    }
    close(listen_fd);
    close(epoll_fd);
    return 0;
}
