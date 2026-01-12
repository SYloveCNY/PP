#include "ChatWindow.h"
#include "ui_ChatWindow.h" 
#include <QVBoxLayout>
#include <QLabel>
#include <QTextEdit>
#include <QLineEdit>
#include <QPushButton>
#include <QHBoxLayout>
#include <QTimer>
#include <QJsonObject>
#include <QJsonDocument>
#include <QFile>
#include <QByteArray>
#include <QCloseEvent>
#include <thread>
#include <QTime>
#include <iostream>
#include "../../include/protocol.h"
#include "../../include/protocol_qt.h"

// ========== 补充缺失的全局变量定义 ==========
static uint32_t nextMsgId = 1;       // 消息ID自增
QTcpSocket* g_clientSocket = nullptr;// 全局客户端Socket
int g_currentUserId = 0;             // 当前登录用户ID

// ========== 实现sendPacket函数（客户端） ==========
bool sendPacket(QTcpSocket* socket, uint32_t msgType, const std::string& data, uint32_t msgId, uint32_t senderId) {
    if (!socket || !socket->isValid()) return false;

    PacketHeader header;
    header.msgType = msgType;
    header.dataLen = static_cast<uint32_t>(data.size());
    header.msgId = msgId;
    header.senderId = senderId;

    // 转换为网络字节序
    PacketHeader netHeader = htonHeader(header);

    // 发送头部+数据
    socket->write(reinterpret_cast<const char*>(&netHeader), sizeof(PacketHeader));
    socket->write(data.c_str(), data.size());
    return socket->flush();
}

// ========== ChatWindow类实现 ==========
ChatWindow::ChatWindow(QWidget *parent)
    : QWidget(parent)
    , m_userId(0)
    , m_socket(nullptr)
    , m_heartbeatTimer(new QTimer(this))
    , ui(new Ui::ChatWindow) // 初始化UI对象
    , m_isPrivateChat(false) // 初始为公聊
{
    // 加载UI文件（替代手动创建UI）
    ui->setupUi(this);
    this->setWindowTitle("聊天窗口");

    // 添加“获取在线用户”按钮到在线用户分组框
    QPushButton* getUserListBtn = new QPushButton("获取在线用户", this);
    ui->groupBox_onlineUsers->layout()->addWidget(getUserListBtn);

    // 添加用户信息标签到聊天区域顶部
    m_userInfoLabel = new QLabel("当前用户：未登录", this);
    ui->verticalLayout->insertWidget(0, m_userInfoLabel); // 插入到聊天框上方

    // 适配UI文件的控件）
    connect(getUserListBtn, &QPushButton::clicked, this, &ChatWindow::getOnlineUserList);
    connect(ui->pushButton_send, &QPushButton::clicked, this, &ChatWindow::sendMessage);
    connect(ui->pushButton_switchChatType, &QPushButton::clicked, this, &ChatWindow::switchChatType);
    connect(m_heartbeatTimer, &QTimer::timeout, this, &ChatWindow::sendHeartbeat);
    
    // 启动心跳定时器
    m_heartbeatTimer->start(10000);
}

ChatWindow::~ChatWindow() {
    delete ui; // 释放UI对象
}

// 新增：主动关闭窗口时发送下线通知
void ChatWindow::closeEvent(QCloseEvent *event) {
    if (m_socket && m_socket->state() == QTcpSocket::ConnectedState) {
        // 停止心跳定时器，避免干扰
        m_heartbeatTimer->stop();
        // 禁用Socket的延迟发送，强制立即发送
        m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);

        // 构造主动下线通知
        UserStatusNotify notify;
        notify.userId = m_userId;
        notify.nickname = m_nickname.toStdString();
        notify.isOnline = false;
        std::string notifyData = serialize(notify);
        
        // 发送主动下线消息（强制发送）
        bool sendOk = sendPacket(m_socket, static_cast<uint32_t>(MsgType::USER_STATUS_NOTIFY), notifyData, nextMsgId++, m_userId);
        if (sendOk) {
            std::cout << "主动下线通知发送成功：ID=" << m_userId << "，数据：" << notifyData << std::endl;
            // 延长等待时间到1秒，确保数据发送完成
            if (m_socket->waitForBytesWritten(1000)) {
                std::cout << "主动下线通知已写入Socket缓冲区" << std::endl;
            } else {
                std::cout << "主动下线通知写入缓冲区超时" << std::endl;
            }
        } else {
            std::cout << "主动下线通知发送失败，Socket状态：" << m_socket->state() << std::endl;
        }
        
        // 断开连接
        m_socket->disconnectFromHost();
        if (m_socket->state() != QTcpSocket::UnconnectedState) {
            m_socket->waitForDisconnected(1000);
        }

        // 清理全局变量
        g_currentUserId = 0;
        g_clientSocket = nullptr;
    }

    // 重置成员变量
    m_userId = 0;
    m_nickname.clear();
    m_socket = nullptr;
    event->accept();
}

