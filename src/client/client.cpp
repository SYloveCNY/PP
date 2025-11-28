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
#include <fstream>
#include <thread>
#include <mutex>
#include <atomic>
#include <filesystem>
#include <limits>
#include <chrono>
#include "protocol.h"

using namespace std;
namespace fs = filesystem;

// 客户端配置常量（可根据需求修改）
const int BUF_SIZE = 4096;              // 缓冲区大小
const int SERVER_PORT = 8888;           // 服务端通信端口（与服务端保持一致）
const int DATA_PORT = 9999;             // 客户端点对点数据传输端口
const int FILE_CHUNK_SIZE = 4096;       // 文件分片大小（4KB，避免单次发送过大）
const int HEARTBEAT_INTERVAL = 10;      // 心跳发送间隔（10秒，维持与服务端连接）

// 全局共享数据（需互斥锁保护，多线程访问）
mutex g_mutex;                          // 全局互斥锁
atomic<bool> g_running = true;          // 程序运行标志（控制线程退出）
int g_server_fd = -1;                   // 与服务端通信的Socket FD
int g_user_id = -1;                     // 自身用户ID（服务端分配，唯一标识）
string g_nickname;                      // 自身昵称
vector<char> g_avatar;                  // 自身头像二进制数据（为空则无头像）
map<int, UserInfo> g_online_users;      // 在线用户列表（user_id -> UserInfo）
map<int, int> g_peer_fds;               // 点对点连接缓存（对方user_id -> 连接FD）

// 函数原型声明（解决"未声明"错误）
void handle_peer_msg(int peer_fd);       // 处理点对点消息
void handle_server_msg();                // 处理服务端消息
void send_common_msg();                  // 发送普通文本消息
void send_image_msg();                   // 发送图片消息
void send_file();                        // 发送文件
int connect_peer(int target_user_id);    // 建立点对点连接
int get_valid_target_id();               // 验证目标用户ID
void show_online_users();                // 显示在线用户列表
bool login();                            // 登录流程

// 1. 设置Socket为非阻塞模式（适配epoll I/O多路复用）
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

