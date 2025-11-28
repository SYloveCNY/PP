#include <iostream>
#include <vector>
#include <map>
#include <string>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <mutex>
#include <errno.h>
#include <chrono>
#include <thread>
#include "protocol.h"

using namespace std;
using namespace chrono;

// 服务端配置常量
const int MAX_EVENTS = 1024;        // epoll最大监听事件数
const int BUF_SIZE = 4096;          // 缓冲区大小
const int SERVER_PORT = 8888;       // 服务端监听端口（登录/通知）
const int HEARTBEAT_TIMEOUT = 30;   // 心跳超时时间（30秒，超时强制下线）
const int EPOLL_WAIT_TIMEOUT = 5000;// epoll_wait超时（5秒，用于心跳检测调度）

// 全局共享数据（需互斥锁保护）
mutex g_mutex;                          // 全局互斥锁
map<int, UserInfo> g_online_users;      // 在线用户列表（user_id -> UserInfo）
map<int, string> g_client_ips;          // 客户端FD -> 客户端IP地址
map<int, int> g_fd_to_user_id;          // 客户端FD -> 用户ID（快速查找）
int g_next_user_id = 1;                 // 下一个分配的用户ID（自增，保证唯一）

// 设置socket为非阻塞模式（用于epoll边缘触发）
void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL failed");
        return;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL failed");
    }
}

// 发送完整数据包（确保头部+数据完整发送，避免部分发送）
bool send_packet(int fd, MsgType msg_type, const vector<char>& data) {
    PacketHeader header;
    header.msg_type = msg_type;
    header.data_len = data.size();

    // 先发送头部（固定大小）
    if (send(fd, &header, sizeof(PacketHeader), 0) != sizeof(PacketHeader)) {
        perror("send header failed");
        return false;
    }

    // 再发送数据（循环发送直到全部发送完成）
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t ret = send(fd, data.data() + sent, data.size() - sent, 0);
        if (ret <= 0) {
            perror("send data failed");
            return false;
        }
        sent += ret;
    }
    return true;
}

// 广播消息给所有在线用户（支持排除指定FD，避免自己收到自己的通知）
void broadcast_packet(MsgType msg_type, const vector<char>& data, int exclude_fd = -1) {
    lock_guard<mutex> lock(g_mutex);
    for (auto& [user_id, user] : g_online_users) {
        if (user.manage_port != exclude_fd) { // 排除指定客户端FD
            if (!send_packet(user.manage_port, msg_type, data)) {
                cerr << "广播消息给用户 [" << user.nickname << "]（ID:" << user_id << "）失败" << endl;
            }
        }
    }
}

// 处理客户端登录请求
void handle_login_req(int client_fd, const vector<char>& data) {
    LoginReq req;
    size_t offset = 0;

    // 反序列化昵称
    req.nickname = deserialize_string(data, offset);
    // 反序列化头像
    req.avatar = deserialize_vector(data, offset);
    // 反序列化数据传输端口
    if (offset + sizeof(uint16_t) > data.size()) {
        cerr << "handle_login_req: 数据长度不足，无法读取data_port" << endl;
        return;
    }
    memcpy(&req.data_port, data.data() + offset, sizeof(uint16_t));
    offset += sizeof(uint16_t);

    // 构造用户信息
    UserInfo user;
    user.user_id = g_next_user_id++;          // 分配唯一用户ID
    user.nickname = req.nickname;             // 昵称
    user.avatar = req.avatar;                 // 头像
    user.manage_port = client_fd;             // 与服务端通信的FD
    user.data_port = req.data_port;           // 客户端点对点数据端口
    user.last_heartbeat = system_clock::now();// 初始化心跳时间

    // 获取客户端IP地址
    {
        lock_guard<mutex> lock(g_mutex);
        auto ip_it = g_client_ips.find(client_fd);
        if (ip_it != g_client_ips.end()) {
            user.ip = ip_it->second;
        } else {
            user.ip = "unknown";
            cerr << "警告：未找到客户端FD=" << client_fd << "的IP地址" << endl;
        }
    }

    // 保存用户信息到全局变量
    {
        lock_guard<mutex> lock(g_mutex);
        g_online_users[user.user_id] = user;
        g_fd_to_user_id[client_fd] = user.user_id;
    }

    // 发送登录响应
    LoginRsp rsp;
    rsp.success = true;
    rsp.user_id = user.user_id;
    rsp.msg = "登录成功！欢迎使用聊天系统～";
    vector<char> rsp_data = serialize_login_rsp(rsp);
    if (!send_packet(client_fd, MsgType::LOGIN_RSP, rsp_data)) {
        cerr << "发送登录响应给用户 [" << user.nickname << "] 失败" << endl;
    } else {
        cout << "用户 [" << user.nickname << "]（ID:" << user.user_id << "）登录成功，IP:" << user.ip << endl;
    }

    // 广播新用户上线通知（排除新用户自己）
    vector<char> notify_data;
    auto nickname_data = serialize_string(user.nickname);
    auto avatar_data = serialize_vector(user.avatar);
    auto ip_data = serialize_string(user.ip);
    // 拼接通知数据：昵称 + 头像 + IP + user_id + data_port
    notify_data.insert(notify_data.end(), nickname_data.begin(), nickname_data.end());
    notify_data.insert(notify_data.end(), avatar_data.begin(), avatar_data.end());
    notify_data.insert(notify_data.end(), ip_data.begin(), ip_data.end());
    notify_data.insert(notify_data.end(), (char*)&user.user_id, (char*)&user.user_id + sizeof(int));
    notify_data.insert(notify_data.end(), (char*)&user.data_port, (char*)&user.data_port + sizeof(uint16_t));
    broadcast_packet(MsgType::USER_ONLINE_NOTIFY, notify_data, client_fd);
}