// 设置登录信息
void ChatWindow::setLoginInfo(int userId, const QString& nickname, QTcpSocket* socket) {
    m_userId = userId;
    m_nickname = nickname;
    m_socket = socket;
    g_clientSocket = socket;
    g_currentUserId = userId;

    // 更新用户信息
    m_userInfoLabel->setText(QString("当前用户：%1（ID：%2）").arg(nickname).arg(userId));

    // 连接Socket的readyRead信号
    connect(m_socket, &QTcpSocket::readyRead, this, &ChatWindow::onServerReadyRead);
}

// 发送心跳包
void ChatWindow::sendHeartbeat() {
    if (m_socket && m_socket->state() == QTcpSocket::ConnectedState) {
        sendPacket(m_socket, static_cast<uint32_t>(MsgType::HEARTBEAT), "", nextMsgId++, m_userId);
    }
}

// 处理服务器数据
void ChatWindow::onServerReadyRead() {
    QTcpSocket* socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket) return;

    // 循环读取所有可用数据（处理粘包/半包）
    while (socket->bytesAvailable() >= sizeof(PacketHeader)) {
        // 读取头部
        QByteArray headerData = socket->peek(sizeof(PacketHeader));
        if (headerData.size() != sizeof(PacketHeader)) break;

        PacketHeader* netHeader = reinterpret_cast<PacketHeader*>(headerData.data());
        PacketHeader hostHeader = ntohHeader(*netHeader);

        // 检查数据是否完整
            qint64 totalLen = sizeof(PacketHeader) + hostHeader.dataLen;
            if (socket->bytesAvailable() < totalLen) break;

        // 完整读取头部+数据
        socket->read(sizeof(PacketHeader)); // 移除头部数据
        QByteArray data = socket->read(hostHeader.dataLen);
        std::string dataStr = data.toStdString();

        // 处理不同类型的消息
        MsgType msgType = static_cast<MsgType>(hostHeader.msgType);
        switch (msgType) {
            case MsgType::USER_STATUS_NOTIFY: {
                try {
                    UserStatusNotify notify = deserialize<UserStatusNotify>(dataStr);
                    QString statusText = notify.isOnline ? "上线了" : "下线了（主动/意外退出）";
                    QString notifyText = QString("%1（ID：%2）%3")
                        .arg(QString::fromStdString(notify.nickname))
                        .arg(notify.userId)
                        .arg(statusText);
                    showMessage("系统通知", notifyText);
                } catch (std::exception& e) {
                    showMessage("解析错误", QString("解析状态通知失败：%1").arg(e.what()));
                }
                break;
            }
            case MsgType::USER_LIST_RSP: {
                try {
                    nlohmann::json rspJson = nlohmann::json::parse(dataStr);
                    // 【修复4】增加字段存在性检查，避免null错误
                    int userCount = rspJson.contains("userCount") ? rspJson["userCount"].get<int>() : 0;
                    auto usersJson = rspJson.contains("users") ? rspJson["users"] : nlohmann::json::array();

                    std::vector<UserInfo> users;
                    for (auto& userJson : usersJson) {
                        UserInfo user;
                        // 逐个字段检查，避免null转int失败
                        user.userId = userJson.contains("userId") && !userJson["userId"].is_null() ? userJson["userId"].get<int>() : 0;
                        user.nickname = userJson.contains("nickname") && !userJson["nickname"].is_null() ? userJson["nickname"].get<std::string>() : "";
                        user.isOnline = userJson.contains("isOnline") && !userJson["isOnline"].is_null() ? userJson["isOnline"].get<bool>() : false;
                        user.ip = userJson.contains("ip") && !userJson["ip"].is_null() ? userJson["ip"].get<std::string>() : "";
                        user.dataPort = userJson.contains("dataPort") && !userJson["dataPort"].is_null() ? userJson["dataPort"].get<int>() : 0;
                        if (user.userId > 0) { // 过滤无效用户
                            users.push_back(user);
                        }
                    }

                    updateUserList(users);
                    qDebug() << "用户列表解析成功，共" << users.size() << "个用户";
                } catch (std::exception& e) {
                    showMessage("解析错误", QString("解析用户列表失败：%1，原始数据：%2").arg(e.what()).arg(QString::fromStdString(dataStr)));
                }
                break;
            }
            case MsgType::PRIVATE_MSG: {
                try {
                    nlohmann::json msgJson = nlohmann::json::parse(dataStr);
                    std::string senderNickname = msgJson["senderNickname"];
                    std::string content = msgJson["content"];
                    // 显示私聊消息，标记为私聊
                    showMessage(QString::fromStdString(senderNickname) + "(私聊)", QString::fromStdString(content), true);
                } catch (std::exception& e) {
                    showMessage("未知用户", QString::fromStdString(dataStr));
                }
                break;
            }
            case MsgType::PRIVATE_MSG_RSP: {
                try {
                    nlohmann::json rspJson = nlohmann::json::parse(dataStr);
                    bool success = rspJson["success"];
                    std::string msg = rspJson["msg"];
                    showMessage("系统提示", QString::fromStdString(msg));
                } catch (std::exception& e) {
                    showMessage("系统提示", "私聊响应解析失败");
                }
                break;
            }
            case MsgType::COMMON_MSG: {
                try {
                    // 解析服务端转发的JSON消息
                    nlohmann::json msgJson = nlohmann::json::parse(dataStr);
                    std::string senderNickname = msgJson["senderNickname"];
                    std::string content = msgJson["content"];
                    
                    // 显示真实发送方的消息（而非“服务器”）
                    showMessage(QString::fromStdString(senderNickname), QString::fromStdString(content));
                } catch (std::exception& e) {
                    // 兼容旧逻辑：解析失败则显示原始内容
                    showMessage("未知用户", QString::fromStdString(dataStr));
                }
                break;
            }
            case MsgType::HEARTBEAT: { // 新增：处理服务器返回的心跳响应
                showMessage("系统提示", "心跳包响应正常");
                break;
            }
            default:
                showMessage("未知消息", QString("收到未知消息类型：%1").arg((int)msgType));
                break;
        }
    }
}

