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
#include <QFileDialog>  // 补充：文件选择对话框
#include <QByteArray>
#include <QCloseEvent>
#include <thread>
#include <QThread>
#include <QTime>
#include <QRegularExpression>
#include <QDir>         // 补充：目录操作
#include <QCryptographicHash> // 补充：MD5校验
#include <QMetaObject>  // 补充：跨线程调用
#include <QEventLoop>   // 补充：事件循环
#include <QUdpSocket>   // 补充：UDP
#include <QTcpServer>   // 补充：TCP Server
#include <QTcpSocket>   // 补充：TCP Socket
#include <QAbstractSocket> // 补充：Socket错误处理
#include <iostream>
#include "../../include/protocol.h"
#include "../../include/protocol_qt.h"

using namespace nlohmann;

// ========== 补充缺失的全局变量定义 ==========
static uint32_t nextMsgId = 1;       // 消息ID自增
QTcpSocket* g_clientSocket = nullptr;// 全局客户端Socket
int g_currentUserId = 0;             // 当前登录用户ID

// ========== ChatWindow类实现 ==========
ChatWindow::ChatWindow(QWidget *parent)
    : QWidget(parent)
    , m_userId(0)
    , m_socket(nullptr)
    , m_heartbeatTimer(new QTimer(this))
    , ui(new Ui::ChatWindow) // 初始化UI对象
    , m_isPrivateChat(false) // 初始为公聊
    , m_tcpP2PServer(new QTcpServer(this))
    , m_tcpP2PSocket(nullptr)
    , m_udpP2PSocket(nullptr) // 初始化为nullptr
    , m_transferFile(nullptr)
    , m_totalFileSize(0)
    , m_sentFileSize(0)
    , m_userInfoLabel(nullptr) // 显式初始化标签指针
    , m_selectedUserId(0)
    , m_p2pTcpPort(0)
    , m_udpP2PPort(0)
    // 新增：初始化中继相关变量
    , m_relayServerIp("120.239.202.94") // 你的公网IP
    , m_relayTcpPort(8001)              // 中继TCP端口
    , m_relayUdpPort(8002)              // 中继UDP端口
    , m_udpRelaySocket(nullptr)
    , m_tcpRelaySocket(nullptr)
{
    // 加载UI文件（替代手动创建UI）
    ui->setupUi(this);
    this->setWindowTitle("聊天窗口");

    // ========== 1. 先初始化必要的UI组件 ==========
    m_userInfoLabel = new QLabel("当前用户：未登录", this);
    ui->verticalLayout->insertWidget(0, m_userInfoLabel); // 插入到聊天框上方

    // ========== 2. 初始化P2P Socket（先创建对象，再操作） ==========
    m_tcpP2PSocket = new QTcpSocket(this);   // 点对点TCP Socket
    m_udpP2PSocket = new QUdpSocket(this);   // 点对点UDP Socket

    // ========== 新增：初始化中继服务器Socket ==========
    m_udpRelaySocket = new QUdpSocket(this); // 中继UDP Socket
    m_tcpRelaySocket = new QTcpSocket(this); // 中继TCP Socket

    m_udpRelaySocket->bind(QHostAddress::Any); // 系统自动分配可用端口
    showMessage("系统提示", QString("中继UDP Socket绑定端口：%1").arg(m_udpRelaySocket->localPort()));

    // 连接中继TCP服务器
    m_tcpRelaySocket->connectToHost(m_relayServerIp, m_relayTcpPort);
    // 绑定中继相关信号槽
    connect(m_tcpRelaySocket, &QTcpSocket::connected, this, [=]() {
        showMessage("系统提示", "已连接到公网中继服务器（TCP）");
    });
    connect(m_tcpRelaySocket, &QTcpSocket::disconnected, this, [=]() {
        showMessage("系统提示", "与公网中继服务器（TCP）断开连接，正在重连...");
        // 自动重连逻辑
        QTimer::singleShot(3000, this, [=]() {
            m_tcpRelaySocket->connectToHost(m_relayServerIp, m_relayTcpPort);
        });
    });
    connect(m_tcpRelaySocket, &QTcpSocket::errorOccurred, this, [=](QAbstractSocket::SocketError error) {
        showMessage("系统错误", QString("中继TCP连接失败：%1，3秒后重连").arg(m_tcpRelaySocket->errorString()));
        QTimer::singleShot(3000, this, [=]() {
            m_tcpRelaySocket->connectToHost(m_relayServerIp, m_relayTcpPort);
        });
    });
    connect(m_tcpRelaySocket, &QTcpSocket::readyRead, this, &ChatWindow::onRelayTcpReadyRead);
    connect(m_udpRelaySocket, &QUdpSocket::readyRead, this, &ChatWindow::onRelayUdpReadyRead);

    // ========== 3. 先绑定TCP服务器信号，再启动监听（规范顺序） ==========
    connect(m_tcpP2PServer, &QTcpServer::newConnection, 
            this, &ChatWindow::onP2PNewConnection);

    // ========== 4. 启动TCP P2P服务器（区间自动选固定端口） ==========
    const int P2P_PORT_START = 9000; // 端口区间起始（可自定义）
    const int P2P_PORT_END   = 9100; // 端口区间结束（可自定义）
    int selectedP2PPort = 0;

    // 从区间里逐个尝试监听，找到第一个可用端口（固定后永不变化）
    for (int port = P2P_PORT_START; port <= P2P_PORT_END; ++port) {
        if (m_tcpP2PServer->listen(QHostAddress::Any, port)) {
            selectedP2PPort = port;
            break;
        }
    }

    // 处理监听结果（容错+日志）
    if (selectedP2PPort == 0) {
        showMessage("系统错误", QString("P2P TCP服务器启动失败：%1-%2端口全被占用！").arg(P2P_PORT_START).arg(P2P_PORT_END));
    } else {
        // 保存固定端口（从此不再变化）
        m_p2pTcpPort = selectedP2PPort;
        showMessage("系统提示", QString("P2P TCP服务器已启动，固定监听端口：%1").arg(m_p2pTcpPort));
    }

    // ========== 5. UDP相关逻辑（修改为和TCP一样的端口区间遍历模式） ==========
    int selectedUdpPort = 0;
    // 遍历指定端口区间，找第一个可用的UDP端口
    for (int port = P2P_PORT_START; port <= P2P_PORT_END; ++port) {
        // 注意：UDP的bind和TCP的listen参数一致，但逻辑不同（UDP无监听，仅绑定端口）
        if (m_udpP2PSocket->bind(QHostAddress::Any, port)) {
            selectedUdpPort = port;
            break;
        }
    }

    // 处理UDP绑定结果（容错+日志）
    if (selectedUdpPort == 0) {
        showMessage("系统错误", QString("P2P UDP绑定失败：%1-%2端口全被占用！").arg(P2P_PORT_START).arg(P2P_PORT_END));
    } else {
        // 保存固定UDP端口
        m_udpP2PPort = selectedUdpPort;
        showMessage("系统提示", QString("P2P UDP端口绑定成功，固定端口：%1").arg(m_udpP2PPort));
        // 绑定UDP接收信号槽
        connect(m_udpP2PSocket, &QUdpSocket::readyRead, this, &ChatWindow::onUdpReadyRead);
    }

    // ========== 6. 进度条初始化 ==========
    ui->progressBar_transfer->setRange(0, 100); // 进度范围0-100
    ui->progressBar_transfer->setValue(0);     // 初始进度0

    // ========== 7. 信号槽绑定（原有+新增） ==========
    // 原有绑定
    connect(ui->pushButton_getUserList, &QPushButton::clicked, this, &ChatWindow::getOnlineUserList);
    connect(ui->pushButton_send, &QPushButton::clicked, this, &ChatWindow::sendMessage);
    connect(ui->pushButton_switchChatType, &QPushButton::clicked, this, &ChatWindow::switchChatType);
    connect(m_heartbeatTimer, &QTimer::timeout, this, &ChatWindow::sendHeartbeat);
    
    // 新增：绑定图片/文件发送按钮点击事件
    connect(ui->pushButton_sendImage, &QPushButton::clicked, this, 
    static_cast<void (ChatWindow::*)()>(&ChatWindow::sendImage));
    connect(ui->pushButton_sendFile, &QPushButton::clicked, this, 
    static_cast<void (ChatWindow::*)()>(&ChatWindow::sendFile));

    connect(ui->listWidget_onlineUsers, &QListWidget::itemClicked, this, &ChatWindow::onUserItemClicked);

    // 启动心跳定时器
    m_heartbeatTimer->start(10000);
}

