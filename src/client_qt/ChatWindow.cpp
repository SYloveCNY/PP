#include "ChatWindow.h"
#include "ui_ChatWindow.h"
#include <QMessageBox>
#include <QMetaObject>
#include <iostream>
#include <ostream>
#include <QtEndian>    // Qt跨平台字节序函数（替代htonl/ntohl）
#include <arpa/inet.h> // 备用：如果Qt函数不生效，htonl/ntohl的头文件
#include <algorithm>   // 遍历map转vector需要

ChatWindow::ChatWindow(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::ChatWindow),
    m_userId(-1),
    m_serverSocket(nullptr),
    m_onlineUserList(nullptr),
    m_recvBuffer(QByteArray()) // 初始化粘包缓存
{
    ui->setupUi(this);
    // 绑定在线用户列表UI控件（对应ChatWindow.ui中的listWidget_onlineUsers）
    m_onlineUserList = ui->listWidget_onlineUsers;
}

ChatWindow::~ChatWindow() {
    delete ui; // 释放UI资源
}

// 登录成功后设置用户信息和Socket
void ChatWindow::setLoginInfo(int userId, const QString& nickname, QTcpSocket* serverSocket) {
    m_userId = userId;
    m_nickname = nickname;
    m_serverSocket = serverSocket;

    // 绑定Socket的readyRead信号（接收服务端数据）
    connect(m_serverSocket, &QTcpSocket::readyRead, this, &ChatWindow::onServerReadyRead);
    
    // 登录成功后立即发送在线用户列表请求
    onLoginSuccess();
}

// 登录成功后发送USER_LIST_REQ（修复枚举前缀+字节序函数）
void ChatWindow::onLoginSuccess() {
    PacketHeader header;
    // 1. 修复枚举前缀：加MsgType::，强转uint32_t
    // 2. 改用Qt跨平台函数qToBigEndian（替代htonl，无需系统头文件）
    header.msgType = qToBigEndian(static_cast<uint32_t>(MsgType::USER_LIST_REQ));
    header.dataLen = qToBigEndian(static_cast<uint32_t>(0)); // 无数据
    
    // 发送请求
    m_serverSocket->write((char*)&header, sizeof(PacketHeader));
    std::cout << "发送在线用户列表请求（USER_LIST_REQ）" << std::endl;
}

// 显示系统消息（补全实现）
void ChatWindow::showMessage(const QString& sender, const QString& content) {
    // 在聊天日志框显示（对应ChatWindow.ui中的textEdit_chatLog）
    ui->textEdit_chatLog->append(QString("[%1] %2").arg(sender).arg(content));
}

// 接收服务端数据（修复所有错误）
void ChatWindow::onServerReadyRead() {
    QTcpSocket* socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket) return;

    // 读取所有数据到粘包缓存
    m_recvBuffer.append(socket->readAll());

    // 循环解析完整数据包（包头+数据）
    while (m_recvBuffer.size() >= sizeof(PacketHeader)) {
        // 提取包头
        PacketHeader header;
        memcpy(&header, m_recvBuffer.data(), sizeof(PacketHeader));
        
        // 1. 修复ntohl：改用Qt跨平台函数qFromBigEndian
        // 2. 修复枚举前缀：强转MsgType
        MsgType msgType = static_cast<MsgType>(qFromBigEndian(header.msgType));
        uint32_t dataLen = qFromBigEndian(header.dataLen);

        // 检查数据是否完整
        if (m_recvBuffer.size() < sizeof(PacketHeader) + dataLen) {
            break; // 数据不完整，等待后续
        }

        // 提取JSON数据
        QByteArray jsonData = m_recvBuffer.mid(sizeof(PacketHeader), dataLen);
        // 移除已解析的数据包（清理缓存）
        m_recvBuffer.remove(0, sizeof(PacketHeader) + dataLen);

        // 解析JSON
        QJsonDocument doc = QJsonDocument::fromJson(jsonData);
        if (doc.isNull()) {
            showMessage("系统", "收到无效JSON数据");
            continue;
        }
        QJsonObject root = doc.object();

        // 处理在线用户列表响应（修复枚举前缀）
        if (msgType == MsgType::USER_LIST_RSP) {
            QJsonObject data = root["data"].toObject();
            QJsonArray usersArray = data["users"].toArray();

            // 修复类型不匹配：map转vector（匹配updateOnlineUsers参数）
            std::vector<UserInfo> onlineUsers;
            for (const QJsonValue& val : usersArray) {
                QJsonObject userObj = val.toObject();
                UserInfo user;
                user.userId = userObj["userId"].toInt();
                user.nickname = userObj["nickname"].toString().toStdString();
                user.ip = userObj["ip"].toString().toStdString();
                user.dataPort = userObj["dataPort"].toInt();
                onlineUsers.push_back(user); // 存入vector
            }

            // 更新UI（参数为vector，匹配函数声明）
            updateOnlineUsers(onlineUsers);
            showMessage("系统", QString("在线用户列表更新：共%1人").arg(onlineUsers.size()));
        }
    }
}

