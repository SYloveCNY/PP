#define AVATAR_DIR "./avatars/" 

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
#include <QFileIconProvider>   // 对应QFileIconProvider
#include <QDesktopServices>    // 对应QDesktopServices
#include <QUrl>                // 对应QUrl
#include <QFile>               // 对应QFile
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
    , m_p2pClientSocket(nullptr)
    , m_fileTransferMutex()
    , m_imgRecvTimeoutTimer(nullptr)
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

    // 连接中继TCP服务器（取消注释，实际连接）
    connect(m_tcpRelaySocket, &QTcpSocket::connected, this, [=]() {
        showMessage("系统提示", "已连接到公网中继服务器（TCP）");
    });
    // connect(m_tcpRelaySocket, &QTcpSocket::disconnected, this, [=]() {
    //     showMessage("系统提示", "与公网中继服务器（TCP）断开连接，正在重连...");
    //     QTimer::singleShot(3000, this, [=]() {
    //         m_tcpRelaySocket->connectToHost(m_relayServerIp, m_relayTcpPort);
    //     });
    // });
    // connect(m_tcpRelaySocket, &QTcpSocket::errorOccurred, this, [=](QAbstractSocket::SocketError error) {
    //     showMessage("系统错误", QString("中继TCP连接失败：%1，3秒后重连").arg(m_tcpRelaySocket->errorString()));
    //     QTimer::singleShot(3000, this, [=]() {
    //         m_tcpRelaySocket->connectToHost(m_relayServerIp, m_relayTcpPort);
    //     });
    // });
    connect(m_tcpRelaySocket, &QTcpSocket::readyRead, this, &ChatWindow::onRelayTcpReadyRead);
    connect(m_udpRelaySocket, &QUdpSocket::readyRead, this, &ChatWindow::onRelayUdpReadyRead);
    // 内网测试：注释中继服务器连接（避免反复报错）
    // m_tcpRelaySocket->connectToHost(m_relayServerIp, m_relayTcpPort);

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
    //遍历指定端口区间，找第一个可用的UDP端口
    for (int port = 9101; port <= 9200; ++port) {
        // 注意：UDP的bind和TCP的listen参数一致，但逻辑不同（UDP无监听，仅绑定端口）
        if (m_udpP2PSocket->bind(QHostAddress::Any, port)) {
            selectedUdpPort = port;
            break;
        }
    }

    // 处理UDP绑定结果（容错+日志）
    if (selectedUdpPort == 0) {
        showMessage("系统错误", QString("P2P UDP绑定失败：9101-9200端口全被占用！"));
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
    connect(ui->pushButton_sendImage, &QPushButton::clicked, this, [this](bool) {this->sendImage();});
    connect(ui->pushButton_sendFile, &QPushButton::clicked, this, &ChatWindow::sendFile);
    connect(ui->listWidget_onlineUsers, &QListWidget::itemClicked, this, &ChatWindow::onUserItemClicked);

    // 启动心跳定时器
    m_heartbeatTimer->start(10000);

    // 统一接收目录（分离文件/图片）
    m_fileRecvDir = QCoreApplication::applicationDirPath() + "/chat_receive/files/";
    m_imgRecvDir = QCoreApplication::applicationDirPath() + "/chat_receive/imgs/";
    // 强制创建目录
    QDir().mkpath(m_fileRecvDir);
    QDir().mkpath(m_imgRecvDir);
    showMessage("系统提示", QString("文件接收目录：%1\n图片接收目录：%2").arg(m_fileRecvDir).arg(m_imgRecvDir));

    // 初始化图片接收超时定时器（仅创建一次）
    m_imgRecvTimeoutTimer = new QTimer(this);
    m_imgRecvTimeoutTimer->setSingleShot(true);
    connect(m_imgRecvTimeoutTimer, &QTimer::timeout, this, [=]() {
        if (!m_pendingImageData.isEmpty()) {
            showMessage("系统提示", "图片接收超时，保存已接收的部分数据");
            QString saveDir = m_imgRecvDir;
            QDir dir(saveDir);
            if (!dir.exists()) dir.mkpath(saveDir);
            QString fileName = QString::fromStdString(m_pendingImageMsg.fileName).split("/").last();
            QString savePath = QString("%1/%2_partial.jpg").arg(saveDir).arg(fileName);
            QFile file(savePath);
            if (file.open(QIODevice::WriteOnly)) {
                file.write(m_pendingImageData);
                file.close();
                showMessage("系统提示", QString("图片部分保存完成：%1（大小：%2字节）").arg(savePath).arg(m_pendingImageData.size()));
            }
        }
    });
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
    if (m_udpRelaySocket) {
        m_udpRelaySocket->close();
        m_udpRelaySocket->deleteLater();
    }
    if (m_tcpRelaySocket) {
        m_tcpRelaySocket->disconnectFromHost();
        m_tcpRelaySocket->deleteLater();
    }
    if (m_transferFile) {
        if (m_transferFile->isOpen()) m_transferFile->close();
        delete m_transferFile;
    }
    delete ui;
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
    if (m_socket) { // 先检查非空
        if (m_socket->state() == QTcpSocket::ConnectedState) {
            m_heartbeatTimer->stop();
            m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);

            // 构造下线通知
            UserStatusNotify notify;
            notify.userId = m_userId;
            notify.nickname = m_nickname.toStdString();
            notify.isOnline = false;
            std::string notifyData = serialize(notify);
            
            sendPacket(m_socket, static_cast<uint32_t>(MsgType::USER_STATUS_NOTIFY), notifyData, nextMsgId++, m_userId);
            m_socket->waitForBytesWritten(1000);
            m_socket->disconnectFromHost();
            m_socket->waitForDisconnected(1000);
        }
        m_socket = nullptr;
    }

    g_currentUserId = 0;
    g_clientSocket = nullptr;
    event->accept();
}