ChatWindow::~ChatWindow() {
    // 安全释放Socket
    if (m_p2pClientSocket) {
        m_p2pClientSocket->disconnectFromHost();
        m_p2pClientSocket->deleteLater();
    }
    if (m_tcpP2PSocket) {
        m_tcpP2PSocket->disconnectFromHost();
        m_tcpP2PSocket->deleteLater();
    }
    if (m_udpP2PSocket) {
        m_udpP2PSocket->close();
        m_udpP2PSocket->deleteLater();
    }
    if (m_tcpP2PServer) {
        m_tcpP2PServer->close();
        m_tcpP2PServer->deleteLater();
    }
    delete ui; // 释放UI对象
}

// 修正sendLoginReq函数
void ChatWindow::sendLoginReq(const QString& nickname) {
    // 前置校验：端口必须有效
    if (m_p2pTcpPort == 0) {
        showMessage("系统错误", "P2P端口未初始化，无法发送登录请求");
        return;
    }
    
    LoginReq req;
    req.nickname = nickname.toStdString();
    req.dataPort = m_p2pTcpPort; // 使用初始化后的TCP端口
    req.udpPort = m_udpP2PPort;  // 使用初始化后的UDP端口
    
    std::string jsonStr = nlohmann::json(req).dump();
    sendPacket(m_socket, static_cast<uint32_t>(MsgType::LOGIN_REQ), jsonStr, nextMsgId++, g_currentUserId);
    // 关键：本地缓存自己的端口，用于校验
    m_userPortMap[m_userId] = QString::number(m_p2pTcpPort);
    showMessage("系统提示", QString("登录请求已发送，携带P2P端口：TCP=%1，UDP=%2").arg(m_p2pTcpPort).arg(m_udpP2PPort));
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

    // 关键：打印本地监听端口，确认和登录时传递的一致
    m_userInfoLabel->setText(QString("当前用户：%1（ID：%2，P2P监听端口：%3）")
                             .arg(nickname).arg(userId).arg(m_p2pTcpPort));
    showMessage("系统提示", QString("登录成功！本地P2P监听端口：%1，请确认服务端存储的是该端口").arg(m_p2pTcpPort));

    connect(m_socket, &QTcpSocket::readyRead, this, &ChatWindow::onServerReadyRead);
}