// 更新在线用户UI（参数为vector<UserInfo>，完全匹配）
void ChatWindow::updateOnlineUsers(const std::vector<UserInfo>& users) {
    if (!m_onlineUserList) return;

    // 清空旧列表
    m_onlineUserList->clear();

    // 添加在线用户到列表
    for (const auto& user : users) {
        QString userText = QString("%1 (ID: %2)")
                            .arg(QString::fromStdString(user.nickname))
                            .arg(user.userId);
        m_onlineUserList->addItem(userText);
    }
}
// #include "ChatWindow.h"
// #include <QVBoxLayout>
// #include <QHBoxLayout>
// #include <QListWidget>
// #include <QTextEdit>
// #include <QLineEdit>
// #include <QPushButton>
// #include <QMessageBox>
// #include <QTimer>
// #include <QDateTime>
// #include <QDebug>
// #include <stdexcept>
// #include "protocol_qt.h"  // 包含协议序列化/反序列化函数
// #include "UserInfo.h"     // 包含 UserInfo 结构体定义

// // 构造函数：初始化 UI、绑定信号槽、启动定时器
// ChatWindow::ChatWindow(int userId, const QString& nickname, QTcpSocket* serverSocket, QUdpSocket* udpSocket, QWidget* parent)
//     : QWidget(parent)
//     , m_userId(userId)
//     , m_nickname(nickname)
//     , m_serverSocket(serverSocket)
//     , m_udpSocket(udpSocket)
//     , m_selectedUserId(0)  // 默认选中广播
//     , m_recvBuffer()
//     , m_onlineUsers()
// {
//     // 禁止手动删除 Socket（由登录窗口创建，统一管理）
//     m_serverSocket->setParent(this);
//     if (m_udpSocket) m_udpSocket->setParent(this);

//     // 初始化 UI
//     initUI();

//     // 初始化定时器
//     initTimers();

//     // 绑定 Socket 信号槽
//     bindSocketSignals();

//     // 发送用户列表请求 + 启动超时定时器
//     sendUserListReq();
//     m_userListTimeoutTimer->start(5000);  // 5秒超时
// }

// // 析构函数：释放资源
// ChatWindow::~ChatWindow()
// {
//     // 停止定时器
//     m_heartbeatTimer->stop();
//     m_userListTimeoutTimer->stop();

//     // 释放定时器
//     delete m_heartbeatTimer;
//     delete m_userListTimeoutTimer;
// }

// // 初始化 UI 布局和控件
// void ChatWindow::initUI()
// {
//     // 设置窗口属性
//     setWindowTitle(QString("聊天窗口 - %1（ID:%2）").arg(m_nickname).arg(m_userId));
//     setMinimumSize(800, 600);

//     // 1. 左侧：用户列表
//     m_userListWidget = new QListWidget(this);
//     m_userListWidget->setMinimumWidth(200);
//     m_userListWidget->setMaximumWidth(300);
//     // 绑定用户选择事件
//     connect(m_userListWidget, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* current) {
//         if (current) {
//             m_selectedUserId = current->data(Qt::UserRole).toInt();
//             qDebug() << "[用户选择] 选中目标ID：" << m_selectedUserId;
//         }
//     });