// 处理客户端的在线用户列表请求
void handle_user_list_req(int client_fd) {
    cout << "收到客户端FD=" << client_fd << "的在线用户列表请求" << endl;
    lock_guard<mutex> lock(g_mutex);
    vector<char> list_data;

    // 序列化在线用户数量
    size_t user_count = g_online_users.size();
    list_data.resize(sizeof(size_t));
    memcpy(list_data.data(), &user_count, sizeof(size_t));

    // 序列化每个用户的详细信息
    for (auto& [user_id, user] : g_online_users) {
        auto nickname_data = serialize_string(user.nickname);
        auto avatar_data = serialize_vector(user.avatar);
        auto ip_data = serialize_string(user.ip);
        // 拼接用户数据：昵称 + 头像 + IP + user_id + data_port
        list_data.insert(list_data.end(), nickname_data.begin(), nickname_data.end());
        list_data.insert(list_data.end(), avatar_data.begin(), avatar_data.end());
        list_data.insert(list_data.end(), ip_data.begin(), ip_data.end());
        list_data.insert(list_data.end(), (char*)&user.user_id, (char*)&user.user_id + sizeof(int));
        list_data.insert(list_data.end(), (char*)&user.data_port, (char*)&user.data_port + sizeof(uint16_t));
    }

    // 发送用户列表响应
    if (!send_packet(client_fd, MsgType::USER_LIST_RSP, list_data)) {
        cerr << "发送在线用户列表给FD=" << client_fd << "失败" << endl;
    } else {
        cout << "已发送在线用户列表给FD=" << client_fd << "，共" << user_count << "个用户" << endl;
    }
}

// 处理客户端断开连接（主动退出/网络异常/心跳超时）
void handle_client_disconnect(int client_fd) {
    int offline_user_id = -1;
    string offline_nickname = "unknown";

    // 清理全局变量中的用户数据
    {
        lock_guard<mutex> lock(g_mutex);
        // 通过FD查找用户ID
        auto fd_it = g_fd_to_user_id.find(client_fd);
        if (fd_it != g_fd_to_user_id.end()) {
            offline_user_id = fd_it->second;
            g_fd_to_user_id.erase(fd_it);
        }
        // 移除客户端IP记录
        g_client_ips.erase(client_fd);
        // 查找并移除在线用户
        auto user_it = g_online_users.find(offline_user_id);
        if (user_it != g_online_users.end()) {
            offline_nickname = user_it->second.nickname;
            g_online_users.erase(user_it);
        }
    }

    // 广播用户下线通知
    if (offline_user_id != -1) {
        vector<char> notify_data;
        // 拼接通知数据：user_id + 昵称
        notify_data.resize(sizeof(int) + offline_nickname.size());
        memcpy(notify_data.data(), &offline_user_id, sizeof(int));
        memcpy(notify_data.data() + sizeof(int), offline_nickname.data(), offline_nickname.size());
        broadcast_packet(MsgType::USER_OFFLINE_NOTIFY, notify_data, client_fd);

        cout << "用户 [" << offline_nickname << "]（ID:" << offline_user_id << "）下线" << endl;
    } else {
        cout << "客户端FD=" << client_fd << "断开连接（未找到对应用户）" << endl;
    }

    // 关闭客户端FD
    close(client_fd);
}

// 处理客户端发送的心跳包（更新心跳时间戳）
void handle_heartbeat(int client_fd) {
    lock_guard<mutex> lock(g_mutex);
    // 通过FD查找用户ID
    auto fd_it = g_fd_to_user_id.find(client_fd);
    if (fd_it != g_fd_to_user_id.end()) {
        int user_id = fd_it->second;
        // 查找在线用户并更新心跳时间
        auto user_it = g_online_users.find(user_id);
        if (user_it != g_online_users.end()) {
            user_it->second.last_heartbeat = system_clock::now();
            // 调试日志（可选关闭）
            // cout << "收到用户 [" << user_it->second.nickname << "]（ID:" << user_id << "）的心跳包" << endl;
        }
    }
}