// 更新用户列表（简化实现）
void ChatWindow::updateUserList(const std::vector<UserInfo>& users) {
    // 清空列表前先确认控件有效
    if (!ui->listWidget_onlineUsers) {
        showMessage("系统提示", "在线用户列表控件未初始化！");
        return;
    }
    ui->listWidget_onlineUsers->clear();
    
    // 遍历添加用户到列表
    for (const auto& user : users) {
        QString ipStr = user.ip.empty() ? "未知" : QString::fromStdString(user.ip);
        QString portStr = (user.dataPort <= 0) ? "无" : QString::number(user.dataPort);
        
        QString itemText = QString("%1（ID：%2，IP：%3，端口：%4）")
            .arg(QString::fromStdString(user.nickname))
            .arg(user.userId)
            .arg(ipStr)
            .arg(portStr);
        
        ui->listWidget_onlineUsers->addItem(itemText);
    }
    
    // 聊天框提示更新结果
    showMessage("系统提示", QString("在线用户列表已更新，当前在线%1人").arg(users.size()));
}

// 处理通知（上线/下线）
void ChatWindow::processNotification(const MsgType& type, const std::string& data) {
    UserStatusNotify notify = deserialize<UserStatusNotify>(data);
    QString notifyStr = QString("%1（ID：%2）%3")
        .arg(QString::fromStdString(notify.nickname))
        .arg(notify.userId)
        .arg(notify.isOnline ? "上线了" : "下线了");
    
    showMessage("系统通知", notifyStr);
}