//     // 2. 右侧：聊天区域 + 输入区域
//     QWidget* rightWidget = new QWidget(this);
//     QVBoxLayout* rightLayout = new QVBoxLayout(rightWidget);

//     // 聊天显示区域（只读）
//     m_chatDisplay = new QTextEdit(this);
//     m_chatDisplay->setReadOnly(true);
//     m_chatDisplay->setPlaceholderText("聊天记录...");
//     rightLayout->addWidget(m_chatDisplay, 1);  // 占满剩余空间

//     // 输入区域（输入框 + 发送按钮）
//     QWidget* inputWidget = new QWidget(this);
//     QHBoxLayout* inputLayout = new QHBoxLayout(inputWidget);
//     m_msgInput = new QLineEdit(this);
//     m_msgInput->setPlaceholderText("输入消息后按回车或点击发送...");
//     m_sendBtn = new QPushButton("发送", this);
//     inputLayout->addWidget(m_msgInput, 1);
//     inputLayout->addWidget(m_sendBtn);
//     rightLayout->addWidget(inputWidget);

//     // 3. 主布局：左右分栏
//     QHBoxLayout* mainLayout = new QHBoxLayout(this);
//     mainLayout->addWidget(m_userListWidget);
//     mainLayout->addWidget(rightWidget, 1);
//     setLayout(mainLayout);

//     // 绑定发送按钮和回车事件
//     connect(m_sendBtn, &QPushButton::clicked, this, [this]() { sendMessage(m_msgInput->text().trimmed()); });
//     connect(m_msgInput, &QLineEdit::returnPressed, this, [this]() { sendMessage(m_msgInput->text().trimmed()); });
// }

// // 初始化定时器（心跳 + 用户列表超时）
// void ChatWindow::initTimers()
// {
//     // 1. 心跳定时器：每3秒发送一次心跳包
//     m_heartbeatTimer = new QTimer(this);
//     m_heartbeatTimer->setInterval(3000);
//     connect(m_heartbeatTimer, &QTimer::timeout, this, &ChatWindow::sendHeartbeat);
//     m_heartbeatTimer->start();  // 启动心跳

//     // 2. 用户列表超时定时器：单次触发
//     m_userListTimeoutTimer = new QTimer(this);
//     m_userListTimeoutTimer->setSingleShot(true);
//     connect(m_userListTimeoutTimer, &QTimer::timeout, this, [this]() {
//         showMessage("系统", "获取在线用户列表超时，请检查网络连接！");
//     });
// }

// // 绑定 Socket 信号槽（接收数据、断开连接）
// void ChatWindow::bindSocketSignals()
// {
//     // 接收数据信号
//     connect(m_serverSocket, &QTcpSocket::readyRead, this, &ChatWindow::onServerReadyRead);
//     // 断开连接信号
//     connect(m_serverSocket, &QTcpSocket::disconnected, this, &ChatWindow::onDisconnected);
//     // Socket 错误信号
//     connect(m_serverSocket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError err) {
//         QString errMsg = QString("Socket错误：%1（错误码：%2）").arg(m_serverSocket->errorString()).arg(err);
//         qDebug() << errMsg;
//         showMessage("系统", errMsg);
//     });
// }

// // 发送用户列表请求（USER_LIST_REQ）
// void ChatWindow::sendUserListReq()
// {
//     if (m_serverSocket->state() != QAbstractSocket::ConnectedState) {
//         qDebug() << "[发送用户列表请求] 失败：Socket未连接";
//         return;
//     }

//     // 构造包头（无数据体，dataLen=0）
//     PacketHeader header;
//     header.msgType = htonl(static_cast<uint32_t>(MsgType::USER_LIST_REQ));
//     header.dataLen = htonl(0);