// 心跳检测线程（定期清理超时未发心跳的用户）
void heartbeat_check_thread() {
    while (true) {
        // 每10秒检测一次
        this_thread::sleep_for(seconds(10));
        lock_guard<mutex> lock(g_mutex);

        auto now = system_clock::now();
        vector<int> offline_user_ids; // 存储超时需要下线的用户ID

        // 遍历所有在线用户，检查心跳超时
        for (auto& [user_id, user] : g_online_users) {
            duration<double> diff = now - user.last_heartbeat;
            if (diff.count() > HEARTBEAT_TIMEOUT) {
                // 心跳超时，标记为下线
                offline_user_ids.push_back(user_id);
                cout << "用户 [" << user.nickname << "]（ID:" << user_id << "）心跳超时（" 
                     << diff.count() << "秒），强制下线" << endl;
            }
        }

        // 清理超时用户（关闭连接+更新全局变量）
        for (int user_id : offline_user_ids) {
            auto user_it = g_online_users.find(user_id);
            if (user_it != g_online_users.end()) {
                int client_fd = user_it->second.manage_port;
                // 关闭客户端FD
                close(client_fd);
                // 移除FD->user_id映射
                g_fd_to_user_id.erase(client_fd);
                // 移除客户端IP记录
                g_client_ips.erase(client_fd);
                // 移除在线用户
                g_online_users.erase(user_id);

                // 广播下线通知
                vector<char> notify_data;
                notify_data.resize(sizeof(int) + user_it->second.nickname.size());
                memcpy(notify_data.data(), &user_id, sizeof(int));
                memcpy(notify_data.data() + sizeof(int), user_it->second.nickname.data(), user_it->second.nickname.size());
                broadcast_packet(MsgType::USER_OFFLINE_NOTIFY, notify_data, client_fd);
            }
        }
    }
}

// 处理普通消息
void handle_common_msg(int client_fd, const vector<char>& data) {
    (void)client_fd;  // 告诉编译器该参数有意未使用

    size_t offset = 0;
    CommonMsg msg;

    // 反序列化from_user_id
    if (offset + sizeof(int) > data.size()) {
        cerr << "handle_common_msg: 缺少from_user_id" << endl;
        return;
    }
    memcpy(&msg.from_user_id, data.data() + offset, sizeof(int));
    offset += sizeof(int);

    // 反序列化to_user_id
    if (offset + sizeof(int) > data.size()) {
        cerr << "handle_common_msg: 缺少to_user_id" << endl;
        return;
    }
    memcpy(&msg.to_user_id, data.data() + offset, sizeof(int));
    offset += sizeof(int);

    // 反序列化from_nickname
    msg.from_nickname = deserialize_string(data, offset);
    // 反序列化content
    msg.content = deserialize_string(data, offset);

    // 查找目标用户并转发消息
    lock_guard<mutex> lock(g_mutex);
    auto to_user_it = g_online_users.find(msg.to_user_id);
    if (to_user_it == g_online_users.end()) {
        cerr << "目标用户ID=" << msg.to_user_id << "不在线，消息转发失败" << endl;
        return;
    }

    // 直接转发消息给目标用户
    if (!send_packet(to_user_it->second.manage_port, MsgType::COMMON_MSG, data)) {
        cerr << "转发消息给用户ID=" << msg.to_user_id << "失败" << endl;
    } else {
        cout << "用户ID=" << msg.from_user_id << "向ID=" << msg.to_user_id << "发送消息：" << msg.content << endl;
    }
}

// 读取完整数据包（处理粘包，支持非阻塞模式）
bool recv_complete_packet(int fd, PacketHeader& header, vector<char>& data, bool& is_eagain) {
    is_eagain = false;

    // 第一步：读取数据包头部
    ssize_t ret = recv(fd, &header, sizeof(PacketHeader), 0);
    if (ret == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // 非阻塞模式下暂时无数据，属于正常情况
            is_eagain = true;
            return false;
        } else {
            perror("recv header failed");
            return false;
        }
    } else if (ret != sizeof(PacketHeader)) {
        // 头部不完整（连接可能已断开）
        cerr << "recv header incomplete: 期望" << sizeof(PacketHeader) << "字节，实际" << ret << "字节" << endl;
        return false;
    }

    // 第二步：读取数据部分
    data.resize(header.data_len);
    size_t recved = 0;
    while (recved < header.data_len) {
        ret = recv(fd, data.data() + recved, header.data_len - recved, 0);
        if (ret == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 暂时无数据，后续再读
                is_eagain = true;
                return false;
            } else {
                perror("recv data failed");
                return false;
            }
        } else if (ret == 0) {
            // 客户端主动关闭连接
            cerr << "client fd=" << fd << " closed connection while receiving data" << endl;
            return false;
        }
        recved += ret;
    }

    return true;
}