// 发送普通消息
void ChatWindow::sendMessage() {
    QString content = ui->lineEdit_input->text().trimmed();
    if (content.isEmpty() || !m_socket) return;

    if (m_isPrivateChat) {
        // 私聊逻辑
        bool ok;
        int receiverId = ui->lineEdit_privateId->text().toInt(&ok);
        if (!ok || receiverId <= 0) {
            showMessage("系统提示", "请输入有效的私聊对象ID！");
            return;
        }

        // 构造私聊消息
        PrivateMsg msg;
        msg.senderId = m_userId;
        msg.receiverId = receiverId;
        msg.content = content.toStdString();

        // 序列化
        nlohmann::json j;
        j["senderId"] = msg.senderId;
        j["receiverId"] = msg.receiverId;
        j["content"] = msg.content;
        std::string data = j.dump();

        // 发送私聊消息
        sendPacket(m_socket, static_cast<uint32_t>(MsgType::PRIVATE_MSG), data, nextMsgId++, m_userId);

        // 显示自己发送的私聊消息
        showMessage(m_nickname + "(私聊→" + QString::number(receiverId) + ")", content, true);
    } else {
        // 原有公聊逻辑
        nlohmann::json msgJson;
        msgJson["content"] = content.toStdString();
        std::string data = msgJson.dump();

        sendPacket(m_socket, static_cast<uint32_t>(MsgType::COMMON_MSG), data, nextMsgId++, m_userId);
        showMessage(m_nickname, content);
    }

    ui->lineEdit_input->clear();
}

// 打开图片选择对话框并发送
void ChatWindow::sendImageDialog() {
    // 简化实现：模拟发送图片
    QString filePath = "/test.jpg"; // 实际项目中用QFileDialog选择
    sendImage(filePath);
}

// 发送图片
void ChatWindow::sendImage(const QString& filePath) {
    if (!m_socket) return;

    ImageMsg imgMsg;
    imgMsg.senderId = m_userId;
    imgMsg.receiverId = 0; // 广播
    imgMsg.fileName = filePath.toStdString();
    imgMsg.fileSize = std::to_string(1024); // 模拟大小
    imgMsg.base64Data = "dummy_base64_data"; // 模拟Base64数据

    // 序列化图片消息
    std::string jsonData = serialize(imgMsg);
    sendPacket(m_socket, static_cast<uint32_t>(MsgType::IMAGE_MSG), jsonData, nextMsgId++, m_userId);

    showMessage(m_nickname, QString("发送图片：%1").arg(filePath));
}

// 显示聊天消息
void ChatWindow::showMessage(const QString& sender, const QString& content, bool isPrivate) {
    QString prefix = isPrivate ? "[私聊] " : "";
    QString msg = QString("[%1] %2%3：%4").arg(QTime::currentTime().toString())
        .arg(prefix)
        .arg(sender)
        .arg(content);
    ui->textEdit_chatLog->append(msg);
}

// 获取在线用户
void ChatWindow::getOnlineUserList() {
    if (!m_socket || m_socket->state() != QTcpSocket::ConnectedState) {
        showMessage("系统提示", "未连接到服务器，无法获取在线用户！");
        return;
    }

    // 发送USER_LIST_REQ（msgType=3），无数据体
    bool ret = sendPacket(
        m_socket, 
        static_cast<uint32_t>(MsgType::USER_LIST_REQ), 
        "",  // 无数据体
        nextMsgId++, 
        m_userId
    );

    if (ret) {
        showMessage("系统提示", "已发送在线用户列表请求，请等待响应...");
    } else {
        showMessage("系统提示", "发送在线用户列表请求失败！");
    }
}

// 切换聊天类型函数
void ChatWindow::switchChatType() {
    m_isPrivateChat = !m_isPrivateChat;
    if (m_isPrivateChat) {
        ui->pushButton_switchChatType->setText("私聊");
        ui->lineEdit_privateId->setEnabled(true); // 启用私聊ID输入
    } else {
        ui->pushButton_switchChatType->setText("公聊");
        ui->lineEdit_privateId->setEnabled(false); // 禁用私聊ID输入
    }
}