//     // 发送请求
//     qint64 sent = m_serverSocket->write(reinterpret_cast<char*>(&header), sizeof(PacketHeader));
//     if (sent == sizeof(PacketHeader)) {
//         qDebug() << "[发送用户列表请求] 成功，字节数：" << sent;
//         showMessage("系统", "正在获取在线用户列表...");
//     } else {
//         qDebug() << "[发送用户列表请求] 失败，发送字节数：" << sent;
//         showMessage("系统", "发送用户列表请求失败！");
//     }
// }

// // 发送聊天消息（COMMON_MSG）
// void ChatWindow::sendMessage(const QString& content)
// {
//     // 输入校验
//     if (content.isEmpty()) {
//         QMessageBox::warning(this, "输入错误", "消息内容不能为空！");
//         return;
//     }
//     if (m_serverSocket->state() != QAbstractSocket::ConnectedState) {
//         QMessageBox::warning(this, "发送失败", "与服务端的连接已断开！");
//         return;
//     }
//     if (m_selectedUserId == -1) {
//         QMessageBox::warning(this, "发送失败", "未选择接收者！");
//         return;
//     }

//     try {
//         // 构造普通消息
//         CommonMsg msg;
//         msg.fromUserId = m_userId;
//         msg.toUserId = m_selectedUserId;
//         msg.fromNickname = m_nickname.toStdString();
//         msg.content = content.toStdString();
//         // 可选：添加时间戳（如果协议支持）
//         // msg.timestamp = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

//         // 序列化消息体
//         std::vector<char> payload = serializeCommonMsg(msg);

//         // 构造包头
//         PacketHeader header;
//         header.msgType = htonl(static_cast<uint32_t>(MsgType::COMMON_MSG));
//         header.dataLen = htonl(static_cast<uint32_t>(payload.size()));

//         // 拼接完整数据包
//         std::vector<char> sendData;
//         sendData.insert(sendData.end(), reinterpret_cast<char*>(&header),
//                         reinterpret_cast<char*>(&header) + sizeof(PacketHeader));
//         sendData.insert(sendData.end(), payload.begin(), payload.end());

//         // 发送数据（确保完整发送）
//         qint64 totalSent = 0;
//         qint64 totalLen = sendData.size();
//         while (totalSent < totalLen) {
//             qint64 sent = m_serverSocket->write(sendData.data() + totalSent, totalLen - totalSent);
//             if (sent == -1) {
//                 throw std::runtime_error("Socket写入失败：" + m_serverSocket->errorString().toStdString());
//             }
//             totalSent += sent;

//             // 等待数据写入内核缓冲区（超时5秒）
//             if (!m_serverSocket->waitForBytesWritten(5000)) {
//                 throw std::runtime_error("发送超时：" + m_serverSocket->errorString().toStdString());
//             }
//         }

//         // 本地显示发送的消息
//         QString senderPrefix = "我";
//         if (m_selectedUserId == 0) {
//             senderPrefix += "（广播）";
//         } else {
//             auto it = m_onlineUsers.find(m_selectedUserId);
//             if (it != m_onlineUsers.end()) {
//                 senderPrefix += QString("（发给%1）").arg(QString::fromStdString(it->second.nickname));
//             } else {
//                 senderPrefix += QString("（发给ID:%1）").arg(m_selectedUserId);
//             }
//         }
//         showMessage(senderPrefix, content);

//         // 清空输入框
//         m_msgInput->clear();

//         qDebug() << "[发送消息] 成功，总字节数：" << totalLen
//                  << "，目标ID：" << m_selectedUserId
//                  << "，内容：" << content;

//     } catch (const std::exception& e) {
//         QString errMsg = "发送消息失败：" + QString::fromStdString(e.what());
//         showMessage("系统", errMsg);
//         qDebug() << "[发送消息] 失败：" << errMsg;
//     }
// }

// // 发送心跳包（HEARTBEAT）
// void ChatWindow::sendHeartbeat()
// {
//     if (m_serverSocket->state() != QAbstractSocket::ConnectedState) {
//         qDebug() << "[发送心跳] 失败：Socket未连接";
//         return;
//     }