// 设置登录信息
void ChatWindow::setLoginInfo(int userId, const QString& nickname, QTcpSocket* socket) {
    m_userId = userId;
    m_nickname = nickname;
    m_socket = socket;
    g_clientSocket = socket;
    g_currentUserId = userId;

    // 1. 先获取头像路径
    QString avatarPath = getUserAvatarPath(userId);
    QPixmap avatarPixmap(avatarPath);
    
    // 2. 调用setSelfAvatar显示头像+文字（不再单独设置pixmap）
    setSelfAvatar(avatarPixmap);

    // 3. 显示P2P端口信息
    m_userInfoLabel->setText(QString("当前用户：%1（ID：%2，P2P监听端口：%3）")
                             .arg(nickname).arg(userId).arg(m_p2pTcpPort));
    showMessage("系统提示", QString("登录成功！本地P2P监听端口：%1").arg(m_p2pTcpPort));

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
    m_socket->write((char*)&netHeader, sizeof(PacketHeader));
    m_socket->write(heartbeatData.c_str(), heartbeatData.size());

    showMessage("系统提示", "心跳包响应正常");
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
    if (!ui->listWidget_onlineUsers) {
        showMessage("系统提示", "在线用户列表控件未初始化！");
        return;
    }
    ui->listWidget_onlineUsers->clear();
    
    for (const auto& user : users) {
        // 1. 获取用户头像
        QString avatarPath = getUserAvatarPath(user.userId);
        QPixmap avatarPixmap(avatarPath);
        if (avatarPixmap.isNull()) {
            avatarPixmap.load("qrc:/images/default_avatar.png");
        }
        
        // 2. 调用addUserItemWithAvatar添加带头像的项
        addUserItemWithAvatar(
            user.userId, 
            QString::fromStdString(user.nickname), 
            avatarPixmap
        );
        
        // 3. 缓存用户端口
        if (user.userId > 0) {
            m_userPortMap[user.userId] = QString::number(user.dataPort);
        }
    }
    
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
        m_selectedUserId = receiverId;
    }
    
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

    // 本地测试时跳过内网IP判断（核心修复）
    bool useRelay = false; // 强制直连，注释此行可恢复中继逻辑
    // bool useRelay = isPrivateIp(m_targetIp);
    if (useRelay) {
        showMessage("系统提示", "检测到目标为内网IP，将通过公网中继发送图片");
    }

    // 3. 建立连接并发送图片（区分中继/直连）
    // ========== 修复点1：改用指针变量，支持lambda中修改 ==========
    bool* pSendOk = new bool(false);

    if (useRelay) {
        // 内网：走中继TCP连接
        if (m_tcpRelaySocket->state() != QTcpSocket::ConnectedState) {
            m_tcpRelaySocket->abort();
            m_tcpRelaySocket->connectToHost(m_relayServerIp, m_relayTcpPort);
            if (!m_tcpRelaySocket->waitForConnected(2000)) {
                showMessage("系统错误", "连接中继服务器失败：" + m_tcpRelaySocket->errorString());
                delete pSendOk; // 释放内存
                return;
            }
        }

        // 拼接中继数据：senderId|receiverId|Base64图片数据
        QString relayData = QString("%1|%2|%3")
            .arg(g_currentUserId)
            .arg(receiverId)
            .arg(QString(imgData.toBase64()));
        
        // 分块发送（避免单次发送过大）
        const int BLOCK_SIZE = 4096;
        qint64 totalSent = 0;
        for (int i = 0; i < relayData.size(); i += BLOCK_SIZE) {
            QString block = relayData.mid(i, BLOCK_SIZE);
            qint64 sent = m_tcpRelaySocket->write(block.toUtf8());
            if (sent > 0) {
                totalSent += sent;
                m_tcpRelaySocket->flush();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } else {
                showMessage("系统错误", "中继发送图片失败：" + m_tcpRelaySocket->errorString());
                break;
            }
        }
        
        if (totalSent > 0) {
            showMessage("系统提示", QString("图片数据已发送至中继服务器：%1字节").arg(totalSent));
            *pSendOk = true; // 修改指针指向的变量
        }
    } else {
        // 公网：原有P2P直连逻辑
        QTcpSocket* p2pSocket = new QTcpSocket(this); // 新建Socket，避免覆盖全局Socket
        
        // ========== 修复点2：lambda捕获指针，支持修改 ==========
        connect(p2pSocket, &QTcpSocket::connected, this, [=]() {
            // 分块发送图片数据（核心：循环发送所有数据，直到发完）
            const int BLOCK_SIZE = 4096;
            qint64 totalSent = 0;
            qint64 remaining = imgData.size();
            while (remaining > 0) {
                int sendSize = qMin(BLOCK_SIZE, (int)remaining);
                QByteArray block = imgData.mid(totalSent, sendSize);
                qint64 sent = p2pSocket->write(block);
                
                if (sent <= 0) {
                    showMessage("系统错误", "P2P发送图片失败：" + p2pSocket->errorString());
                    break;
                }
                
                // 关键：强制刷出数据，确保接收端能收到
                p2pSocket->flush();
                // 等待数据写入完成（避免发送过快导致粘包）
                p2pSocket->waitForBytesWritten(50);
                
                totalSent += sent;
                remaining -= sent;
                
                // 实时更新发送进度（可选）
                QMetaObject::invokeMethod(this, "showMessage",
                    Qt::QueuedConnection,
                    Q_ARG(QString, "系统提示"),
                    Q_ARG(QString, QString("图片发送中：%1/%2字节").arg(totalSent).arg(imgData.size())));
            }

            if (totalSent == imgData.size()) {
                showMessage("系统提示", QString("图片数据发送成功：%1字节").arg(totalSent));
                *pSendOk = true;
                
                // 关键修改：发送完成后保持连接10秒，让接收端读完所有数据
                QTimer::singleShot(10000, this, [=]() {
                    if (p2pSocket->state() == QTcpSocket::ConnectedState) {
                        p2pSocket->disconnectFromHost();
                    }
                });

                // 新增：监听接收端断开事件，收到后立即释放（避免空等10秒）
                connect(p2pSocket, &QTcpSocket::disconnected, this, [=]() {
                    showMessage("系统提示", "接收端已读取完图片数据，连接正常断开");
                    p2pSocket->deleteLater();
                });
            } else {
                showMessage("系统错误", QString("图片发送不完整：仅发送%1/%2字节").arg(totalSent).arg(imgData.size()));
                p2pSocket->disconnectFromHost();
            }
        });

        // 连接失败处理
        connect(p2pSocket, &QTcpSocket::errorOccurred, this, [=](QAbstractSocket::SocketError err) {
            showMessage("P2P 错误", QString("P2P套接字错误：%1").arg(p2pSocket->errorString()));
            p2pSocket->deleteLater();
        });

        // 发起连接（重试3次）
        bool connectOk = false;
        for (int retry = 0; retry < 3; retry++) {
            p2pSocket->connectToHost(m_targetIp, m_targetTcpPort);
            if (p2pSocket->waitForConnected(2000)) {
                connectOk = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        if (!connectOk) {
            showMessage("系统错误", "P2P连接失败（已重试3次），无法发送图片");
            p2pSocket->deleteLater();
            delete pSendOk; // 释放内存
            return;
        }
    }

    // 最终结果提示
    if (*pSendOk) {
        showMessage("系统提示", QString("图片发送成功（接收方ID：%1）").arg(receiverId));
    } else {
        showMessage("系统错误", "图片发送最终失败，请检查中继服务器或网络");
    }
    
    // ========== 修复点3：释放指针内存，避免泄漏 ==========
    delete pSendOk;
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

    // ========== 新增：等待打洞完成（如果正在打洞） ==========
    int punchWaitCount = 0;
    // 新增：内网IP直接跳过打洞等待
    if (isPrivateIp(m_targetIp)) {
        m_holePunchState = HolePunchState::Success;
        punchWaitCount = 15; // 直接退出等待循环
    }
    while (m_holePunchState == HolePunchState::Punching) {
        if (punchWaitCount >= 15) { // 最多等3秒打洞完成
            QMetaObject::invokeMethod(this, "showMessage",
                Qt::QueuedConnection,
                Q_ARG(QString, "系统提示"),
                Q_ARG(QString, "打洞超时，使用默认传输策略"));
            break;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        punchWaitCount++;
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
    // ========== 修复：把useRelay提到for循环外，扩大作用域 ==========
    bool useRelay = false; 

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
        fragMsg.base64Data = fragmentData.toBase64().toStdString(); 
        // ========== 修复核心问题：不再清空fileData ==========
        fragMsg.fileData = fragMsg.base64Data; // 保持数据一致，接收端能读到
        fragMsg.dataLen = fragmentData.size();
        std::string fragData = serialize(fragMsg);

        // ========== 核心修改：根据打洞状态动态决定是否使用中继 ==========
        if (m_holePunchState == HolePunchState::Success) {
            useRelay = false; // 打洞成功，优先直连
            QMetaObject::invokeMethod(this, "showMessage",
                Qt::QueuedConnection,
                Q_ARG(QString, "系统提示"),
                Q_ARG(QString, QString("打洞成功，分片%1使用P2P直连传输").arg(idx)));
        } else if (m_holePunchState == HolePunchState::Failed) {
            useRelay = true; // 打洞失败，强制使用中继
            QMetaObject::invokeMethod(this, "showMessage",
                Qt::QueuedConnection,
                Q_ARG(QString, "系统提示"),
                Q_ARG(QString, QString("打洞失败，分片%1使用中继传输").arg(idx)));
        } else {
            // 未打洞/打洞中，根据IP类型判断（内网用中继，公网试直连）
            useRelay = isPrivateIp(m_targetIp);
            QMetaObject::invokeMethod(this, "showMessage",
                Qt::QueuedConnection,
                Q_ARG(QString, "系统提示"),
                Q_ARG(QString, QString("未检测到打洞状态，分片%1%2传输").arg(idx).arg(useRelay ? "使用中继" : "尝试直连")));
        }

        if (useRelay) {
            if (metaMsg.protocol == TransportProtocol::TCP) {
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
            }
        } else {
            if (metaMsg.protocol == TransportProtocol::TCP) {
                if (m_tcpP2PSocket->state() != QTcpSocket::ConnectedState) {
                    m_tcpP2PSocket->abort();
                    m_tcpP2PSocket->connectToHost(m_targetIp, m_targetTcpPort);
                    // ========== 优化：打洞成功时增加连接超时时间 ==========
                    int connectTimeout = (m_holePunchState == HolePunchState::Success) ? 3000 : 2000;
                    if (!m_tcpP2PSocket->waitForConnected(connectTimeout)) {
                        QMetaObject::invokeMethod(this, "showMessage",
                            Qt::QueuedConnection,
                            Q_ARG(QString, "系统错误"),
                            Q_ARG(QString, QString("连接接收方TCP端口失败：%1").arg(m_tcpP2PSocket->errorString())));
                        // ========== 新增：TCP直连失败自动切换到中继 ==========
                        // ========== 修复：先构造QString再传给Q_ARG ==========
                        QMetaObject::invokeMethod(this, "showMessage",
                            Qt::QueuedConnection,
                            Q_ARG(QString, "系统提示"),
                            Q_ARG(QString, QString("TCP直连失败，切换到中继模式重试分片%1").arg(idx)));
                        // 强制使用中继并重试当前分片
                        useRelay = true;
                        idx--; // 回退分片索引，重新发送
                        continue;
                    }
                }

                qint64 sent = m_tcpP2PSocket->write(fragData.c_str(), fragData.size());
                if (sent <= 0 || !m_tcpP2PSocket->waitForBytesWritten(1000)) {
                    QMetaObject::invokeMethod(this, "showMessage",
                        Qt::QueuedConnection,
                        Q_ARG(QString, "系统错误"),
                        Q_ARG(QString, QString("发送分片%1失败").arg(idx)));
                    sendSuccess = false;
                    break;
                }
            } else {
                // ========== 优化：打洞成功的UDP直连逻辑 ==========
                qint64 sent = -1;
                if (m_holePunchState == HolePunchState::Success) {
                    // 打洞成功，直接发送原始分片数据（无需封装）
                    sent = m_udpP2PSocket->writeDatagram(fragmentData, m_targetIp, m_targetUdpPort);
                } else {
                    // 未打洞，发送序列化后的分片消息
                    sent = m_udpP2PSocket->writeDatagram(fragData.c_str(), fragData.size(), m_targetIp, m_targetUdpPort);
                }
                
                if (sent <= 0) {
                    QMetaObject::invokeMethod(this, "showMessage",
                        Qt::QueuedConnection,
                        Q_ARG(QString, "系统错误"),
                        Q_ARG(QString, QString("发送UDP分片%1失败").arg(idx)));
                    // ========== 新增：UDP直连失败自动切换到中继 ==========
                    // ========== 修复：先构造QString再传给Q_ARG ==========
                    QMetaObject::invokeMethod(this, "showMessage",
                        Qt::QueuedConnection,
                        Q_ARG(QString, "系统提示"),
                        Q_ARG(QString, QString("UDP直连失败，切换到中继模式重试分片%1").arg(idx)));
                    useRelay = true;
                    idx--; // 回退分片索引，重新发送
                    continue;
                } else {
                    QMetaObject::invokeMethod(this, "showMessage",
                        Qt::QueuedConnection,
                        Q_ARG(QString, "系统提示"),
                        Q_ARG(QString, QString("UDP分片%1直连发送成功（%2字节）").arg(idx).arg(sent)));
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
        // ========== 修复：useRelay已在外部定义，可正常访问 ==========
        QString transferMode = (m_holePunchState == HolePunchState::Success) ? "P2P直连" : (useRelay ? "中继" : "尝试直连");
        QMetaObject::invokeMethod(this, "showMessage",
            Qt::QueuedConnection,
            Q_ARG(QString, m_nickname),
            Q_ARG(QString, QString("文件 %1 发送完成（%2，MD5：%3）").arg(QString::fromStdString(metaMsg.fileName)).arg(transferMode).arg(QString::fromStdString(metaMsg.fileMd5))));
    } else {
        QMetaObject::invokeMethod(this, "showMessage",
            Qt::QueuedConnection,
            Q_ARG(QString, "系统错误"),
            Q_ARG(QString, QString("文件 %1 发送失败").arg(QString::fromStdString(metaMsg.fileName))));
    }

    m_transferFile->close();
    // ========== 新增：传输完成后重置打洞状态 ==========
    m_holePunchState = HolePunchState::Idle;
}

// 更新传输进度
void ChatWindow::updateTransferProgress(int progress) {
    ui->progressBar_transfer->setValue(progress);
}

// ========== 新增：中继数据处理入口（优先处理） ==========
// 该函数供onRelayTcpReadyRead/onRelayUdpReadyRead调用，处理单个中继分片
void ChatWindow::processRelayFragment(int senderId, const FileMsg& fragMsg, bool isRelayData, bool isBase64) {
    int realSenderId = senderId;
    if (realSenderId == 0) {
        if (!m_relayMetaMap.isEmpty()) {
            realSenderId = m_relayMetaMap.keys().first();
        } else {
            QMetaObject::invokeMethod(this, "showMessage",
                Qt::QueuedConnection,
                Q_ARG(QString, "系统错误"),
                Q_ARG(QString, "senderId为空且无元信息缓存，跳过分片处理"));
            return;
        }
    }

    // 新增：获取文件元信息（解决metaMsg未定义问题）
    FileMsg metaMsg;
    if (m_relayMetaMap.contains(realSenderId)) {
        metaMsg = m_relayMetaMap[realSenderId];
    }

    QString saveDir = m_fileRecvDir;
    QString fileName = QString::fromStdString(fragMsg.fileName).split("/").last();
    QDir dir(saveDir);
    // 修复：创建完整目录而非当前目录
    if (!dir.exists()) {
        if (!dir.mkpath(saveDir)) {
            QMetaObject::invokeMethod(this, "showMessage",
                Qt::QueuedConnection,
                Q_ARG(QString, "系统错误"),
                Q_ARG(QString, QString("创建文件保存目录失败：%1").arg(saveDir)));
            return;
        }
    }
    QString savePath = QString("%1/relay_%2_%3").arg(saveDir).arg(realSenderId).arg(fileName);

    // 核心修复：跳过元信息分片（idx=0）的空数据检查
    QByteArray fragmentData;
    if (fragMsg.fragmentIdx == 0) {
        // 元信息分片，仅缓存不写入
        m_relayMetaMap[realSenderId] = fragMsg;
        QMetaObject::invokeMethod(this, "showMessage",
            Qt::QueuedConnection,
            Q_ARG(QString, "系统提示"),
            Q_ARG(QString, QString("缓存文件元信息（senderId：%1，分片数：%2）").arg(realSenderId).arg(fragMsg.totalFragments)));
        return;
    }

    // 修复：根据isBase64标记决定是否解码
    if (isRelayData && isBase64 && !fragMsg.base64Data.empty()) {
        // 中继数据+Base64：需要解码
        QByteArray base64Data = QByteArray::fromStdString(fragMsg.base64Data);
        fragmentData = QByteArray::fromBase64(base64Data);
        if (fragmentData.isEmpty()) {
            QMetaObject::invokeMethod(this, "showMessage",
                Qt::QueuedConnection,
                Q_ARG(QString, "系统错误"),
                Q_ARG(QString, QString("分片%1 Base64解码失败，数据为空").arg(fragMsg.fragmentIdx)));
            return;
        }
    } else if (!isRelayData && !fragMsg.base64Data.empty()) {
        // 直连数据：直接使用原始二进制，不解码
        fragmentData = QByteArray::fromStdString(fragMsg.base64Data);
        // 新增：UDP直连数据日志
        if (metaMsg.protocol == TransportProtocol::UDP) {
            QMetaObject::invokeMethod(this, "showMessage",
                Qt::QueuedConnection,
                Q_ARG(QString, "系统提示"),
                Q_ARG(QString, QString("处理UDP直连分片%1，原始数据长度：%2").arg(fragMsg.fragmentIdx).arg(fragmentData.size())));
        }
    }

    if (fragmentData.isEmpty()) {
        QMetaObject::invokeMethod(this, "showMessage",
            Qt::QueuedConnection,
            Q_ARG(QString, "系统错误"),
            Q_ARG(QString, QString("分片%1数据为空，跳过写入").arg(fragMsg.fragmentIdx)));
        return;
    }

    m_relayFragMap[realSenderId][fragMsg.fragmentIdx] = fragmentData;

    int progress = (m_relayFragMap[realSenderId].size() * 100) / fragMsg.totalFragments;

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

    if (m_relayFragMap[realSenderId].size() == fragMsg.totalFragments || 
        (fragMsg.totalFragments == 1 && m_relayFragMap[realSenderId].contains(1))) {
        QFile file(savePath);
        if (file.open(QIODevice::WriteOnly)) {
            qint64 total = 0;
            // 按分片ID顺序写入（关键：避免乱序）
            for (uint32_t i = 1; i <= fragMsg.totalFragments; ++i) {
                if (m_relayFragMap[realSenderId].contains(i)) {
                    QByteArray data = m_relayFragMap[realSenderId][i];
                    QByteArray writeData;
                    // 修复：区分UDP直连/中继数据的解码逻辑
                    if (!isRelayData && metaMsg.protocol == TransportProtocol::UDP) {
                        // UDP直连：直接使用原始数据
                        writeData = data;
                    } else if (isRelayData && isBase64) {
                        // 中继数据：Base64解码
                        writeData = QByteArray::fromBase64(data);
                    } else {
                        // 其他直连数据：直接使用
                        writeData = data;
                    }

                    if (!writeData.isEmpty()) { // 非空才写入
                        qint64 writeLen = file.write(writeData);
                        if (writeLen > 0) {
                            total += writeLen;
                        } else {
                            QMetaObject::invokeMethod(this, "showMessage",
                                Qt::QueuedConnection,
                                Q_ARG(QString, "系统错误"),
                                Q_ARG(QString, QString("写入分片%1失败，写入长度：%2").arg(i).arg(writeLen)));
                        }
                    } else {
                        QMetaObject::invokeMethod(this, "showMessage",
                            Qt::QueuedConnection,
                            Q_ARG(QString, "系统提示"),
                            Q_ARG(QString, QString("分片%1数据为空，跳过写入").arg(i)));
                    }
                } else {
                    QMetaObject::invokeMethod(this, "showMessage",
                        Qt::QueuedConnection,
                        Q_ARG(QString, "系统错误"),
                        Q_ARG(QString, QString("缺失分片%1，文件可能损坏").arg(i)));
                }
            }
            // 强制刷盘（关键：确保数据写入磁盘）
            file.flush();
            file.close();

            QString saveMsg = QString("[中继合并] 文件保存成功！路径：%1，实际大小：%2 字节")
                .arg(savePath)
                .arg(total);
            
            QMetaObject::invokeMethod(this, "showMessage",
                Qt::QueuedConnection,
                Q_ARG(QString, "系统提示"),
                Q_ARG(QString, saveMsg));
        } else {
            QMetaObject::invokeMethod(this, "showMessage",
                Qt::QueuedConnection,
                Q_ARG(QString, "系统错误"),
                Q_ARG(QString, QString("打开文件失败：%1").arg(file.errorString())));
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
        Q_ARG(QString, "已缓存文件元信息，优先等待直连接收..."));

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
    processRelayFragment(fragMsg.senderId, fragMsg, true);
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
    if (!m_udpP2PSocket) return;

    while (m_udpP2PSocket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(m_udpP2PSocket->pendingDatagramSize());
        QHostAddress senderAddr;
        quint16 senderPort;
        qint64 readLen = m_udpP2PSocket->readDatagram(datagram.data(), datagram.size(), &senderAddr, &senderPort);
        if (readLen <= 0) continue;
        
        // 处理中继UDP数据（格式：senderId|receiverId|数据）
        QStringList relayParts = QString(datagram).split("|");
        if (relayParts.size() >= 3) {
            bool senderIdOk = false;
            bool receiverIdOk = false;
            int senderId = relayParts[0].toInt(&senderIdOk);
            int receiverId = relayParts[1].toInt(&receiverIdOk);
            
            // 校验ID有效性
            if (!senderIdOk || !receiverIdOk || (receiverId != m_userId && receiverId != 0)) {
                showMessage("系统提示", "[UDP中继] 无效的中继数据：ID解析失败或非本机数据");
                continue;
            }

            QByteArray realData = relayParts[2].toUtf8();
            if (realData.isEmpty()) {
                showMessage("系统提示", "[UDP中继] 中继数据为空，忽略");
                continue;
            }

            showMessage("系统提示", QString("[UDP中继] 解析到中继数据（senderId：%1，receiverId：%2），长度：%3字节")
                        .arg(senderId).arg(receiverId).arg(realData.size()));

            try {
                // 处理中继文件分片
                FileMsg fragMsg = deserializeFileMsg(realData.toStdString());
                if (fragMsg.senderId == 0) fragMsg.senderId = senderId;
                processRelayFragment(senderId, fragMsg, true); // 标记为中继数据
            } catch (std::exception& e) {
                // 处理中继图片数据
                if (!m_pendingImageMsg.fileName.empty() && m_pendingImageMsg.fileSize > 0) {
                    m_pendingImageData = QByteArray::fromBase64(realData);
                    showMessage("系统提示", QString("[UDP中继] 接收图片数据：%1/%2字节")
                                .arg(m_pendingImageData.size()).arg(m_pendingImageMsg.fileSize));
                    
                    if (m_pendingImageData.size() >= m_pendingImageMsg.fileSize) {
                        QString saveDir = m_imgRecvDir;
                        QDir dir(saveDir);
                        if (!dir.exists()) dir.mkpath(saveDir);
                        
                        QString fileName = QString::fromStdString(m_pendingImageMsg.fileName).split("/").last();
                        QString savePath = QString("%1/udp_relay_%2_%3").arg(saveDir).arg(senderId).arg(fileName);
                        
                        QFile file(savePath);
                        if (file.open(QIODevice::WriteOnly)) {
                            qint64 writeLen = file.write(m_pendingImageData);
                            file.flush();
                            file.close();
                            showMessage("系统提示", QString("[UDP中继] 图片保存完成：%1（大小：%2字节）").arg(savePath).arg(writeLen));
                        } else {
                            showMessage("系统错误", QString("[UDP中继] 保存图片失败：%1").arg(file.errorString()));
                        }
                        m_pendingImageMsg = ImageMsg();
                        m_pendingImageData.clear();
                    }
                }
            }
            continue;
        }

        // 处理直连UDP数据（无中继格式）
        try {
            // 直连文件分片：优先解析为FileMsg
            FileMsg fragMsg = deserializeFileMsg(datagram.toStdString());
            showMessage("系统提示", QString("收到UDP文件分片：来自%1:%2，分片ID：%3，数据长度：%4字节")
                        .arg(senderAddr.toString())
                        .arg(senderPort)
                        .arg(fragMsg.fragmentIdx)
                        .arg(fragMsg.base64Data.size()));
            processRelayFragment(fragMsg.senderId, fragMsg, false, false); // 标记为直连数据
        } catch (std::exception& e) {
            // 解析失败：直接使用原始二进制数据（打洞成功时发送的原始数据）
            showMessage("系统提示", QString("收到UDP原始分片数据：来自%1:%2，长度%3字节（解析FileMsg失败，直接处理）")
                        .arg(senderAddr.toString())
                        .arg(senderPort)
                        .arg(datagram.size()));
            // 构造临时FileMsg，填充原始数据
            FileMsg tempFrag;
            tempFrag.senderId = m_relayMetaMap.isEmpty() ? 0 : m_relayMetaMap.keys().first();
            tempFrag.base64Data = datagram.toStdString();
            tempFrag.fragmentIdx = 1; // 单分片默认ID为1
            tempFrag.totalFragments = 1;
            processRelayFragment(tempFrag.senderId, tempFrag, false, false);
        }
    }
}

// 显示聊天消息
void ChatWindow::showMessage(const QString& sender, const QString& content, bool isPrivate) {
    // 步骤1：处理私聊标记（仅用于显示）
    QString realSender = isPrivate ? sender + "(私聊)" : sender;
    
    // 步骤2：判断是否是自己发送的消息（核心：决定气泡方向）
    bool isSelf = false;
    // 匹配规则：sender是自己的昵称，或包含自己昵称+私聊
    if (sender == m_nickname || 
        sender.startsWith(m_nickname + "(私聊)") || 
        sender.startsWith(m_nickname + "(私聊→")) {
        isSelf = true;
    }
    // 系统消息/别人消息：isSelf=false
    else if (sender != "系统提示" && sender != "系统错误" && sender != "未知用户") {
        isSelf = false;
    }

    // 步骤3：调用气泡消息函数（所有showMessage都变成气泡样式）
    addMessage(realSender, content, isSelf);
}

// 获取在线用户
void ChatWindow::getOnlineUserList() {
    if (!m_socket || m_socket->state() != QTcpSocket::ConnectedState) {
        showMessage("系统提示", "未连接到服务器，无法获取在线用户！");
        return;
    }

    if (m_userId <= 0) {
        showMessage("系统提示", "用户未登录，无法获取在线用户！");
        return;
    }

    // 清空之前的用户列表缓存
    m_userPortMap.clear();
    
    // 发送用户列表请求（确保数据为空字符串）
    bool ret = sendPacket(
        m_socket, 
        static_cast<uint32_t>(MsgType::USER_LIST_REQ), 
        "",  // 明确的空数据
        nextMsgId++, 
        m_userId
    );

    if (ret) {
        showMessage("系统提示", "已发送在线用户列表请求，请等待响应...");
        // 增加超时处理
        QTimer::singleShot(5000, this, [=]() {
            if (ui->listWidget_onlineUsers->count() == 0) {
                showMessage("系统提示", "获取用户列表超时，尝试重新获取...");
                getOnlineUserList(); // 重试一次
            }
        });
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

    // 严格判断：仅当|分隔后的receiverId匹配本机ID时，才认为是中继数据
    if (rawData.contains('|')) {
        QStringList relayParts = QString(rawData).split("|");
        if (relayParts.size() >= 3) {
            int senderId = relayParts[0].toInt();
            int receiverId = relayParts[1].toInt();
            
            // 仅当接收方是本机时，才解析为中继数据
            if (receiverId == m_userId) {
                realImageData = QByteArray::fromBase64(relayParts[2].toUtf8());
                isRelayData = true;
                relaySenderId = senderId;
            } else {
                // 不是本机中继数据，直接作为直连图片数据
                realImageData = rawData;
            }
        } else {
            // 格式错误，作为直连数据
            realImageData = rawData;
        }
    } else {
        // 无|分隔符，直连图片数据
        realImageData = rawData;
    }

    // ========== 校验元信息 ==========
    if (m_pendingImageMsg.fileSize <= 0) {
        showMessage("系统提示", "无有效的图片元信息，保存为临时文件");
        // 保存临时图片（避免数据丢失）
        QString saveDir = m_imgRecvDir;
        QDir dir(saveDir);
        if (!dir.exists()) dir.mkpath(saveDir);
    QString timestamp = QTime::currentTime().toString("hhmmsszzz");
    // 优先从元信息获取原文件名和后缀（TCP临时文件专用命名）
    QString fileName = QString("tcp_temp_img_%1").arg(timestamp);
    if (!m_relayMetaMap.isEmpty()) {
        fileName = QString::fromStdString(m_relayMetaMap.begin().value().fileName).split("/").last();
        // 确保文件名唯一：追加时间戳避免覆盖
        fileName = fileName.split(".").first() + "_" + timestamp + "." + fileName.split(".").last();
    }
    // 无后缀时补充通用后缀
    if (!fileName.contains(".")) {
        fileName += ".bin";
    }
    QString savePath = QString("%1/%2").arg(saveDir).arg(fileName);
        
        QFile file(savePath);
        if (file.open(QIODevice::WriteOnly)) {
            qint64 writeLen = file.write(realImageData);
            file.flush();
            file.close();
            showMessage("系统提示", QString("临时图片保存完成：%1（大小：%2字节）").arg(savePath).arg(writeLen));
        }
        return;
    }

    // ========== 累加数据并保存 ==========
    m_pendingImageData += realImageData;
    m_imgRecvTimeoutTimer->start(10000);

    showMessage("系统提示", QString("已接收%1图片数据：%2/%3字节（来自用户%4）")
                .arg(isRelayData ? "中继" : "直连")
                .arg(m_pendingImageData.size()).arg(m_pendingImageMsg.fileSize).arg(relaySenderId));

    // 实时打印累加进度，处理冗余数据，仅匹配长度时保存
    showMessage("系统提示", QString("图片数据累加中：当前%1字节 / 目标%2字节")
                .arg(m_pendingImageData.size()).arg(m_pendingImageMsg.fileSize));
    
    bool isDataComplete = false;
    // 处理冗余数据：截断到目标长度
    if (m_pendingImageData.size() > m_pendingImageMsg.fileSize) {
        m_pendingImageData = m_pendingImageData.left(m_pendingImageMsg.fileSize);
        showMessage("系统提示", "图片数据超出目标长度，已截断至实际大小");
        isDataComplete = true;
    } else if (m_pendingImageData.size() == m_pendingImageMsg.fileSize) {
        isDataComplete = true;
    }

    // 仅当数据完整（匹配/截断后匹配）时保存
    if (isDataComplete) {
        m_imgRecvTimeoutTimer->stop(); // 数据完整，停止超时定时器
        
        // 校验MD5（可选）
        QCryptographicHash hash(QCryptographicHash::Md5);
        hash.addData(m_pendingImageData);
        QString md5 = hash.result().toHex();
        
        // 保存图片（优化：追加senderId避免文件名冲突）
        QString saveDir = m_imgRecvDir;
        QDir dir(saveDir);
        if (!dir.exists()) dir.mkpath(saveDir);
        
        QString fileName = QString::fromStdString(m_pendingImageMsg.fileName).split("/").last();
        // 追加发送方ID，避免多用户发送同名文件覆盖
        fileName = QString("%1_%2").arg(relaySenderId).arg(fileName);
        QString savePath = QString("%1/%2").arg(saveDir).arg(fileName);

        QFile file(savePath);
        if (file.open(QIODevice::WriteOnly)) {
            qint64 writeLen = file.write(m_pendingImageData);
            file.flush();
            file.close();
            showMessage("系统提示", QString("图片接收完成：%1（大小：%2字节，MD5：%3）")
                        .arg(savePath).arg(writeLen).arg(md5));
        } else {
            showMessage("系统错误", QString("保存图片失败：%1").arg(file.errorString()));
        }
        
        // 重置缓存（必须：避免下次接收数据污染）
        m_pendingImageMsg = ImageMsg();
        m_pendingImageData.clear();
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
            QString saveDir = m_imgRecvDir;
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
            QString saveDir = m_imgRecvDir;
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

// 生成气泡HTML（核心：微信风格气泡）
QString ChatWindow::generateBubbleHtml(const QString& sender, const QString& content, bool isSelf) {
    QString safeContent = content.toHtmlEscaped();
    safeContent.replace("\n", "<br>");

    // 区分消息类型（图片/文件/系统/文本）
    QString contentStyle = "";
    if (content.contains("[图片]") || content.contains(".jpg") || content.contains(".png")) {
        // 图片消息：加图标+蓝色文字
        safeContent = QString("<span style='color:#1677FF;'>📷 %1</span>").arg(safeContent);
    } else if (content.contains("[文件]") || content.contains(".zip") || content.contains(".txt")) {
        // 文件消息：加图标+绿色文字
        safeContent = QString("<span style='color:#07C160;'>📎 %1</span>").arg(safeContent);
    } else if (sender == "系统提示" || sender == "系统错误") {
        // 系统消息：灰色文字
        safeContent = QString("<span style='color:#888;'>ℹ️ %1</span>").arg(safeContent);
    }

    // 气泡样式（优化颜色和圆角，更贴近微信）
    QString bubbleStyle = isSelf 
        ? R"(
            style='background:#07C160; color:white; border-radius:18px 18px 0 18px; 
                   padding:10px 15px; margin:5px 0; max-width:70%; float:right;
                   box-shadow: 0 1px 2px rgba(0,0,0,0.1);'
        )" 
        : R"(
            style='background:white; color:#333; border-radius:18px 18px 18px 0; 
                   padding:10px 15px; margin:5px 0; max-width:70%; float:left;
                   box-shadow: 0 1px 2px rgba(0,0,0,0.1);'
        )";

    // 系统消息特殊样式（居中+浅灰色气泡）
    if (sender == "系统提示" || sender == "系统错误") {
        bubbleStyle = R"(
            style='background:#F5F5F5; color:#888; border-radius:12px; 
                   padding:8px 12px; margin:5px auto; max-width:60%; text-align:center;
                   display:block; float:none;'
        )";
    }

    return QString(R"(
        <div style='width:100%; overflow:hidden; margin:8px 0;'>
            <div %1>
                <div style='font-size:12px; margin-bottom:4px; %2'>%3</div>
                <div style='font-size:14px; line-height:1.5;'>%4</div>
                <div style='font-size:10px; color:#ccc; margin-top:4px; text-align:right;'>%5</div>
            </div>
        </div>
        <div style='clear:both;'></div>
    )").arg(bubbleStyle)
      .arg(isSelf ? "color:rgba(255,255,255,0.8);" : "color:#888;")
      .arg(sender)
      .arg(safeContent)
      .arg(QTime::currentTime().toString("HH:mm")); // 追加消息时间
}

// 新增：从sender字符串提取用户ID
int ChatWindow::parseUserIdFromSender(const QString& sender) {
    // 匹配规则：从sender中提取数字ID（支持"用户123"、"123-昵称"、"昵称(ID:123)"等格式）
    QRegularExpression re(R"(\d+)"); // 匹配所有数字
    QRegularExpressionMatch match = re.match(sender);
    if (match.hasMatch()) {
        return match.captured().toInt();
    }
    return -1; // 无ID返回-1
}

// 添加消息到聊天框
void ChatWindow::addMessage(const QString& sender, const QString& content, bool isSelf) {
    // 1. 定义气泡样式（保留原有样式，优化命名）
    QString bubbleStyle = isSelf 
        ? R"(style='background:#07C160; color:white; border-radius:18px 18px 0 18px; padding:10px 15px; margin:5px 0; max-width:70%; box-shadow: 0 1px 2px rgba(0,0,0,0.1);')"
        : R"(style='background:white; color:#333; border-radius:18px 18px 18px 0; padding:10px 15px; margin:5px 0; max-width:70%; box-shadow: 0 1px 2px rgba(0,0,0,0.1);')";

    // 2. 复用封装好的函数获取头像路径（避免重复代码，保证路径正确）
    QString avatarPath = "qrc:/images/default_avatar.png";
    if (!isSelf) {
        int senderId = parseUserIdFromSender(sender);
        if (senderId > 0) {
            avatarPath = getUserAvatarPath(senderId); // 复用之前的函数，避免重复逻辑
        }
    } else {
        avatarPath = getUserAvatarPath(m_userId);
    }

    // 3. 禁用滚动条自动跳转（避免插入HTML时滚动条乱跑）
    QScrollBar* scrollBar = ui->textEdit_chatLog->verticalScrollBar();
    bool isAtBottom = (scrollBar->value() == scrollBar->maximum());

    // 4. 构建完整的带头像+消息内容的HTML（核心修复：补全内容，修正布局）
    QString bubbleHtml = QString(R"(
        <div style='width:100%; overflow:hidden; margin:8px 0;'>
            <!-- 头像：左/右浮动 -->
            <img src='%1' style='width:30px; height:30px; border-radius:15px; %2; margin:0 5px;'/>
            <!-- 消息气泡：对应浮动，包裹实际内容 -->
            <div %3 style='%4'>
                <div style='word-wrap: break-word;'>%5</div>
                <div style='font-size:10px; color:#999; margin-top:4px; text-align:right;'>%6</div>
            </div>
        </div>
    )").arg(
        avatarPath,                                    // %1: 头像路径
        isSelf ? "float:right;" : "float:left;",       // %2: 头像浮动方向
        isSelf ? "float:right;" : "float:left;",       // %3: 气泡浮动方向
        bubbleStyle,                                   // %4: 气泡样式
        content.toHtmlEscaped(),                       // %5: 消息内容（转义HTML，避免XSS/格式错乱）
        QDateTime::currentDateTime().toString("HH:mm") // %6: 发送时间
    );

    // 5. 只追加一次完整的HTML（修复重复追加问题）
    ui->textEdit_chatLog->append(bubbleHtml);

    // 6. 如果原本在底部，自动滚动到底部
    if (isAtBottom) {
        QTimer::singleShot(0, scrollBar, [scrollBar]() {
            scrollBar->setValue(scrollBar->maximum());
        });
    }
}

void ChatWindow::saveCompleteImage(int relaySenderId) {
    QCryptographicHash hash(QCryptographicHash::Md5);
    hash.addData(m_pendingImageData);
    QString md5 = hash.result().toHex();
    
    QString saveDir = m_imgRecvDir;
    QDir dir(saveDir);
    if (!dir.exists()) dir.mkpath(saveDir);
    
    QString fileName = QString::fromStdString(m_pendingImageMsg.fileName).split("/").last();
    fileName = QString("%1_%2").arg(relaySenderId).arg(fileName);
    QString savePath = QString("%1/%2").arg(saveDir).arg(fileName);

    QFile file(savePath);
    if (file.open(QIODevice::WriteOnly)) {
        qint64 writeLen = file.write(m_pendingImageData);
        file.flush();
        file.close();
        showMessage("系统提示", QString("图片接收完成：%1（大小：%2字节，MD5：%3）")
                    .arg(savePath).arg(writeLen).arg(md5));
    } else {
        showMessage("系统错误", QString("保存图片失败：%1").arg(file.errorString()));
    }
    
    // 重置缓存
    m_pendingImageMsg = ImageMsg();
    m_pendingImageData.clear();
}

QString ChatWindow::getUserAvatarPath(int userId) {
    // 1. 优先查缓存，避免重复编码
    if (m_avatarBase64Cache.contains(userId)) {
        return m_avatarBase64Cache[userId];
    }

    // 2. 原有逻辑（拼路径、加载图片、转Base64）
    QString avatarDir = QCoreApplication::applicationDirPath() + "/avatars/";
    QDir dir(avatarDir);
    if (!dir.exists()) dir.mkpath(avatarDir);

    QStringList filters;
    filters << QString("%1.png").arg(userId) 
            << QString("%1.jpg").arg(userId) 
            << QString("%1.jpeg").arg(userId);
    
    QStringList avatarFiles = dir.entryList(filters, QDir::Files);
    QString avatarFilePath;
    if (!avatarFiles.isEmpty()) {
        avatarFilePath = avatarDir + avatarFiles.first();
    }

    QPixmap avatarPixmap;
    QString base64Str;
    if (!avatarFilePath.isEmpty() && avatarPixmap.load(avatarFilePath)) {
        avatarPixmap = avatarPixmap.scaled(30, 30, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QByteArray ba;
        QBuffer buffer(&ba);
        buffer.open(QIODevice::WriteOnly);
        avatarPixmap.save(&buffer, "PNG"); 
        base64Str = QString("data:image/png;base64,%1").arg(QString(ba.toBase64()));
    } else if (avatarPixmap.load(":/images/default_avatar.png")) {
        avatarPixmap = avatarPixmap.scaled(30, 30, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QByteArray ba;
        QBuffer buffer(&ba);
        buffer.open(QIODevice::WriteOnly);
        avatarPixmap.save(&buffer, "PNG");
        base64Str = QString("data:image/png;base64,%1").arg(QString(ba.toBase64()));
    }

    // 3. 缓存结果（仅缓存有效字符串）
    if (!base64Str.isEmpty()) {
        m_avatarBase64Cache[userId] = base64Str;
    }

    return base64Str;
}

// 设置当前用户头像（显示在label_userInfo，图文结合）
void ChatWindow::setSelfAvatar(const QPixmap& pixmap) {
    if (!ui || !ui->label_userInfo) return;
    
    // 调整标签为水平布局：头像 + 文字
    QHBoxLayout* layout = new QHBoxLayout(ui->label_userInfo);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(5);
    
    // 头像Label（大小64x64）
    QLabel* avatarLabel = new QLabel(ui->label_userInfo);
    avatarLabel->setFixedSize(64, 64);
    avatarLabel->setPixmap(pixmap.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    avatarLabel->setScaledContents(true);
    layout->addWidget(avatarLabel);
    
    // 文字Label（显示用户信息）
    QLabel* textLabel = new QLabel(ui->label_userInfo);
    textLabel->setText(QString("当前用户：%1（ID：%2）").arg(m_selfNickname).arg(m_selfUserId));
    layout->addWidget(textLabel);
    
    ui->label_userInfo->setLayout(layout);
    qDebug() << "[ChatWindow] 当前用户头像已显示在label_userInfo";
}

// 给在线用户列表项添加头像（自定义列表项）
void ChatWindow::addUserItemWithAvatar(int userId, const QString& nickname, const QPixmap& avatar) {
    if (!ui || !ui->listWidget_onlineUsers) return;
    
    // 创建自定义列表项
    QListWidgetItem* item = new QListWidgetItem(ui->listWidget_onlineUsers);
    item->setSizeHint(QSize(0, 50)); // 设置项高度
    
    // 项的布局：头像 + 用户名
    QWidget* itemWidget = new QWidget;
    QHBoxLayout* itemLayout = new QHBoxLayout(itemWidget);
    itemLayout->setContentsMargins(5, 5, 5, 5);
    itemLayout->setSpacing(10);
    
    // 头像Label（48x48）
    QLabel* avatarLabel = new QLabel(itemWidget);
    avatarLabel->setFixedSize(48, 48);
    avatarLabel->setPixmap(avatar.scaled(48, 48, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    avatarLabel->setScaledContents(true);
    itemLayout->addWidget(avatarLabel);
    
    // 用户名Label
    QLabel* nameLabel = new QLabel(itemWidget);
    nameLabel->setText(QString("用户%1：%2").arg(userId).arg(nickname.isEmpty() ? "未命名" : nickname));
    itemLayout->addWidget(nameLabel);
    
    ui->listWidget_onlineUsers->addItem(item);
    ui->listWidget_onlineUsers->setItemWidget(item, itemWidget);
    qDebug() << "[ChatWindow] 在线用户" << userId << "头像已添加到列表";
}