// 2. 发送完整数据包（头部+数据，解决部分发送问题）
bool send_packet(int fd, MsgType msg_type, const vector<char>& data) {
    PacketHeader header;
    header.msg_type = msg_type;
    header.data_len = data.size();

    // 先发送固定大小的头部
    if (send(fd, &header, sizeof(PacketHeader), 0) != sizeof(PacketHeader)) {
        perror("send header failed");
        return false;
    }

    // 循环发送数据（确保全部发送完成）
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

// 3. 接收完整数据包（处理粘包，确保头部+数据完整读取）
bool recv_complete_packet(int fd, PacketHeader& header, vector<char>& data) {
    // 第一步：读取头部（固定大小）
    ssize_t ret = recv(fd, &header, sizeof(PacketHeader), 0);
    if (ret == -1) {
        perror("recv header failed");
        return false;
    } else if (ret != sizeof(PacketHeader)) {
        cerr << "recv header incomplete: 期望" << sizeof(PacketHeader) << "字节，实际" << ret << "字节" << endl;
        return false;
    }

    // 第二步：读取数据（按头部指定长度读取）
    data.resize(header.data_len);
    size_t recved = 0;
    while (recved < header.data_len) {
        ret = recv(fd, data.data() + recved, header.data_len - recved, 0);
        if (ret == -1) {
            perror("recv data failed");
            return false;
        } else if (ret == 0) {
            cerr << "connection closed while receiving data" << endl;
            return false;
        }
        recved += ret;
    }
    return true;
}

// 4. 读取文件为二进制数据（支持图片、文档等任意文件）
vector<char> read_file(const string& file_path) {
    ifstream file(file_path, ios::binary | ios::ate);
    if (!file.is_open()) {
        cout << "❌ 文件打开失败：" << file_path << endl;
        return {};
    }

    // 获取文件大小并分配内存
    streampos file_size = file.tellg();
    vector<char> data(file_size);
    // 读取文件内容
    file.seekg(0, ios::beg);
    file.read(data.data(), file_size);
    file.close();

    cout << "✅ 文件读取成功：" << file_path << "，大小：" << file_size << "字节" << endl;
    return data;
}

// 5. 保存二进制数据为文件（接收图片/文件时使用，避免覆盖本地文件）
bool save_file(const string& file_path, const vector<char>& data) {
    ofstream file(file_path, ios::binary);
    if (!file.is_open()) {
        cout << "❌ 文件保存失败：" << file_path << endl;
        return false;
    }

    // 写入文件数据
    file.write(data.data(), data.size());
    file.close();

    cout << "✅ 文件已保存至：" << file_path << "，大小：" << data.size() << "字节" << endl;
    return true;
}

// 1. 心跳发送线程（定期向服务端发送心跳，防止连接被断开）
void heartbeat_send_thread() {
    while (g_running && g_server_fd != -1) {
        // 每10秒发送一次心跳包（无业务数据，仅维持连接）
        this_thread::sleep_for(chrono::seconds(HEARTBEAT_INTERVAL));
        if (!send_packet(g_server_fd, MsgType::HEARTBEAT, {})) {
            cout << "❌ 心跳包发送失败，与服务端连接可能已断开" << endl;
            g_running = false;
            break;
        }
        // 调试日志（可选注释：cout << "💓 已发送心跳包" << endl;）
    }
}

// 2. 点对点连接监听线程（接收其他客户端的连接请求）
void peer_listen_thread() {
    // 创建点对点监听Socket
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("❌ 点对点监听Socket创建失败");
        return;
    }

    // 设置端口复用（避免端口占用报错）
    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        perror("❌ 点对点监听setsockopt失败");
        close(listen_fd);
        return;
    }

    // 绑定点对点数据端口（与登录时告知服务端的端口一致）
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY; // 监听所有网卡IP
    addr.sin_port = htons(DATA_PORT);  // 绑定固定数据端口
    if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("❌ 点对点监听bind失败");
        close(listen_fd);
        return;
    }

    // 开始监听（最大等待队列长度5）
    if (listen(listen_fd, 5) < 0) {
        perror("❌ 点对点监听listen失败");
        close(listen_fd);
        return;
    }

    cout << "✅ 点对点数据传输监听已启动，端口：" << DATA_PORT << endl;

    // 循环接受其他客户端的连接请求
    while (g_running) {
        sockaddr_in peer_addr;
        socklen_t addr_len = sizeof(peer_addr);
        int peer_fd = accept(listen_fd, (sockaddr*)&peer_addr, &addr_len);
        if (peer_fd < 0) {
            perror("❌ 点对点accept失败");
            continue;
        }

        // 打印新连接信息
        string peer_ip = inet_ntoa(peer_addr.sin_addr);
        cout << "\n📥 收到新的点对点连接：IP=" << peer_ip << "，FD=" << peer_fd << endl;

        // 设置为非阻塞模式
        set_nonblocking(peer_fd);

        // 启动独立线程处理该点对点连接的消息
        thread peer_msg_thread(handle_peer_msg, peer_fd);
        peer_msg_thread.detach(); // 后台运行，无需等待
    }

    // 线程退出时关闭监听Socket
    close(listen_fd);
}