//     // 构造心跳包（无数据体）
//     PacketHeader header;
//     header.msgType = htonl(static_cast<uint32_t>(MsgType::HEARTBEAT));
//     header.dataLen = htonl(0);

//     qint64 sent = m_serverSocket->write(reinterpret_cast<char*>(&header), sizeof(PacketHeader));
//     if (sent == sizeof(PacketHeader)) {
//         qDebug() << "[发送心跳] 成功，字节数：" << sent;
//     } else {
//         qDebug() << "[发送心跳] 失败，发送字节数：" << sent;
//         showMessage("系统", "心跳发送失败，连接可能异常！");
//     }
// }

// // 接收服务端数据并处理（核心函数）
// void ChatWindow::onServerReadyRead()
// {
//     // 检查Socket状态
//     if (m_serverSocket->state() != QAbstractSocket::ConnectedState) {
//         qDebug() << "[TCP接收] 错误：Socket未连接，状态：" << m_serverSocket->state();
//         return;
//     }
//     if (m_serverSocket->error() != QAbstractSocket::UnknownSocketError) {
//         QString errMsg = "Socket异常：" + m_serverSocket->errorString();
//         qDebug() << "[TCP接收] 错误：" << errMsg;
//         showMessage("系统", errMsg);
//         return;
//     }

//     // 读取数据到缓存
//     QByteArray newData = m_serverSocket->readAll();
//     if (newData.isEmpty()) {
//         qDebug() << "[TCP接收] 收到空包（正常连接保持）";
//         return;
//     }

//     qDebug() << "[TCP接收] 字节数：" << newData.size()
//              << "，十六进制：" << newData.toHex()
//              << "，缓存总长度：" << m_recvBuffer.size() + newData.size();
//     m_recvBuffer.append(newData);

//     // 循环解析完整数据包（处理粘包/半包）
//     while (m_recvBuffer.size() >= sizeof(PacketHeader)) {
//         // 解析包头（转主机字节序）
//         PacketHeader header;
//         memcpy(&header, m_recvBuffer.data(), sizeof(PacketHeader));
//         header.msgType = ntohl(header.msgType);
//         header.dataLen = ntohl(header.dataLen);

//         qDebug() << "[包头解析] 消息类型：" << header.msgType
//                  << "，数据长度：" << header.dataLen
//                  << "，包头长度：" << sizeof(PacketHeader);

//         // 检查数据包完整性
//         uint32_t totalPacketLen = sizeof(PacketHeader) + header.dataLen;
//         if (m_recvBuffer.size() < totalPacketLen) {
//             qDebug() << "[数据包不完整] 缓存长度：" << m_recvBuffer.size()
//                      << "，需要长度：" << totalPacketLen << "，等待后续数据";
//             break;
//         }

//         // 检查消息类型是否合法（根据协议枚举范围）
//         const uint32_t minMsgType = static_cast<uint32_t>(MsgType::LOGIN_REQ);
//         const uint32_t maxMsgType = static_cast<uint32_t>(MsgType::HEARTBEAT);
//         if (header.msgType < minMsgType || header.msgType > maxMsgType) {
//             qDebug() << "[无效消息] 收到非法msgType：" << header.msgType << "，丢弃数据包";
//             m_recvBuffer.remove(0, totalPacketLen);
//             continue;
//         }

//         // 提取payload数据
//         std::vector<char> payload;
//         if (header.dataLen > 0) {
//             payload.assign(m_recvBuffer.begin() + sizeof(PacketHeader),
//                            m_recvBuffer.begin() + totalPacketLen);
//         }

//         // 移除已处理的数据包
//         m_recvBuffer.remove(0, totalPacketLen);
//         qDebug() << "[payload提取] 长度：" << payload.size() << "，剩余缓存：" << m_recvBuffer.size();

//         // 处理不同类型消息
//         handleMessage(static_cast<MsgType>(header.msgType), payload);
//     }
// }