// 发送心跳包
void ChatWindow::sendHeartbeat()
{
    // 前置检查：仅当已登录+和服务端连接正常时发送心跳
    if (m_userId <= 0 || !m_socket || m_socket->state() != QTcpSocket::ConnectedState) {
        qDebug() << "心跳包发送跳过：未登录或服务端连接断开";
        return;
    }

    // 1. 获取最新的P2P端口（适配你的成员变量）
    uint16_t currentTcpPort = 0;
    // 优先用m_tcpP2PServer的监听端口，其次用缓存的m_p2pTcpPort
    if (m_tcpP2PServer && m_tcpP2PServer->isListening()) {
        currentTcpPort = m_tcpP2PServer->serverPort();
        m_p2pTcpPort = currentTcpPort; // 更新缓存的端口
    } else if (m_p2pTcpPort > 0) {
        currentTcpPort = m_p2pTcpPort; // 用缓存的端口
    }

    uint16_t currentUdpPort = 0;
    if (m_udpP2PSocket && m_udpP2PSocket->state() == QUdpSocket::BoundState) {
        currentUdpPort = m_udpP2PSocket->localPort();
        m_udpP2PPort = currentUdpPort; // 更新缓存的UDP端口
    } else if (m_udpP2PPort > 0) {
        currentUdpPort = m_udpP2PPort;
    }

    // 2. 构造心跳包数据（JSON格式，携带端口）
    nlohmann::json heartbeatJson;
    heartbeatJson["dataPort"] = currentTcpPort; // TCP P2P端口
    heartbeatJson["udpPort"] = currentUdpPort;  // UDP P2P端口
    std::string heartbeatData = heartbeatJson.dump();

    // 3. 构造协议头部（按你的protocol.h定义）
    PacketHeader header;
    header.msgType = static_cast<uint32_t>(MsgType::HEARTBEAT); // 心跳消息类型
    header.dataLen = heartbeatData.size();                     // 数据体长度
    header.msgId = 0;                                          // 无消息ID则填0
    header.senderId = m_userId;                                // 当前登录用户ID

    // 4. 转换为网络字节序（调用你封装的htonHeader函数）
    PacketHeader netHeader = htonHeader(header);

    // 5. 发送数据（先头部，后数据体）
    qint64 headerSent = m_socket->write((char*)&netHeader, sizeof(PacketHeader));
    qint64 dataSent = m_socket->write(heartbeatData.c_str(), heartbeatData.size());

    // 调试输出（可选，方便排查）
    qDebug() << "[心跳包] 发送成功 | "
             << "用户ID=" << m_userId << " | "
             << "TCP端口=" << currentTcpPort << " | "
             << "UDP端口=" << currentUdpPort << " | "
             << "发送字节数=" << headerSent + dataSent;
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
            case MsgType::P2P_ADDR_NOTIFY: {
                try {
                    P2PAddrNotify notify = deserializeP2PAddrNotify(dataStr);
                    m_targetIp = QHostAddress(QString::fromStdString(notify.targetIp));
                    m_targetTcpPort = notify.targetTcpPort;
                    m_targetUdpPort = notify.targetUdpPort;
                    
                    showMessage("系统提示", QString("获取点对点地址成功：%1（TCP：%2，UDP：%3）")
                                .arg(m_targetIp.toString())
                                .arg(m_targetTcpPort)
                                .arg(m_targetUdpPort));
                } catch (std::exception& e) {
                    showMessage("解析错误", QString("解析点对点地址失败：%1").arg(e.what()));
                }
                break;
            }
            // 新增图片/文件消息处理
            case MsgType::IMAGE_MSG: {
                try {
                    ImageMsg imgMsg = deserializeImageMsg(dataStr);
                    // 核心修复1：立即缓存图片元信息到成员变量（m_pendingImageMsg）
                    m_pendingImageMsg = imgMsg;
                    m_pendingImageData.clear(); // 清空旧数据，避免干扰
                    
                    showMessage("系统提示", QString("收到图片消息：%1（大小：%2字节），已缓存元信息，等待P2P连接...")
                                .arg(QString::fromStdString(imgMsg.fileName))
                                .arg(imgMsg.fileSize));
                    
                } catch (std::exception& e) {
                    showMessage("解析错误", QString("解析图片消息失败：%1").arg(e.what()));
                }
                break;
            }
            case MsgType::IMAGE_MSG_RSP: { // 新增：处理图片响应
                try {
                    ImageMsgRsp rsp = deserializeImageMsgRsp(dataStr);
                    QString msg = rsp.success ? 
                        QString("图片发送成功（接收方ID：%1）").arg(rsp.receiverId) : 
                        QString("图片发送失败：%1").arg(QString::fromStdString(rsp.msg));
                    showMessage("系统提示", msg);
                } catch (std::exception& e) {
                    showMessage("解析错误", QString("解析图片响应失败：%1").arg(e.what()));
                }
                break;
            }
            case MsgType::FILE_MSG: {
                try {
                    FileMsg fileMsg = deserializeFileMsg(dataStr);
                    showMessage("系统提示", QString("收到文件消息：%1（大小：%2字节），准备接收...")
                                .arg(QString::fromStdString(fileMsg.fileName))
                                .arg(fileMsg.fileSize));
                    
                    // 启动接收文件线程
                    std::thread recvFileThread(&ChatWindow::receiveFile, this, fileMsg);
                    recvFileThread.detach();
                } catch (std::exception& e) {
                    showMessage("解析错误", QString("解析文件消息失败：%1").arg(e.what()));
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
        
        // 缓存用户ID和端口
        if (user.userId > 0) {
            m_userPortMap[user.userId] = portStr;
        }
        
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
    QString filePath = QFileDialog::getOpenFileName(this, "选择图片", "", 
        "图片文件 (*.jpg *.jpeg *.png *.bmp *.gif);;所有文件 (*.*)");
    if (!filePath.isEmpty()) {
        sendImage(filePath);
    }
}

// 发送图片
void ChatWindow::sendImage(const QString& filePath) {
    // 第一步：优先获取有效接收方ID（兜底逻辑）
    int receiverId = m_selectedUserId;
    
    // 兜底：如果列表选择失败，从私聊ID输入框读取
    bool ok;
    int inputReceiverId = ui->lineEdit_privateId->text().toInt(&ok);
    if (ok && inputReceiverId > 0 && inputReceiverId != m_userId) {
        receiverId = inputReceiverId;
        // 同步更新m_selectedUserId，保持一致性
        m_selectedUserId = receiverId;
    }
    
    // 校验接收方ID是否有效
    if (receiverId <= 0 || receiverId == m_userId) {
        showMessage("系统提示", "请先选择有效的接收方（不能是自己）！");
        return;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        showMessage("系统提示", "打开图片失败：" + file.errorString());
        return;
    }
    QByteArray imgData = file.readAll();
    file.close();

    // 构造ImageMsg
    ImageMsg imgMsg;
    imgMsg.senderId = g_currentUserId;
    imgMsg.receiverId = receiverId;
    imgMsg.fileName = filePath.toStdString();
    imgMsg.fileSize = imgData.size();
    imgMsg.protocol = TransportProtocol::TCP;

    // 1. 先发送元信息到服务端（触发P2P_ADDR_NOTIFY）
    std::string jsonStr = serializeImageMsg(imgMsg);
    sendPacket(m_socket, static_cast<uint32_t>(MsgType::IMAGE_MSG), jsonStr, nextMsgId++, g_currentUserId);

    // 2. 等待P2P地址（最多等3秒）
    int waitCount = 0;
    m_targetIp = QHostAddress();
    m_targetTcpPort = 0;
    m_targetUdpPort = 0;
    showMessage("系统提示", "等待服务端返回接收方P2P地址...");
    while (m_targetIp.isNull() && waitCount < 30) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        waitCount++;
    }

    if (m_targetIp.isNull() || m_targetTcpPort == 0) {
        showMessage("系统错误", "获取P2P地址超时或地址无效，无法发送图片");
        return;
    }
    showMessage("系统提示", QString("准备连接接收方P2P端口：%1:%2").arg(m_targetIp.toString()).arg(m_targetTcpPort));

    // ========== 核心修改：判断是否走中继 ==========
    bool useRelay = isPrivateIp(m_targetIp);
    if (useRelay) {
        showMessage("系统提示", "检测到目标为内网IP，将通过公网中继发送图片");
    }

    // 3. 建立连接并发送图片（区分中继/直连）
    bool sendOk = false;
    if (useRelay) {
        // 内网：走中继TCP连接
        if (m_tcpRelaySocket->state() == QTcpSocket::ConnectedState) {
            m_tcpRelaySocket->disconnectFromHost();
            m_tcpRelaySocket->waitForDisconnected(1000);
        }
        m_tcpRelaySocket->abort();

        // 连接中继服务器（重试3次）
        for (int retry = 0; retry < 3; retry++) {
            m_tcpRelaySocket->connectToHost(m_relayServerIp, m_relayTcpPort);
            if (m_tcpRelaySocket->waitForConnected(2000)) {
                // 拼接中继数据：senderId|receiverId|图片数据
                QString relayData = QString("%1|%2|%3")
                    .arg(g_currentUserId)
                    .arg(receiverId)
                    .arg(QString(imgData.toBase64())); // Base64避免特殊字符问题
                
                // 发送到中继
                qint64 sent = m_tcpRelaySocket->write(relayData.toUtf8());
                if (sent > 0) {
                    m_tcpRelaySocket->flush();
                    showMessage("系统提示", QString("图片数据已发送至中继服务器：%1字节").arg(sent));
                    sendOk = true;
                } else {
                    showMessage("系统错误", "中继发送图片失败：" + m_tcpRelaySocket->errorString());
                }
                break;
            }
            showMessage("系统提示", QString("连接中继服务器失败（第%1次重试）：%2").arg(retry+1).arg(m_tcpRelaySocket->errorString()));
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    } else {
        // 公网：原有P2P直连逻辑
        if (m_tcpP2PSocket->state() == QTcpSocket::ConnectedState) {
            m_tcpP2PSocket->disconnectFromHost();
            m_tcpP2PSocket->waitForDisconnected(1000);
        }
        m_tcpP2PSocket->abort();

        // 连接重试：最多3次
        bool connectOk = false;
        for (int retry = 0; retry < 3; retry++) {
            m_tcpP2PSocket->connectToHost(m_targetIp, m_targetTcpPort);
            if (m_tcpP2PSocket->waitForConnected(2000)) { // 2秒超时
                connectOk = true;
                break;
            }
            showMessage("系统提示", QString("P2P连接失败（第%1次重试）：%2").arg(retry+1).arg(m_tcpP2PSocket->errorString()));
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        if (!connectOk) {
            showMessage("系统错误", "P2P连接失败（已重试3次），无法发送图片");
            return;
        }

        // 发送图片数据
        qint64 sent = m_tcpP2PSocket->write(imgData);
        if (sent > 0) {
            m_tcpP2PSocket->flush();
            // 等待数据真正发出去，再关闭
            m_tcpP2PSocket->waitForBytesWritten(2000);
            showMessage("系统提示", QString("图片数据发送成功：%1字节").arg(sent));
            sendOk = true;
        }

        // 关闭P2P连接
        QTimer::singleShot(200, this, [=]() {
            m_tcpP2PSocket->disconnectFromHost();
        });
    }

    if (!sendOk) {
        showMessage("系统错误", "图片发送最终失败，请检查中继服务器或网络");
    }
}

void ChatWindow::sendImage() {
    sendImageDialog();
}

// 发送文件
void ChatWindow::sendFile() {
    QString filePath = QFileDialog::getOpenFileName(this, "选择文件", "", "所有文件 (*.*)");
    if (filePath.isEmpty()) return;
    
    // 关键修改：动态分配QFile，避免局部变量销毁导致野指针
    if (m_transferFile) { // 先释放旧文件
        if (m_transferFile->isOpen()) m_transferFile->close();
        delete m_transferFile;
        m_transferFile = nullptr;
    }
    m_transferFile = new QFile(filePath, this); // 父对象设为ChatWindow，自动管理内存
    if (!m_transferFile->open(QIODevice::ReadOnly)) {
        showMessage("系统提示", "打开文件失败：" + m_transferFile->errorString());
        delete m_transferFile;
        m_transferFile = nullptr;
        return;
    }
    
    // 获取文件信息
    m_totalFileSize = m_transferFile->size();
    m_sentFileSize = 0;
    
    // 计算MD5
    QCryptographicHash hash(QCryptographicHash::Md5);
    if (hash.addData(m_transferFile)) {
        QString fileMd5 = hash.result().toHex();
        
        // 构造文件元信息
        FileMsg fileMsg;
        fileMsg.senderId = m_userId;
        fileMsg.receiverId = ui->lineEdit_privateId->text().toInt();
        // 校验接收方ID
        if (fileMsg.receiverId <= 0 || fileMsg.receiverId == m_userId) {
            showMessage("系统提示", "请输入有效的接收方ID（不能是自己）！");
            m_transferFile->close();
            delete m_transferFile;
            m_transferFile = nullptr;
            return;
        }
        fileMsg.fileName = filePath.toStdString();
        fileMsg.fileSize = m_totalFileSize;
        fileMsg.fileData = ""; // 元信息分片无数据
        fileMsg.fragmentIdx = 0;
        fileMsg.totalFragments = (m_totalFileSize + 1024 * 1024 - 1) / (1024 * 1024); // 1MB分片
        fileMsg.protocol = (ui->combo_protocol->currentText() == "TCP") 
            ? TransportProtocol::TCP : TransportProtocol::UDP;
        fileMsg.fileMd5 = fileMd5.toStdString();
        
        // 发送元信息到服务端
        std::string metaData = serialize(fileMsg);
        sendPacket(m_socket, static_cast<uint32_t>(MsgType::FILE_MSG), metaData, nextMsgId++, m_userId);
        
        // 启动分片发送线程
        std::thread sendFileThread(&ChatWindow::sendFileFragment, this, fileMsg);
        sendFileThread.detach();
    } else {
        showMessage("系统提示", "计算文件MD5失败");
        m_transferFile->close();
        delete m_transferFile;
        m_transferFile = nullptr;
    }
}

// 分片发送文件
void ChatWindow::sendFileFragment(FileMsg metaMsg) {
    if (!m_transferFile || !m_transferFile->isOpen()) {
        QMetaObject::invokeMethod(this, "showMessage",
            Qt::QueuedConnection,
            Q_ARG(QString, "系统错误"),
            Q_ARG(QString, "文件未打开，无法发送分片"));
        return;
    }

    // 等待P2P地址就绪（关键！发送前必须确保目标地址有效）
    int waitCount = 0;
    while (m_targetIp.isNull() || (metaMsg.protocol == TransportProtocol::TCP && m_targetTcpPort == 0) || 
           (metaMsg.protocol == TransportProtocol::UDP && m_targetUdpPort == 0)) {
        if (waitCount >= 30) { // 最多等3秒
            QMetaObject::invokeMethod(this, "showMessage",
                Qt::QueuedConnection,
                Q_ARG(QString, "系统错误"),
                Q_ARG(QString, "获取P2P地址超时，无法发送文件分片"));
            m_transferFile->close();
            return;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        waitCount++;
    }

    const qint64 fragmentSize = 1024 * 1024; // 1MB分片
    qint64 offset = 0;
    bool sendSuccess = true;

    for (uint32_t idx = 1; idx <= metaMsg.totalFragments && sendSuccess; idx++) {
        // 读取分片数据
        m_transferFile->seek(offset);
        QByteArray fragmentData = m_transferFile->read(fragmentSize);
        if (fragmentData.isEmpty() && idx != metaMsg.totalFragments) {
            QMetaObject::invokeMethod(this, "showMessage",
                Qt::QueuedConnection,
                Q_ARG(QString, "系统错误"),
                Q_ARG(QString, QString("读取分片%1失败").arg(idx)));
            sendSuccess = false;
            break;
        }

        // 构造分片消息
        FileMsg fragMsg = metaMsg;
        fragMsg.fragmentIdx = idx;
        fragMsg.fileData = fragmentData.toBase64().toStdString();
        fragMsg.dataLen = fragmentData.size();
        std::string fragData = serialize(fragMsg);

        // 发送分片
        if (metaMsg.protocol == TransportProtocol::TCP) {
            // ========== 新增：TCP内网穿透逻辑 ==========
            if (isPrivateIp(m_targetIp)) {
                // 目标是内网IP → 走中继服务器
                if (m_tcpRelaySocket->state() != QTcpSocket::ConnectedState) {
                    m_tcpRelaySocket->abort();
                    m_tcpRelaySocket->connectToHost(m_relayServerIp, m_relayTcpPort);
                    if (!m_tcpRelaySocket->waitForConnected(2000)) {
                        QMetaObject::invokeMethod(this, "showMessage",
                            Qt::QueuedConnection,
                            Q_ARG(QString, "系统错误"),
                            Q_ARG(QString, QString("连接中继TCP服务器失败：%1").arg(m_tcpRelaySocket->errorString())));
                        sendSuccess = false;
                        break;
                    }
                }

                // 拼接中继数据格式：senderId|receiverId|分片数据
                QString relayData = QString("%1|%2|%3")
                    .arg(m_userId)
                    .arg(metaMsg.receiverId)
                    .arg(QString::fromStdString(fragData));
                
                // 发送到中继服务器
                qint64 sent = m_tcpRelaySocket->write(relayData.toUtf8());
                if (sent <= 0 || !m_tcpRelaySocket->waitForBytesWritten(1000)) {
                    QMetaObject::invokeMethod(this, "showMessage",
                        Qt::QueuedConnection,
                        Q_ARG(QString, "系统错误"),
                        Q_ARG(QString, QString("发送TCP分片%1至中继服务器失败").arg(idx)));
                    sendSuccess = false;
                    break;
                }

                QMetaObject::invokeMethod(this, "showMessage",
                    Qt::QueuedConnection,
                    Q_ARG(QString, "系统提示"),
                    Q_ARG(QString, QString("TCP分片%1已发送至中继服务器").arg(idx)));
            } else {
                // 目标是公网IP → 原有直连逻辑
                if (m_tcpP2PSocket->state() != QTcpSocket::ConnectedState) {
                    m_tcpP2PSocket->abort();
                    m_tcpP2PSocket->connectToHost(m_targetIp, m_targetTcpPort);
                    if (!m_tcpP2PSocket->waitForConnected(2000)) {
                        QMetaObject::invokeMethod(this, "showMessage",
                            Qt::QueuedConnection,
                            Q_ARG(QString, "系统错误"),
                            Q_ARG(QString, QString("连接接收方TCP端口失败：%1").arg(m_tcpP2PSocket->errorString())));
                        sendSuccess = false;
                        break;
                    }
                }

                // 发送数据并确保写入
                qint64 sent = m_tcpP2PSocket->write(fragData.c_str(), fragData.size());
                if (sent <= 0 || !m_tcpP2PSocket->waitForBytesWritten(1000)) {
                    QMetaObject::invokeMethod(this, "showMessage",
                        Qt::QueuedConnection,
                        Q_ARG(QString, "系统错误"),
                        Q_ARG(QString, QString("发送分片%1失败").arg(idx)));
                    sendSuccess = false;
                    break;
                }
            }
        } else {
            // ========== 新增：UDP内网穿透逻辑 ==========
            if (isPrivateIp(m_targetIp)) {
                // 目标是内网IP → 走中继服务器
                // 拼接中继数据格式：senderId|receiverId|分片数据
                QString relayData = QString("%1|%2|%3")
                    .arg(m_userId)
                    .arg(metaMsg.receiverId)
                    .arg(QString::fromStdString(fragData));
                
                // 发送到中继服务器
                qint64 sent = m_udpRelaySocket->writeDatagram(
                    relayData.toUtf8(),
                    QHostAddress(m_relayServerIp),
                    m_relayUdpPort
                );
                
                if (sent <= 0) {
                    QMetaObject::invokeMethod(this, "showMessage",
                        Qt::QueuedConnection,
                        Q_ARG(QString, "系统错误"),
                        Q_ARG(QString, QString("发送UDP分片%1至中继服务器失败").arg(idx)));
                    sendSuccess = false;
                    break;
                }

                QMetaObject::invokeMethod(this, "showMessage",
                    Qt::QueuedConnection,
                    Q_ARG(QString, "系统提示"),
                    Q_ARG(QString, QString("UDP分片%1已发送至中继服务器").arg(idx)));
            } else {
                // 目标是公网IP → 原有直连逻辑
                qint64 sent = m_udpP2PSocket->writeDatagram(fragData.c_str(), fragData.size(), m_targetIp, m_targetUdpPort);
                if (sent <= 0) {
                    QMetaObject::invokeMethod(this, "showMessage",
                        Qt::QueuedConnection,
                        Q_ARG(QString, "系统错误"),
                        Q_ARG(QString, QString("发送UDP分片%1失败").arg(idx)));
                    sendSuccess = false;
                    break;
                }
            }
        }

        // 更新进度
        QMutexLocker locker(&m_fileTransferMutex);
        m_sentFileSize += fragmentData.size();
        int progress = static_cast<int>((m_sentFileSize * 100) / m_totalFileSize);
        QMetaObject::invokeMethod(this, "updateTransferProgress",
            Qt::QueuedConnection,
            Q_ARG(int, progress));

        offset += fragmentSize;
        QThread::msleep(10); // 避免发送过快
    }

    // 发送完成/失败提示
    if (sendSuccess) {
        QMetaObject::invokeMethod(this, "showMessage",
            Qt::QueuedConnection,
            Q_ARG(QString, m_nickname),
            Q_ARG(QString, QString("文件 %1 发送完成（MD5：%2）").arg(QString::fromStdString(metaMsg.fileName)).arg(QString::fromStdString(metaMsg.fileMd5))));
    } else {
        QMetaObject::invokeMethod(this, "showMessage",
            Qt::QueuedConnection,
            Q_ARG(QString, "系统错误"),
            Q_ARG(QString, QString("文件 %1 发送失败").arg(QString::fromStdString(metaMsg.fileName))));
    }

    m_transferFile->close();
}

// 更新传输进度
void ChatWindow::updateTransferProgress(int progress) {
    ui->progressBar_transfer->setValue(progress);
}

// ========== 新增：中继数据处理入口（优先处理） ==========
// 该函数供onRelayTcpReadyRead/onRelayUdpReadyRead调用，处理单个中继分片
void ChatWindow::processRelayFragment(int senderId, const FileMsg& fragMsg) {
    int realSenderId = senderId;
    if (realSenderId == 0 && !m_relayMetaMap.isEmpty()) {
        realSenderId = m_relayMetaMap.keys().first();
    } else if (realSenderId == 0) {
        realSenderId = 1;
    }

    QString saveDir = QDir::homePath() + "/chat_files";
    QString fileName = QString::fromStdString(fragMsg.fileName).split("/").last();
    QDir dir(saveDir);
    if (!dir.exists()) dir.mkpath(".");
    QString savePath = QString("%1/relay_%2_%3").arg(saveDir).arg(realSenderId).arg(fileName);

    // ====================== 修复：Base64 解码 ======================
    QByteArray rawData = QByteArray::fromStdString(fragMsg.fileData);
    QByteArray fragmentData = QByteArray::fromBase64(rawData);

    m_relayFragMap[realSenderId][fragMsg.fragmentIdx] = fragmentData;

    int progress = (m_relayFragMap[realSenderId].size() * 100) / fragMsg.totalFragments;

    // ========== 修复编译错误：先构造完整QString，再传入Q_ARG ==========
    QString progressMsg = QString("[中继合并] 已缓存分片%1/%2（senderId：%3），进度：%4%")
        .arg(fragMsg.fragmentIdx)
        .arg(fragMsg.totalFragments)
        .arg(realSenderId)
        .arg(progress);
    
    QMetaObject::invokeMethod(this, "showMessage",
        Qt::QueuedConnection,
        Q_ARG(QString, "系统提示"),
        Q_ARG(QString, progressMsg));

    QMetaObject::invokeMethod(this, "updateTransferProgress",
        Qt::QueuedConnection, Q_ARG(int, progress));

    if (m_relayFragMap[realSenderId].size() == fragMsg.totalFragments) {
        QFile file(savePath);
        if (file.open(QIODevice::WriteOnly)) {
            qint64 total = 0;
            for (uint32_t i = 1; i <= fragMsg.totalFragments; ++i) {
                if (m_relayFragMap[realSenderId].contains(i)) {
                    total += file.write(m_relayFragMap[realSenderId][i]);
                }
            }
            file.close();

            // ========== 同样修复这里的arg()调用 ==========
            QString saveMsg = QString("[中继合并] 文件保存成功！路径：%1，实际大小：%2 字节")
                .arg(savePath)
                .arg(total);
            
            QMetaObject::invokeMethod(this, "showMessage",
                Qt::QueuedConnection,
                Q_ARG(QString, "系统提示"),
                Q_ARG(QString, saveMsg));
        }

        m_relayFragMap.remove(realSenderId);
        m_relayMetaMap.remove(realSenderId);
        QMetaObject::invokeMethod(this, "updateTransferProgress",
            Qt::QueuedConnection, Q_ARG(int, 0));
    }
}

// 接收文件
void ChatWindow::receiveFile(const FileMsg& metaMsg) {
    // ========== 强制缓存元信息，不管ID ==========
    if (metaMsg.senderId > 0) {
        m_relayMetaMap[metaMsg.senderId] = metaMsg;
        QMetaObject::invokeMethod(this, "showMessage",
            Qt::QueuedConnection,
            Q_ARG(QString, "系统提示"),
            Q_ARG(QString, QString("强制缓存文件元信息（senderId：%1，receiverId：%2，分片数：%3）").arg(metaMsg.senderId).arg(metaMsg.receiverId).arg(metaMsg.totalFragments)));
    }

    // ========== 核心修改：关闭直连接收逻辑，仅缓存元信息等待中继分片 ==========
    QMetaObject::invokeMethod(this, "showMessage",
        Qt::QueuedConnection,
        Q_ARG(QString, "系统提示"),
        Q_ARG(QString, "已禁用直连接收，等待中继分片..."));

    // 启动一个定时器，5秒后检查分片是否到齐（避免永久等待）
    QTimer::singleShot(5000, this, [=]() {
        if (m_relayFragMap.contains(metaMsg.senderId) && m_relayFragMap[metaMsg.senderId].size() == metaMsg.totalFragments) {
            return; // 已处理，无需操作
        } else if (m_relayFragMap.contains(metaMsg.senderId)) {
            QMetaObject::invokeMethod(this, "showMessage",
                Qt::QueuedConnection,
                Q_ARG(QString, "系统提示"),
                Q_ARG(QString, QString("5秒检查：已收到%1/%2个分片，继续等待...").arg(m_relayFragMap[metaMsg.senderId].size()).arg(metaMsg.totalFragments)));
        } else {
            QMetaObject::invokeMethod(this, "showMessage",
                Qt::QueuedConnection,
                Q_ARG(QString, "系统提示"),
                Q_ARG(QString, "5秒检查：未收到任何分片，可能是直连数据，请手动处理"));
        }
    });
}

// ========== 新增：供中继数据接收函数调用的分片处理接口 ==========
void ChatWindow::receiveFileFragment(const FileMsg& fragMsg) {
    // 从中继元信息缓存获取文件元信息
    if (!m_relayMetaMap.contains(fragMsg.senderId)) {
        QMetaObject::invokeMethod(this, "showMessage",
            Qt::QueuedConnection,
            Q_ARG(QString, "系统错误"),
            Q_ARG(QString, QString("[中继] 未找到用户%1的文件元信息，无法处理分片").arg(fragMsg.senderId)));
        return;
    }

    FileMsg metaMsg = m_relayMetaMap[fragMsg.senderId];
    // 调用中继分片处理逻辑
    processRelayFragment(fragMsg.senderId, fragMsg);
}

// 处理点对点新连接的槽函数
void ChatWindow::onP2PNewConnection() {
    // 关键：用新增的m_p2pClientSocket存储客户端连接，绝不覆盖发送用的m_tcpP2PSocket
    m_p2pClientSocket = m_tcpP2PServer->nextPendingConnection();
    
    // 空指针校验（必须！）
    if (!m_p2pClientSocket) {
        showMessage("系统错误", "获取P2P新连接失败：无可用的客户端Socket");
        return;
    }
    
    // 指定父对象为ChatWindow，由QT自动管理内存（避免内存泄漏）
    m_p2pClientSocket->setParent(this);

    // 核心修复：先检查是否有缓存的待接收图片信息
    if (m_pendingImageMsg.fileSize <= 0 || m_pendingImageMsg.senderId <= 0) {
        showMessage("系统提示", "无待接收的图片信息，关闭P2P连接");
        m_p2pClientSocket->disconnectFromHost();
        m_p2pClientSocket->deleteLater();
        m_p2pClientSocket = nullptr;
        return;
    }

    // 绑定新连接的信号槽（仅针对这个接收用的客户端Socket）
    connect(m_p2pClientSocket, &QTcpSocket::readyRead, this, &ChatWindow::onP2PSocketReadyRead);
    connect(m_p2pClientSocket, &QTcpSocket::disconnected, this, &ChatWindow::onP2PClientDisconnected);
    connect(m_p2pClientSocket, &QTcpSocket::errorOccurred, this, &ChatWindow::onP2PSocketError);

    // 打印精准日志，确认连接信息（便于排查端口/IP问题）
    showMessage("系统提示", QString("P2P 新连接建立：%1:%2（本地监听端口：%3）")
                .arg(m_p2pClientSocket->peerAddress().toString())
                .arg(m_p2pClientSocket->peerPort())
                .arg(m_tcpP2PServer->serverPort()));
}

// 新增：客户端连接断开的专属槽函数（避免影响发送用Socket）
void ChatWindow::onP2PClientDisconnected() {
    QTcpSocket* socket = qobject_cast<QTcpSocket*>(sender());
    if (socket) {
        showMessage("系统提示", "P2P 客户端连接已断开：" + socket->peerAddress().toString());
        socket->deleteLater(); // 安全释放Socket资源
        m_p2pClientSocket = nullptr; // 重置接收用Socket指针（仅重置这个！）
    }
}

// 槽函数：选择在线用户作为接收方
void ChatWindow::onUserItemClicked(QListWidgetItem* item) {
    if (!item) return;
    
    QString itemText = item->text();
    // 提取ID：基于格式“（ID：xxx，IP：xxx）”，截取“ID：”和“，”之间的内容
    int idStart = itemText.indexOf("ID：") + 3; // “ID：”长度为3，定位到ID开头
    int idEnd = itemText.indexOf("，", idStart); // 定位到ID后的逗号
    
    if (idStart > 0 && idEnd > idStart) {
        QString userIdStr = itemText.mid(idStart, idEnd - idStart);
        bool ok;
        int userId = userIdStr.toInt(&ok);
        
        if (ok && userId > 0 && userId != m_userId) { // 排除自己
            m_selectedUserId = userId;
            showMessage("系统提示", QString("已选择接收方：%1（ID：%2）").arg(itemText.left(idStart-3)).arg(m_selectedUserId));
            
            // 额外：将选中的ID填充到私聊ID输入框，方便用户确认
            ui->lineEdit_privateId->setText(userIdStr);
        } else {
            showMessage("系统提示", "提取用户ID失败，或无法选择自己作为接收方");
            m_selectedUserId = 0;
        }
    } else {
        showMessage("系统提示", "无法从列表项中提取用户ID");
        m_selectedUserId = 0;
    }
}

// 处理点对点数据接收
void ChatWindow::onP2PDataReady() {
    QTcpSocket* socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket) return;
    QByteArray data = socket->readAll();
    // 处理接收的图片/文件数据
}

// 新增UDP接收槽函数
void ChatWindow::onUdpReadyRead() {
    if (!m_udpP2PSocket) return; // 判空

    while (m_udpP2PSocket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(m_udpP2PSocket->pendingDatagramSize());
        QHostAddress senderAddr;
        quint16 senderPort;
        qint64 readLen = m_udpP2PSocket->readDatagram(datagram.data(), datagram.size(), &senderAddr, &senderPort);
        if (readLen <= 0) continue; // 读取失败直接跳过
        
        // ========== 核心修复：优先处理中继UDP数据 ==========
        QStringList relayParts = QString(datagram).split("|");
        if (relayParts.size() >= 3) {
            // 是中继UDP数据（文件/图片）
            int senderId = relayParts[0].toInt();
            int receiverId = relayParts[1].toInt();
            
            // 兼容receiverId=0/正确ID，senderId=0时用元信息里的senderId
            if (receiverId != m_userId && receiverId != 0) continue;
            if (senderId == 0 && !m_relayMetaMap.isEmpty()) {
                senderId = m_relayMetaMap.keys().first(); // 兜底取第一个缓存的senderId
            }

            QByteArray realData = relayParts[2].toUtf8();
            showMessage("系统提示", QString("[UDP中继] 解析到中继数据（senderId：%1，receiverId：%2），长度：%3字节")
                        .arg(senderId).arg(receiverId).arg(realData.size()));

            try {
                // 优先处理文件分片
                FileMsg fragMsg = deserializeFileMsg(realData.toStdString());
                if (fragMsg.senderId == 0) fragMsg.senderId = senderId; // 修复senderId=0
                processRelayFragment(senderId, fragMsg); // 直接调用合并逻辑
                continue;
            } catch (std::exception& e) {
                // 处理图片数据
                if (!m_pendingImageMsg.fileName.empty()) {
                    m_pendingImageData = QByteArray::fromBase64(realData);
                    showMessage("系统提示", QString("[UDP中继] 接收图片数据：%1/%2字节")
                                .arg(m_pendingImageData.size()).arg(m_pendingImageMsg.fileSize));
                    
                    // 数据完整则保存
                    if (m_pendingImageData.size() >= m_pendingImageMsg.fileSize) {
                        QString saveDir = "./recv_imgs";
                        QDir dir(saveDir);
                        if (!dir.exists()) dir.mkpath(saveDir);
                        
                        QString fileName = QString::fromStdString(m_pendingImageMsg.fileName).split("/").last();
                        QString savePath = QString("%1/udp_relay_%2_%3").arg(saveDir).arg(senderId).arg(fileName);
                        
                        QFile file(savePath);
                        if (file.open(QIODevice::WriteOnly)) {
                            file.write(m_pendingImageData);
                            file.close();
                            showMessage("系统提示", QString("[UDP中继] 图片保存完成：%1").arg(savePath));
                        } else {
                            showMessage("系统错误", QString("[UDP中继] 保存图片失败：%1").arg(file.errorString()));
                        }
                        // 重置缓存
                        m_pendingImageMsg = ImageMsg();
                        m_pendingImageData.clear();
                    }
                }
                continue;
            }
        }

        // ========== 直连UDP数据（兼容旧逻辑） ==========
        try {
            // 尝试反序列化为文件分片
            FileMsg fragMsg = deserializeFileMsg(datagram.toStdString());
            showMessage("系统提示", QString("收到UDP文件分片：来自%1:%2，分片ID：%3")
                        .arg(senderAddr.toString())
                        .arg(senderPort)
                        .arg(fragMsg.fragmentIdx));
            // 直连分片也调用合并逻辑
            processRelayFragment(fragMsg.senderId, fragMsg);
            continue; // 跳过图片保存逻辑
        } catch (std::exception& e) {
            // 不是文件分片，判定为图片数据
            showMessage("系统提示", QString("收到UDP图片数据：来自%1:%2，长度%3字节")
                        .arg(senderAddr.toString())
                        .arg(senderPort)
                        .arg(datagram.size()));
            
            // 保存图片数据（仅处理真正的图片）
            QString saveDir = "./recv_imgs";
            QDir dir(saveDir);
            if (!dir.exists()) dir.mkpath(saveDir);

            // 生成唯一文件名（基于时间戳+随机数，避免重复）
            QString timestamp = QTime::currentTime().toString("hhmmsszzz");
            QString savePath = QString("%1/udp_img_%2.jpg").arg(saveDir).arg(timestamp);
            QFile file(savePath); 
            if (file.open(QIODevice::WriteOnly)) {
                file.write(datagram);
                file.close();
                showMessage("系统提示", QString("UDP图片已保存至：%1").arg(savePath));
            } else {
                showMessage("系统错误", QString("保存UDP图片失败：%1").arg(file.errorString()));
            }
        }
    }
}

// 显示聊天消息
void ChatWindow::showMessage(const QString& sender, const QString& content, bool isPrivate) {
    QString prefix = isPrivate ? "[私聊] " : "";
    // 区分消息类型（图片/文件/文本）
    QString typePrefix = "";
    if (content.contains("图片消息") || content.contains(".jpg") || content.contains(".png")) {
        typePrefix = "[图片] ";
    } else if (content.contains("文件消息") || content.contains(".zip") || content.contains(".txt")) {
        typePrefix = "[文件] ";
    } else {
        typePrefix = "[文本] ";
    }
    
    QString msg = QString("[%1] %2%3%4：%5")
        .arg(QTime::currentTime().toString())
        .arg(prefix)
        .arg(typePrefix)
        .arg(sender)
        .arg(content);
    
    // 不同类型消息用不同颜色
    if (typePrefix == "[图片] ") {
        ui->textEdit_chatLog->setTextColor(Qt::blue);
    } else if (typePrefix == "[文件] ") {
        ui->textEdit_chatLog->setTextColor(Qt::green);
    } else if (prefix == "[私聊] ") {
        ui->textEdit_chatLog->setTextColor(Qt::red);
    } else {
        ui->textEdit_chatLog->setTextColor(Qt::black);
    }
    
    ui->textEdit_chatLog->append(msg);
    // 恢复默认颜色
    ui->textEdit_chatLog->setTextColor(Qt::black);
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

//错误处理槽函数
void ChatWindow::onP2PSocketError(QAbstractSocket::SocketError socketError) {
    QTcpSocket* socket = qobject_cast<QTcpSocket*>(sender());
    if (socket) {
        showMessage("P2P 错误", QString("P2P套接字错误：%1").arg(socket->errorString()));
    }
}

//P2P数据读取槽函数
void ChatWindow::onP2PSocketReadyRead() {
    QTcpSocket* socket = m_p2pClientSocket;
    if (!socket) {
        showMessage("系统提示", "P2P Socket为空，忽略数据");
        return;
    }

    // ========== 读取所有数据，避免半包 ==========
    QByteArray rawData = socket->readAll();
    QByteArray realImageData;
    bool isRelayData = false;
    int relaySenderId = 0;

    // ========== 解析中继数据 ==========
    QStringList relayParts = QString(rawData).split("|");
    if (relayParts.size() >= 3) {
        int senderId = relayParts[0].toInt();
        int receiverId = relayParts[1].toInt();
        
        // 兼容所有合法ID
        if (receiverId != m_userId && receiverId != 0) {
            showMessage("系统提示", QString("收到非本机的中继图片数据（接收方ID：%1），忽略").arg(receiverId));
            return; // 移除断开连接逻辑，仅忽略数据
        }

        // 修复senderId=0
        relaySenderId = senderId == 0 ? (m_relayMetaMap.isEmpty() ? 1 : m_relayMetaMap.keys().first()) : senderId;
        // 解码Base64（核心：用toUtf8，避免编码错误）
        realImageData = QByteArray::fromBase64(relayParts[2].toUtf8());
        isRelayData = true;

        showMessage("系统提示", QString("解析中继图片数据成功（来自用户%1）：%2字节（原始数据：%3字节）")
                    .arg(relaySenderId).arg(realImageData.size()).arg(rawData.size()));
    } else {
        realImageData = rawData;
    }

    // ========== 校验元信息 ==========
    if (m_pendingImageMsg.fileSize <= 0) {
        showMessage("系统提示", "无有效的图片元信息，保存为临时文件");
        // 保存临时图片（避免数据丢失）
        QString saveDir = "./recv_imgs";
        QDir dir(saveDir);
        if (!dir.exists()) dir.mkpath(saveDir);
        QString timestamp = QTime::currentTime().toString("hhmmsszzz");
        QString savePath = QString("%1/tmp_img_%2.jpg").arg(saveDir).arg(timestamp);
        
        QFile file(savePath);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(realImageData);
            file.close();
            showMessage("系统提示", QString("临时图片保存完成：%1（大小：%2字节）").arg(savePath).arg(realImageData.size()));
        }
        return; // 移除断开连接逻辑
    }

    // ========== 累加数据并保存 ==========
    m_pendingImageData += realImageData;
    showMessage("系统提示", QString("已接收%1图片数据：%2/%3字节（来自用户%4）")
                .arg(isRelayData ? "中继" : "直连")
                .arg(m_pendingImageData.size()).arg(m_pendingImageMsg.fileSize).arg(relaySenderId));

    // 数据完整则保存
    if (m_pendingImageData.size() >= m_pendingImageMsg.fileSize) {
        QString saveDir = "./recv_imgs";
        QDir dir(saveDir);
        if (!dir.exists()) dir.mkpath(saveDir);
        
        QString fileName = QString::fromStdString(m_pendingImageMsg.fileName).split("/").last();
        if (isRelayData) {
            fileName = QString("relay_%1_%2").arg(relaySenderId).arg(fileName);
        }
        QString savePath = QString("%1/%2").arg(saveDir).arg(fileName);

        QFile file(savePath);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(m_pendingImageData);
            file.close();
            showMessage("系统提示", QString("%1图片接收完成，已保存至：%2（实际大小：%3字节）")
                        .arg(isRelayData ? "中继" : "直连")
                        .arg(savePath)
                        .arg(m_pendingImageData.size()));
        } else {
            showMessage("系统错误", QString("保存图片失败：%1").arg(file.errorString()));
        }

        // 重置缓存
        m_pendingImageMsg = ImageMsg();
        m_pendingImageData.clear();
        // 数据接收完成后再断开连接
        socket->disconnectFromHost();
    }
}

//P2P连接断开槽函数
void ChatWindow::onP2PSocketDisconnected() {
    QTcpSocket* socket = qobject_cast<QTcpSocket*>(sender());
    if (socket) {
        showMessage("系统提示", "P2P 连接已断开");
        socket->deleteLater(); // 安全删除套接字
        
        // 仅重置对应的Socket指针：如果是接收用的，重置m_p2pClientSocket；如果是发送用的，重置m_tcpP2PSocket
        if (socket == m_p2pClientSocket) {
            m_p2pClientSocket = nullptr;
        } else if (socket == m_tcpP2PSocket) {
            m_tcpP2PSocket = nullptr;
        }
    }
}

// 判断是否为内网IP（用于决定是否走中继）
bool ChatWindow::isPrivateIp(const QHostAddress& addr) {
    if (addr.isNull()) return true;
    QString ip = addr.toString();
    // 匹配内网IP段：192.168.x.x、10.x.x.x、172.16-31.x.x
    return ip.startsWith("192.168.") || ip.startsWith("10.") || 
           (ip.startsWith("172.") && (ip.mid(4,2).toInt() >=16 && ip.mid(4,2).toInt() <=31));
}

// 处理中继TCP数据接收
void ChatWindow::onRelayTcpReadyRead() {
    QByteArray data = m_tcpRelaySocket->readAll();
    QStringList parts = QString(data).split("|");
    if (parts.size() < 3) {
        showMessage("系统提示", "[中继] 收到无效的TCP数据");
        return;
    }

    int senderId = parts[0].toInt();
    int receiverId = parts[1].toInt();
    if (receiverId != m_userId) {
        showMessage("系统提示", QString("[中继] 收到非本机的TCP数据（接收方ID：%1），忽略").arg(receiverId));
        return;
    }

    QByteArray realData = parts[2].toUtf8();
    showMessage("系统提示", QString("[中继] 收到TCP数据（来自用户%1），长度：%2字节").arg(senderId).arg(realData.size()));

    try {
        // 尝试解析为文件分片
        FileMsg fragMsg = deserializeFileMsg(realData.toStdString());
        receiveFileFragment(fragMsg); // 调用新增接口处理
    } catch (...) {
        // 图片数据：复用原有中继图片接收逻辑
        m_pendingImageData = QByteArray::fromBase64(realData);
        if (m_pendingImageMsg.fileSize > 0 && m_pendingImageData.size() == m_pendingImageMsg.fileSize) {
            QString saveDir = "./recv_imgs";
            QDir dir(saveDir);
            dir.mkpath(saveDir);
            QString fileName = QString::fromStdString(m_pendingImageMsg.fileName).split("/").last();
            QString savePath = QString("%1/relay_%2_%3").arg(saveDir).arg(senderId).arg(fileName);
            
            QFile file(savePath);
            if (file.open(QIODevice::WriteOnly)) {
                file.write(m_pendingImageData);
                file.close();
                showMessage("系统提示", QString("[中继] TCP图片保存完成：%1").arg(savePath));
                m_pendingImageMsg = ImageMsg();
                m_pendingImageData.clear();
            }
        }
    }
}

// 处理中继UDP数据接收
void ChatWindow::onRelayUdpReadyRead() {
    while (m_udpRelaySocket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(m_udpRelaySocket->pendingDatagramSize());
        QHostAddress senderAddr;
        quint16 senderPort;
        qint64 readLen = m_udpRelaySocket->readDatagram(datagram.data(), datagram.size(), &senderAddr, &senderPort);
        if (readLen <= 0) continue;

        QStringList parts = QString(datagram).split("|");
        if (parts.size() < 3) {
            showMessage("系统提示", "[中继] 收到无效的UDP数据");
            continue;
        }

        int senderId = parts[0].toInt();
        int receiverId = parts[1].toInt();
        if (receiverId != m_userId) continue;

        QByteArray realData = parts[2].toUtf8();
        showMessage("系统提示", QString("[中继] 收到UDP数据（来自%1:%2，用户%3），长度：%4字节")
                    .arg(senderAddr.toString()).arg(senderPort).arg(senderId).arg(realData.size()));

        try {
            // 文件分片
            FileMsg fragMsg = deserializeFileMsg(realData.toStdString());
            receiveFileFragment(fragMsg); // 调用新增接口处理
        } catch (...) {
            // 图片数据
            QString saveDir = "./recv_imgs";
            QDir dir(saveDir);
            dir.mkpath(saveDir);
            QString timestamp = QTime::currentTime().toString("hhmmsszzz");
            QString savePath = QString("%1/relay_udp_img_%2_%3.jpg").arg(saveDir).arg(senderId).arg(timestamp);
            QFile file(savePath);
            if (file.open(QIODevice::WriteOnly)) {
                file.write(realData);
                file.close();
                showMessage("系统提示", QString("[中继] UDP图片已保存至：%1").arg(savePath));
            }
        }
    }
}