// 1. 登录流程（输入昵称/头像、连接服务端、获取用户ID）
bool login() {
    cout << "=====================================" << endl;
    cout << "          聊天客户端 - 登录" << endl;
    cout << "=====================================" << endl;

    // 输入昵称
    cout << "请输入你的昵称：";
    cin >> g_nickname;
    cin.ignore(numeric_limits<streamsize>::max(), '\n'); // 忽略换行符

    // 输入头像（可选）
    cout << "请输入头像文件路径（如：./avatar.jpg，无头像直接回车）：";
    string avatar_path;
    getline(cin, avatar_path);
    if (!avatar_path.empty()) {
        g_avatar = read_file(avatar_path);
        if (g_avatar.empty()) {
            cout << "⚠️  头像文件读取失败，将使用默认空头像" << endl;
        } else {
            cout << "✅ 头像读取成功，大小：" << g_avatar.size() << "字节" << endl;
        }
    } else {
        cout << "ℹ️  未设置头像，将使用默认空头像" << endl;
    }

    // 创建与服务端通信的Socket
    g_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server_fd < 0) {
        perror("❌ 服务端Socket创建失败");
        return false;
    }

    // 连接服务端（本地测试用127.0.0.1，远程服务器需修改IP）
    sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr) <= 0) {
        perror("❌ 无效的服务端IP");
        close(g_server_fd);
        return false;
    }

    if (connect(g_server_fd, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("❌ 连接服务端失败（请确保服务端已启动）");
        close(g_server_fd);
        return false;
    }
    cout << "✅ 已连接服务端（IP：127.0.0.1，端口：" << SERVER_PORT << "）" << endl;

    // 构造并发送登录请求
    LoginReq req;
    req.nickname = g_nickname;
    req.avatar = g_avatar;
    req.data_port = DATA_PORT;

    vector<char> req_data;
    auto nickname_data = serialize_string(req.nickname);
    auto avatar_data = serialize_vector(req.avatar);
    req_data.insert(req_data.end(), nickname_data.begin(), nickname_data.end());
    req_data.insert(req_data.end(), avatar_data.begin(), avatar_data.end());
    req_data.insert(req_data.end(), (char*)&req.data_port, (char*)&req.data_port + sizeof(uint16_t));

    if (!send_packet(g_server_fd, MsgType::LOGIN_REQ, req_data)) {
        cout << "❌ 登录请求发送失败" << endl;
        close(g_server_fd);
        return false;
    }

    // 接收并处理登录响应
    PacketHeader header;
    vector<char> rsp_data;
    if (!recv_complete_packet(g_server_fd, header, rsp_data) || header.msg_type != MsgType::LOGIN_RSP) {
        cout << "❌ 登录响应接收失败" << endl;
        close(g_server_fd);
        return false;
    }

    LoginRsp rsp = deserialize_login_rsp(rsp_data);
    if (!rsp.success) {
        cout << "❌ 登录失败：" << rsp.msg << endl;
        close(g_server_fd);
        return false;
    }

    // 登录成功，保存用户ID
    g_user_id = rsp.user_id;
    cout << "🎉 登录成功！你的用户ID：" << g_user_id << "，提示：" << rsp.msg << endl;

    // 主动请求在线用户列表
    if (!send_packet(g_server_fd, MsgType::USER_LIST_REQ, {})) {
        cout << "❌ 在线用户列表请求发送失败" << endl;
        close(g_server_fd);
        return false;
    }

    return true;
}

// 2. 显示在线用户列表
void show_online_users() {
    lock_guard<mutex> lock(g_mutex);
    cout << "\n=====================================" << endl;
    cout << "           在线用户列表" << endl;
    cout << "=====================================" << endl;
    if (g_online_users.empty()) {
        cout << "⚠️  当前无在线用户" << endl;
        cout << "=====================================" << endl;
        return;
    }

    for (auto& [user_id, user] : g_online_users) {
        if (user_id == g_user_id) {
            cout << "ID：" << user_id << "（自己） | 昵称：" << user.nickname << " | IP：" << user.ip << endl;
        } else {
            cout << "ID：" << user_id << " | 昵称：" << user.nickname << " | IP：" << user.ip << " | 数据端口：" << user.data_port << endl;
        }
    }
    cout << "=====================================" << endl;
}

// 3. 验证目标用户ID（防止输入错误）
int get_valid_target_id() {
    int target_id = -1;
    while (true) {
        cout << "请输入目标用户ID（输入-1取消）：";
        cin >> target_id;

        if (cin.fail()) {
            cin.clear();
            cin.ignore(numeric_limits<streamsize>::max(), '\n');
            cout << "❌ 输入错误！请输入有效的整数ID。" << endl;
            continue;
        }

        if (target_id == -1) {
            cout << "ℹ️  已取消操作。" << endl;
            return -1;
        }

        if (target_id == g_user_id) {
            cout << "❌ 错误！不能选择自己作为目标用户。" << endl;
            continue;
        }

        lock_guard<mutex> lock(g_mutex);
        if (g_online_users.find(target_id) == g_online_users.end()) {
            cout << "❌ 错误！ID为" << target_id << "的用户不存在或已下线。" << endl;
            show_online_users();
            continue;
        }

        cin.ignore(numeric_limits<streamsize>::max(), '\n');
        return target_id;
    }
}