// // 处理不同类型的消息（分离解析和处理逻辑，便于维护）
// void ChatWindow::handleMessage(MsgType msgType, const std::vector<char>& payload)
// {
//     switch (msgType) {
//         case MsgType::USER_LIST_RSP: {
//             // 在线用户列表响应：停止超时定时器 + 更新UI
//             m_userListTimeoutTimer->stop();
//             try {
//                 std::map<int, UserInfo> users = deserializeUserListToMap(payload);
//                 qDebug() << "[USER_LIST_RSP] 解析成功，在线用户数：" << users.size();
//                 for (const auto& [id, info] : users) {
//                     qDebug() << "  - ID:" << id << "，昵称:" << QString::fromStdString(info.nickname)
//                              << "，IP:" << QString::fromStdString(info.ip)
//                              << "，端口:" << info.dataPort;
//                 }
//                 m_onlineUsers = users;
//                 updateOnlineUsers(users);  // 刷新用户列表UI
//                 showMessage("系统", "在线用户列表加载完成（共" + QString::number(users.size()) + "人）");
//             } catch (const std::exception& e) {
//                 QString errMsg = "解析用户列表失败：" + QString::fromStdString(e.what());
//                 showMessage("系统", errMsg);
//                 qDebug() << "[USER_LIST_RSP] 失败：" << errMsg;
//             }
//             break;
//         }

//         case MsgType::USER_ONLINE_NOTIFY: {
//             // 新用户上线通知：添加到在线列表 + 更新UI
//             try {
//                 size_t offset = 0;  // 添加 offset 变量
//                 UserInfo newUser = deserializeUserInfo(payload, offset);  // 传递两个参数
//                 qDebug() << "[USER_ONLINE_NOTIFY] 新用户上线：ID=" << newUser.userId
//                          << "，昵称=" << QString::fromStdString(newUser.nickname);
//                 m_onlineUsers[newUser.userId] = newUser;
//                 updateOnlineUsers(m_onlineUsers);
//                 showMessage("系统", QString::fromStdString(newUser.nickname) + " 已上线");
//             } catch (const std::exception& e) {
//                 QString errMsg = "解析上线通知失败：" + QString::fromStdString(e.what());
//                 showMessage("系统", errMsg);
//                 qDebug() << "[USER_ONLINE_NOTIFY] 失败：" << errMsg;
//             }
//             break;
//         }

//         case MsgType::USER_OFFLINE_NOTIFY: {
//             // 用户下线通知：从在线列表移除 + 更新UI
//             try {
//                 size_t offset = 0;  // 添加 offset 变量
//                 UserInfo offlineUser = deserializeUserInfo(payload, offset);  // 传递两个参数
//                 qDebug() << "[USER_OFFLINE_NOTIFY] 用户下线：ID=" << offlineUser.userId
//                          << "，昵称=" << QString::fromStdString(offlineUser.nickname);
//                 m_onlineUsers.erase(offlineUser.userId);
//                 updateOnlineUsers(m_onlineUsers);
//                 showMessage("系统", QString::fromStdString(offlineUser.nickname) + " 已下线");
//             } catch (const std::exception& e) {
//                 QString errMsg = "解析下线通知失败：" + QString::fromStdString(e.what());
//                 showMessage("系统", errMsg);
//                 qDebug() << "[USER_OFFLINE_NOTIFY] 失败：" << errMsg;
//             }
//             break;
//         }

//         case MsgType::COMMON_MSG: {
//             // 普通聊天消息：显示到聊天窗口
//             try {
//                 CommonMsg chatMsg = deserializeCommonMsg(payload);
//                 qDebug() << "[COMMON_MSG] 解析成功：发送者ID=" << chatMsg.fromUserId
//                          << "，昵称=" << QString::fromStdString(chatMsg.fromNickname)
//                          << "，目标ID=" << chatMsg.toUserId
//                          << "，内容=" << QString::fromStdString(chatMsg.content);

