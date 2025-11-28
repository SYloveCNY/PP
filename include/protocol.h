#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <vector>
#include <string>
#include <chrono>

// 消息类型枚举（包含所有功能的消息类型）
enum class MsgType {
    LOGIN_REQ = 1,       // 登录请求
    LOGIN_RSP = 2,       // 登录响应
    USER_ONLINE_NOTIFY = 3,  // 用户上线通知
    USER_OFFLINE_NOTIFY = 4, // 用户下线通知
    USER_LIST_REQ = 5,   // 在线用户列表请求
    USER_LIST_RSP = 6,   // 在线用户列表响应
    COMMON_MSG = 7,      // 普通文本消息
    IMAGE_MSG = 8,       // 图片消息
    FILE_REQ = 9,        // 文件传输请求
    FILE_DATA = 10,      // 文件分片数据
    FILE_END = 11,       // 文件传输结束
    HEARTBEAT = 12       // 心跳包（维持连接）
};

// 数据包头部（所有消息的统一头部）
struct PacketHeader {
    MsgType msg_type;    // 消息类型
    size_t data_len;     // 数据长度（无符号类型，匹配vector::size()）
};

// 登录请求结构体
struct LoginReq {
    std::string nickname;   // 用户昵称
    std::vector<char> avatar; // 头像二进制数据（为空则无头像）
    uint16_t data_port;     // 客户端点对点数据传输端口
};

// 登录响应结构体
struct LoginRsp {
    bool success;       // 登录是否成功
    int user_id;        // 分配的用户ID（唯一标识）
    std::string msg;    // 响应提示信息
};

// 在线用户信息结构体（服务端和客户端共用）
struct UserInfo {
    int user_id;                // 用户ID
    std::string nickname;       // 昵称
    std::vector<char> avatar;   // 头像数据
    std::string ip;             // 客户端IP地址
    uint16_t data_port;         // 点对点数据传输端口
    // 最后心跳时间戳（服务端用于超时检测，客户端忽略不影响）
    std::chrono::time_point<std::chrono::system_clock> last_heartbeat;
};

// 普通文本消息结构体
struct CommonMsg {
    int from_user_id;       // 发送者用户ID
    std::string from_nickname; // 发送者昵称
    std::string content;    // 消息内容
};

// 图片消息结构体
struct ImageMsg {
    int from_user_id;       // 发送者用户ID
    std::string from_nickname; // 发送者昵称
    std::string img_name;   // 图片文件名（含后缀）
    std::vector<char> img_data; // 图片二进制数据
};

// 文件传输请求结构体（用于发起文件传输）
struct FileReq {
    int from_user_id;       // 发送者用户ID
    std::string from_nickname; // 发送者昵称
    std::string file_name;  // 文件名（含后缀）
    uint64_t file_size;     // 文件总大小（字节）
};

// 文件分片数据结构体（大文件分片传输）
struct FileData {
    uint32_t seq;           // 分片序号（从0开始）
    std::vector<char> data; // 分片二进制数据
};

// 序列化字符串：将string转为vector<char>（含长度信息，解决粘包）
inline std::vector<char> serialize_string(const std::string& str) {
    std::vector<char> data;
    size_t len = str.size();
    // 先存储字符串长度（size_t类型，4/8字节，取决于系统）
    data.resize(sizeof(size_t));
    memcpy(data.data(), &len, sizeof(size_t));
    // 再存储字符串内容
    data.insert(data.end(), str.begin(), str.end());
    return data;
}

// 反序列化字符串：从vector<char>中提取string（偏移量引用更新）
inline std::string deserialize_string(const std::vector<char>& data, size_t& offset) {
    // 检查长度字段是否越界
    if (offset + sizeof(size_t) > data.size()) {
        return "";
    }
    // 读取长度
    size_t len = 0;
    memcpy(&len, data.data() + offset, sizeof(size_t));
    offset += sizeof(size_t);

    // 检查内容是否越界
    if (offset + len > data.size()) {
        return "";
    }
    // 提取字符串
    std::string str(data.data() + offset, len);
    offset += len;
    return str;
}

// 序列化vector<char>：用于头像、图片、文件等二进制数据
inline std::vector<char> serialize_vector(const std::vector<char>& vec) {
    std::vector<char> data;
    size_t len = vec.size();
    // 先存储vector长度
    data.resize(sizeof(size_t));
    memcpy(data.data(), &len, sizeof(size_t));
    // 再存储vector内容
    data.insert(data.end(), vec.begin(), vec.end());
    return data;
}

// 反序列化vector<char>：从二进制数据中提取vector<char>
inline std::vector<char> deserialize_vector(const std::vector<char>& data, size_t& offset) {
    // 检查长度字段是否越界
    if (offset + sizeof(size_t) > data.size()) {
        return {};
    }
    // 读取长度
    size_t len = 0;
    memcpy(&len, data.data() + offset, sizeof(size_t));
    offset += sizeof(size_t);

    // 检查内容是否越界
    if (offset + len > data.size()) {
        return {};
    }
    // 提取vector
    std::vector<char> vec(data.data() + offset, data.data() + offset + len);
    offset += len;
    return vec;
}

// 序列化LoginRsp：将登录响应转为二进制数据
inline std::vector<char> serialize_login_rsp(const LoginRsp& rsp) {
    std::vector<char> data;
    // 序列化success（1字节，true=1，false=0）
    data.push_back(rsp.success ? 1 : 0);
    // 序列化user_id（4字节）
    data.resize(data.size() + sizeof(int));
    memcpy(data.data() + data.size() - sizeof(int), &rsp.user_id, sizeof(int));
    // 序列化msg（字符串，含长度）
    auto msg_data = serialize_string(rsp.msg);
    data.insert(data.end(), msg_data.begin(), msg_data.end());
    return data;
}

// 反序列化LoginRsp：从二进制数据中提取登录响应
inline LoginRsp deserialize_login_rsp(const std::vector<char>& data) {
    LoginRsp rsp;
    size_t offset = 0;

    // 反序列化success
    if (offset + 1 > data.size()) {
        rsp.success = false;
        rsp.msg = "反序列化失败：缺少success字段";
        return rsp;
    }
    rsp.success = (data[offset] == 1);
    offset += 1;

    // 反序列化user_id
    if (offset + sizeof(int) > data.size()) {
        rsp.success = false;
        rsp.msg = "反序列化失败：缺少user_id字段";
        return rsp;
    }
    memcpy(&rsp.user_id, data.data() + offset, sizeof(int));
    offset += sizeof(int);

    // 反序列化msg
    rsp.msg = deserialize_string(data, offset);
    return rsp;
}

#endif // PROTOCOL_H