// 4. 建立点对点连接（已连接则直接返回FD）
int connect_peer(int target_user_id) {
    lock_guard<mutex> lock(g_mutex);

    // 检查目标用户是否在线
    auto user_it = g_online_users.find(target_user_id);
    if (user_it == g_online_users.end()) {
        cout << "❌ 目标用户不存在或已下线" << endl;
        return -1;
    }

    // 检查是否已建立连接
    auto peer_it = g_peer_fds.find(target_user_id);
    if (peer_it != g_peer_fds.end()) {
        return peer_it->second;
    }

    // 新建连接
    int peer_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (peer_fd < 0) {
        perror("❌ 点对点Socket创建失败");
        return -1;
    }

    sockaddr_in peer_addr;
    memset(&peer_addr, 0, sizeof(peer_addr));
    peer_addr.sin_family = AF_INET;
    peer_addr.sin_port = htons(user_it->second.data_port);
    if (inet_pton(AF_INET, user_it->second.ip.c_str(), &peer_addr.sin_addr) <= 0) {
        perror("❌ 无效的目标用户IP");
        close(peer_fd);
        return -1;
    }

    if (connect(peer_fd, (sockaddr*)&peer_addr, sizeof(peer_addr)) < 0) {
        perror("❌ 连接目标用户失败");
        close(peer_fd);
        return -1;
    }

    cout << "✅ 已与用户 [" << user_it->second.nickname << "]（ID:" << target_user_id << "）建立点对点连接" << endl;
    g_peer_fds[target_user_id] = peer_fd;
    return peer_fd;
}

// 5. 发送普通文本消息
void send_common_msg() {
    // 1. 显示在线用户列表供选择
    cout << "\n=====================================" << endl;
    cout << "           选择收信人" << endl;
    cout << "=====================================" << endl;
    lock_guard<mutex> lock(g_mutex);
    if (g_online_users.empty()) {
        cout << "❌ 当前无在线用户" << endl;
        return;
    }
    for (auto& [user_id, user] : g_online_users) {
        if (user_id == g_user_id) continue; // 跳过自己
        cout << "ID: " << user_id << "，昵称: " << user.nickname << endl;
    }

    // 2. 输入目标用户ID
    int to_user_id;
    cout << "请输入收信人ID: ";
    cin >> to_user_id;
    cin.ignore(numeric_limits<streamsize>::max(), '\n'); // 清除输入缓冲区

    // 检查目标用户是否在线（排除自己）
    if (to_user_id == g_user_id) {
        cout << "❌ 不能向自己发送消息" << endl;
        return;
    }
    if (g_online_users.find(to_user_id) == g_online_users.end()) {
        cout << "❌ 目标用户不在线" << endl;
        return;
    }

    // 3. 输入消息内容（支持多行，输入:send结束）
    cout << "\n请输入消息内容（输入 \":send\" 发送，回车换行）：" << endl;
    string content;
    string line;
    while (true) {
        getline(cin, line);
        if (line == ":send") {
            break; // 输入:send时结束编辑
        }
        content += line + "\n"; // 保留换行符
    }
    if (content.empty()) {
        cout << "❌ 消息内容不能为空" << endl;
        return;
    }

    // 4. 构造消息并发送
    CommonMsg msg;
    msg.from_user_id = g_user_id;
    msg.to_user_id = to_user_id;
    msg.from_nickname = g_nickname;
    msg.content = content;

    // 序列化消息
    vector<char> data;
    // 序列化from_user_id
    data.insert(data.end(), (char*)&msg.from_user_id, (char*)&msg.from_user_id + sizeof(int));
    // 序列化to_user_id
    data.insert(data.end(), (char*)&msg.to_user_id, (char*)&msg.to_user_id + sizeof(int));
    // 序列化from_nickname
    auto nickname_data = serialize_string(msg.from_nickname);
    data.insert(data.end(), nickname_data.begin(), nickname_data.end());
    // 序列化content
    auto content_data = serialize_string(msg.content);
    data.insert(data.end(), content_data.begin(), content_data.end());

    // 发送消息（通过点对点连接或服务端转发，这里假设通过服务端转发）
    if (send_packet(g_server_fd, MsgType::COMMON_MSG, data)) {
        cout << "✅ 消息已发送" << endl;
    } else {
        cout << "❌ 消息发送失败" << endl;
    }
}