//                 QString senderName = QString::fromStdString(chatMsg.fromNickname);
//                 if (chatMsg.toUserId == 0) {
//                     senderName += "（广播）";
//                 }
//                 showMessage(senderName, QString::fromStdString(chatMsg.content));
//             } catch (const std::exception& e) {
//                 QString errMsg = "解析聊天消息失败：" + QString::fromStdString(e.what());
//                 showMessage("系统", errMsg);
//                 qDebug() << "[COMMON_MSG] 失败：" << errMsg;
//             }
//             break;
//         }

//         case MsgType::HEARTBEAT: {
//             // 服务端心跳响应：仅日志，无需显示
//             qDebug() << "[HEARTBEAT] 收到服务端心跳回应，连接正常";
//             break;
//         }

//         default: {
//             qDebug() << "[未知消息] 收到未定义消息类型：" << static_cast<uint32_t>(msgType);
//             showMessage("系统", "收到未知类型消息（可能是版本不兼容）");
//             break;
//         }
//     }
// }

// // 更新在线用户列表UI
// void ChatWindow::updateOnlineUsers(const std::map<int, UserInfo>& onlineUsers)
// {
//     if (!m_userListWidget) return;

//     // 保存当前选中的ID（刷新后恢复选中状态）
//     int currentSelectedId = m_selectedUserId;
//     bool hasSelected = false;

//     // 清空列表
//     m_userListWidget->clear();

//     // 1. 添加广播选项（toUserId=0）
//     QListWidgetItem* broadcastItem = new QListWidgetItem("📢 广播");
//     broadcastItem->setData(Qt::UserRole, 0);
//     m_userListWidget->addItem(broadcastItem);
//     if (currentSelectedId == 0) {
//         m_userListWidget->setCurrentItem(broadcastItem);
//         hasSelected = true;
//     }

//     // 2. 添加在线用户（跳过自己）
//     for (const auto& [userId, user] : onlineUsers) {
//         if (userId == m_userId) continue;  // 不显示当前登录用户

//         QString itemText = QString("👤 %1（ID:%2）")
//                                .arg(QString::fromStdString(user.nickname))
//                                .arg(userId);
//         QListWidgetItem* userItem = new QListWidgetItem(itemText);
//         userItem->setData(Qt::UserRole, userId);
//         m_userListWidget->addItem(userItem);

//         // 恢复之前的选中状态
//         if (!hasSelected && userId == currentSelectedId) {
//             m_userListWidget->setCurrentItem(userItem);
//             hasSelected = true;
//         }
//     }

//     // 3. 若之前选中的用户已下线，默认选中广播
//     if (!hasSelected && m_userListWidget->count() > 0) {
//         m_userListWidget->setCurrentRow(0);
//         m_selectedUserId = 0;
//     }
// }

// // 在聊天窗口显示消息（带时间戳）
// void ChatWindow::showMessage(const QString& sender, const QString& content)
// {
//     if (!m_chatDisplay) return;

//     // 格式化消息：[时间] 发送者：内容
//     QString timeStr = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
//     QString msgStr = QString("[%1] %2：%3\n").arg(timeStr).arg(sender).arg(content);

//     // 追加到聊天显示区域
//     m_chatDisplay->append(msgStr);

//     // 自动滚动到底部
//     m_chatDisplay->moveCursor(QTextCursor::End);
// }

// // 处理Socket断开连接
// void ChatWindow::onDisconnected()
// {
//     qDebug() << "[连接状态] 与服务端断开连接";
//     showMessage("系统", "与服务端的连接已断开！");

//     // 停止定时器
//     m_heartbeatTimer->stop();
//     m_userListTimeoutTimer->stop();

//     // 禁用发送功能
//     m_msgInput->setEnabled(false);
//     m_sendBtn->setEnabled(false);

//     // 弹出重连提示
//     int ret = QMessageBox::question(this, "连接断开", "是否尝试重新连接服务端？",
//                                     QMessageBox::Yes | QMessageBox::No);
//     if (ret == QMessageBox::Yes) {
//         // 尝试重连（根据实际需求实现重连逻辑）
//         m_serverSocket->connectToHost(m_serverSocket->peerAddress(), m_serverSocket->peerPort());
//         showMessage("系统", "正在尝试重新连接...");
//     }
// }