int main() {
    // 1. 创建服务端监听socket（TCP）
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket create failed");
        return -1;
    }

    // 2. 设置端口复用（避免端口占用报错）
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        perror("setsockopt failed");
        close(server_fd);
        return -1;
    }

    // 3. 绑定端口和IP
    sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;         // IPv4
    server_addr.sin_addr.s_addr = INADDR_ANY; // 监听所有网卡IP
    server_addr.sin_port = htons(SERVER_PORT); // 端口转换为网络字节序
    if (bind(server_fd, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        close(server_fd);
        return -1;
    }

    // 4. 开始监听（最大等待队列长度10）
    if (listen(server_fd, 10) < 0) {
        perror("listen failed");
        close(server_fd);
        return -1;
    }

    cout << "=====================================" << endl;
    cout << "聊天服务端启动成功！" << endl;
    cout << "监听端口：" << SERVER_PORT << endl;
    cout << "心跳超时时间：" << HEARTBEAT_TIMEOUT << "秒" << endl;
    cout << "=====================================" << endl;

    // 5. 启动心跳检测线程（后台运行）
    thread heartbeat_thread(heartbeat_check_thread);
    heartbeat_thread.detach();

    // 6. 创建epoll实例（I/O多路复用）
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("epoll create failed");
        close(server_fd);
        return -1;
    }

    // 7. 将服务端监听socket添加到epoll
    epoll_event ev;
    ev.data.fd = server_fd;
    ev.events = EPOLLIN | EPOLLET; // 边缘触发模式（高效）
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) < 0) {
        perror("epoll_ctl add server_fd failed");
        close(epoll_fd);
        close(server_fd);
        return -1;
    }

    // 8. 循环处理epoll事件
    epoll_event events[MAX_EVENTS];
    while (true) {
        // 等待事件触发（超时时间5秒）
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, EPOLL_WAIT_TIMEOUT);
        if (nfds < 0) {
            perror("epoll wait failed");
            break;
        } else if (nfds == 0) {
            // 超时无事件，继续循环（心跳检测线程会运行）
            continue;
        }

        // 遍历所有触发的事件
        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;

            // 事件1：新客户端连接（服务端监听socket触发）
            if (fd == server_fd) {
                sockaddr_in client_addr;
                socklen_t addr_len = sizeof(client_addr);
                // 接受新连接
                int client_fd = accept(server_fd, (sockaddr*)&client_addr, &addr_len);
                if (client_fd < 0) {
                    perror("accept failed");
                    continue;
                }

                // 提取客户端IP地址并保存
                string client_ip = inet_ntoa(client_addr.sin_addr);
                {
                    lock_guard<mutex> lock(g_mutex);
                    g_client_ips[client_fd] = client_ip;
                }

                cout << "新客户端连接：FD=" << client_fd << "，IP=" << client_ip << endl;

                // 设置客户端socket为非阻塞模式
                set_nonblocking(client_fd);

                // 将客户端socket添加到epoll监听
                ev.data.fd = client_fd;
                ev.events = EPOLLIN | EPOLLET; // 边缘触发
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
                    perror("epoll_ctl add client_fd failed");
                    close(client_fd);
                    continue;
                }
            }

            // 事件2：客户端数据可读（客户端socket触发）
            else if (events[i].events & EPOLLIN) {
                bool is_eagain = false;
                do {
                    PacketHeader header;
                    vector<char> data;
                    // 读取完整数据包
                    if (recv_complete_packet(fd, header, data, is_eagain)) {
                        // 根据消息类型处理
                        switch (header.msg_type) {
                            case MsgType::LOGIN_REQ:
                                handle_login_req(fd, data);
                                break;
                            case MsgType::USER_LIST_REQ:
                                handle_user_list_req(fd);
                                break;
                            case MsgType::HEARTBEAT:
                                handle_heartbeat(fd);
                                break;
                            case MsgType::COMMON_MSG:
                                handle_common_msg(fd, data);
                                break;
                            default:
                                cout << "收到未知消息类型：" << (int)header.msg_type << "，来自FD=" << fd << endl;
                                break;
                        }
                    } else {
                        if (is_eagain) {
                            // 暂时无数据，退出循环
                            break;
                        } else {
                            // 读取失败，客户端断开连接
                            handle_client_disconnect(fd);
                            break;
                        }
                    }
                } while (!is_eagain);
            }
        }
    }

    // 9. 资源释放（程序退出时）
    close(epoll_fd);
    close(server_fd);
    cout << "服务端退出" << endl;
    return 0;
}