// 6. 发送图片消息
void send_image_msg() {
    cout << "\n=====================================" << endl;
    cout << "           发送图片消息" << endl;
    cout << "=====================================" << endl;

    show_online_users();
    int target_id = get_valid_target_id();
    if (target_id == -1) return;

    int peer_fd = connect_peer(target_id);
    if (peer_fd < 0) return;

    cout << "请输入图片文件路径（如：./pic.jpg，输入exit取消）：";
    string img_path;
    getline(cin, img_path);
    if (img_path == "exit") {
        cout << "ℹ️  已取消发送图片。" << endl;
        return;
    }

    vector<char> img_data = read_file(img_path);
    if (img_data.empty()) return;

    fs::path path(img_path);
    string img_name = path.filename().string();

    // 构造图片消息并发送
    ImageMsg msg;
    msg.from_user_id = g_user_id;
    msg.from_nickname = g_nickname;
    msg.img_name = img_name;
    msg.img_data = img_data;

    vector<char> msg_data;
    auto from_nickname_data = serialize_string(msg.from_nickname);
    auto img_name_data = serialize_string(msg.img_name);
    auto img_data_data = serialize_vector(msg.img_data);
    msg_data.insert(msg_data.end(), (char*)&msg.from_user_id, (char*)&msg.from_user_id + sizeof(int));
    msg_data.insert(msg_data.end(), from_nickname_data.begin(), from_nickname_data.end());
    msg_data.insert(msg_data.end(), img_name_data.begin(), img_name_data.end());
    msg_data.insert(msg_data.end(), img_data_data.begin(), img_data_data.end());

    if (send_packet(peer_fd, MsgType::IMAGE_MSG, msg_data)) {
        cout << "✅ 图片发送成功！" << endl;
    } else {
        cout << "❌ 图片发送失败" << endl;
        g_peer_fds.erase(target_id);
        close(peer_fd);
    }
}

// 7. 发送文件
void send_file() {
    cout << "\n=====================================" << endl;
    cout << "             发送文件" << endl;
    cout << "=====================================" << endl;

    show_online_users();
    int target_id = get_valid_target_id();
    if (target_id == -1) return;

    int peer_fd = connect_peer(target_id);
    if (peer_fd < 0) return;

    cout << "请输入文件路径（如：./file.zip，输入exit取消）：";
    string file_path;
    getline(cin, file_path);
    if (file_path == "exit") {
        cout << "ℹ️  已取消发送文件。" << endl;
        return;
    }

    vector<char> file_data = read_file(file_path);
    if (file_data.empty()) return;

    fs::path path(file_path);
    string file_name = path.filename().string();
    uint64_t file_size = file_data.size();

    // 发送文件传输请求
    FileReq req;
    req.from_user_id = g_user_id;
    req.from_nickname = g_nickname;
    req.file_name = file_name;
    req.file_size = file_size;

    vector<char> req_data;
    auto from_nickname_data = serialize_string(req.from_nickname);
    auto file_name_data = serialize_string(req.file_name);
    req_data.insert(req_data.end(), (char*)&req.from_user_id, (char*)&req.from_user_id + sizeof(int));
    req_data.insert(req_data.end(), from_nickname_data.begin(), from_nickname_data.end());
    req_data.insert(req_data.end(), file_name_data.begin(), file_name_data.end());
    req_data.insert(req_data.end(), (char*)&req.file_size, (char*)&req.file_size + sizeof(uint64_t));

    if (!send_packet(peer_fd, MsgType::FILE_REQ, req_data)) {
        cout << "❌ 文件请求发送失败" << endl;
        g_peer_fds.erase(target_id);
        close(peer_fd);
        return;
    }

    // 分片发送文件数据
    uint32_t seq = 0;
    size_t total_chunks = (file_size + FILE_CHUNK_SIZE - 1) / FILE_CHUNK_SIZE;
    for (size_t i = 0; i < total_chunks; ++i) {
        size_t chunk_size = min((size_t)FILE_CHUNK_SIZE, file_size - i * FILE_CHUNK_SIZE);
        vector<char> chunk_data(file_data.begin() + i * FILE_CHUNK_SIZE, 
                                file_data.begin() + i * FILE_CHUNK_SIZE + chunk_size);

        vector<char> pkt_data;
        pkt_data.resize(sizeof(uint32_t) + chunk_size);
        uint32_t current_seq = seq++;
        memcpy(pkt_data.data(), &current_seq, sizeof(uint32_t));
        memcpy(pkt_data.data() + sizeof(uint32_t), chunk_data.data(), chunk_size);

        if (!send_packet(peer_fd, MsgType::FILE_DATA, pkt_data)) {
            cout << "❌ 文件分片 " << (i + 1) << "/" << total_chunks << " 发送失败" << endl;
            g_peer_fds.erase(target_id);
            close(peer_fd);
            return;
        }

        // 显示传输进度
        cout << "📤 文件传输中：" << (i + 1) << "/" << total_chunks 
             << "（" << (i + 1) * 100 / total_chunks << "%）" << "\r" << flush;
    }

    // 发送传输结束通知
    send_packet(peer_fd, MsgType::FILE_END, {});
    cout << "\n✅ 文件传输完成！" << endl;
}

// 8. 处理服务端消息（在线列表、上下线通知）
void handle_server_msg() {
    while (g_running) {
        PacketHeader header;
        vector<char> data;
        if (!recv_complete_packet(g_server_fd, header, data)) {
            cout << "\n❌ 与服务端连接断开，程序将退出" << endl;
            g_running = false;
            break;
        }

        switch (header.msg_type) {
            case MsgType::USER_LIST_RSP: {
                lock_guard<mutex> lock(g_mutex);
                g_online_users.clear();
                size_t offset = 0;

                // 读取用户数量
                size_t user_count = 0;
                if (offset + sizeof(size_t) <= data.size()) {
                    memcpy(&user_count, data.data() + offset, sizeof(size_t));
                    offset += sizeof(size_t);
                }
                cout << "\nℹ️  服务端返回在线用户数：" << user_count << endl;

                // 读取每个用户信息
                for (size_t i = 0; i < user_count; ++i) {
                    UserInfo user;
                    user.nickname = deserialize_string(data, offset);
                    user.avatar = deserialize_vector(data, offset);
                    user.ip = deserialize_string(data, offset);

                    if (offset + sizeof(int) <= data.size()) {
                        memcpy(&user.user_id, data.data() + offset, sizeof(int));
                        offset += sizeof(int);
                    }
                    if (offset + sizeof(uint16_t) <= data.size()) {
                        memcpy(&user.data_port, data.data() + offset, sizeof(uint16_t));
                        offset += sizeof(uint16_t);
                    }

                    g_online_users[user.user_id] = user;
                    cout << "ℹ️  已加载用户：ID=" << user.user_id << "，昵称=" << user.nickname << endl;
                }

                cout << "\n✅ 在线用户列表已更新" << endl;
                show_online_users();
                break;
            }

            case MsgType::USER_ONLINE_NOTIFY: {
                lock_guard<mutex> lock(g_mutex);
                UserInfo user;
                size_t offset = 0;

                user.nickname = deserialize_string(data, offset);
                user.avatar = deserialize_vector(data, offset);
                user.ip = deserialize_string(data, offset);
                if (offset + sizeof(int) <= data.size()) {
                    memcpy(&user.user_id, data.data() + offset, sizeof(int));
                    offset += sizeof(int);
                }
                if (offset + sizeof(uint16_t) <= data.size()) {
                    memcpy(&user.data_port, data.data() + offset, sizeof(uint16_t));
                    offset += sizeof(uint16_t);
                }

                g_online_users[user.user_id] = user;
                cout << "\n📢 【上线通知】用户 [" << user.nickname << "]（ID:" << user.user_id << "）已上线" << endl;
                show_online_users();
                break;
            }

            case MsgType::USER_OFFLINE_NOTIFY: {
                lock_guard<mutex> lock(g_mutex);
                if (data.size() < sizeof(int)) break;

                int user_id = 0;
                memcpy(&user_id, data.data(), sizeof(int));
                string nickname(data.data() + sizeof(int), data.size() - sizeof(int));

                g_online_users.erase(user_id);
                g_peer_fds.erase(user_id);

                cout << "\n📢 【下线通知】用户 [" << nickname << "]（ID:" << user_id << "）已下线" << endl;
                show_online_users();
                break;
            }

            default:
                cout << "\n❓ 收到未知服务端消息类型：" << (int)header.msg_type << endl;
                break;
        }
    }
}

// 9. 处理点对点消息（普通消息、图片、文件）
void handle_peer_msg(int peer_fd) {
    while (g_running) {
        PacketHeader header;
        vector<char> data;
        if (!recv_complete_packet(peer_fd, header, data)) {
            cout << "📤 点对点连接FD=" << peer_fd << "已断开" << endl;
            close(peer_fd);
            // 清理连接缓存
            lock_guard<mutex> lock(g_mutex);
            for (auto it = g_peer_fds.begin(); it != g_peer_fds.end(); ) {
                if (it->second == peer_fd) {
                    it = g_peer_fds.erase(it);
                } else {
                    ++it;
                }
            }
            return;
        }

        // 根据消息类型处理（此处仅示例，需根据实际需求补充）
        switch (header.msg_type) {
            case MsgType::COMMON_MSG:
                // 处理普通消息
                break;
            case MsgType::FILE_DATA:
                // 处理文件数据
                break;
            // 其他消息类型...
            default:
                cout << "未知的点对点消息类型" << endl;
        }
    }
}

int main() {
    // 1. 登录初始化（登录失败则退出程序）
    if (!login()) {
        cout << "❌ 登录失败，程序退出" << endl;
        return -1;
    }

    // 2. 启动后台线程（心跳发送、点对点监听、服务端消息处理）
    thread heartbeat_t(heartbeat_send_thread);
    thread peer_listen_t(peer_listen_thread);
    thread server_msg_t(handle_server_msg);

    // 设置线程分离（后台运行，无需主线程等待）
    heartbeat_t.detach();
    peer_listen_t.detach();
    server_msg_t.detach();

    // 3. 主菜单交互循环
    while (g_running) {
        cout << "\n=====================================" << endl;
        cout << "          聊天客户端 - 主菜单" << endl;
        cout << "=====================================" << endl;
        cout << "1. 发送普通消息" << endl;
        cout << "2. 发送图片消息" << endl;
        cout << "3. 发送文件" << endl;
        cout << "4. 查看在线用户列表" << endl;
        cout << "5. 退出程序" << endl;
        cout << "=====================================" << endl;
        cout << "请输入操作序号（1-5）：";

        int choice;
        cin >> choice;
        cin.ignore(numeric_limits<streamsize>::max(), '\n'); // 忽略换行符

        switch (choice) {
            case 1:
                send_common_msg();
                break;
            case 2:
                send_image_msg();
                break;
            case 3:
                send_file();
                break;
            case 4:
                show_online_users();
                break;
            case 5:
                g_running = false;
                cout << "ℹ️  正在退出程序..." << endl;
                break;
            default:
                cout << "❌ 输入错误！请输入1-5之间的序号。" << endl;
                break;
        }
    }

    // 4. 资源释放（程序退出时清理）
    {
        lock_guard<mutex> lock(g_mutex);
        // 关闭所有点对点连接
        for (auto& [user_id, peer_fd] : g_peer_fds) {
            close(peer_fd);
        }
        g_peer_fds.clear();
        g_online_users.clear();
    }

    // 关闭与服务端的连接
    if (g_server_fd != -1) {
        close(g_server_fd);
        g_server_fd = -1;
    }

    cout << "✅ 程序已安全退出" << endl;
    return 0;
}