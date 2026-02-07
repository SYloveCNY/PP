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
#include <QRegularExpression>
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
    , m_udpP2PSocket(nullptr)
    , m_transferFile(nullptr)
    , m_totalFileSize(0)
    , m_sentFileSize(0)
    , m_userInfoLabel(nullptr) // 显式初始化标签指针
    , m_selectedUserId(0)
    , m_p2pTcpPort(0)
    , m_udpP2PPort(0)
{
    // 加载UI文件（替代手动创建UI）
    ui->setupUi(this);
    this->setWindowTitle("聊天窗口");

    // 添加用户信息标签到聊天区域顶部
    m_userInfoLabel = new QLabel("当前用户：未登录", this);
    ui->verticalLayout->insertWidget(0, m_userInfoLabel); // 插入到聊天框上方

    // ========== 新增：P2P Socket初始化 ==========
    m_tcpP2PSocket = new QTcpSocket(this);   // 点对点TCP Socket
    m_udpP2PSocket = new QUdpSocket(this);   // 点对点UDP Socket

    // ========== 新增：P2P TCP服务器初始化 ==========
    // 绑定TCP服务器新连接信号到槽函数（接收其他客户端的P2P连接）
    connect(m_tcpP2PServer, &QTcpServer::newConnection, 
            this, &ChatWindow::onP2PNewConnection);

    // ========== 新增：进度条初始化 ==========
    ui->progressBar_transfer->setRange(0, 100); // 进度范围0-100
    ui->progressBar_transfer->setValue(0);     // 初始进度0

    // ========== 信号槽绑定（原有+新增） ==========
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

    connect(m_udpP2PSocket, &QUdpSocket::readyRead, this, &ChatWindow::onUdpReadyRead);

    // 启动心跳定时器
    m_heartbeatTimer->start(10000);
}

ChatWindow::~ChatWindow() {
    delete ui; // 释放UI对象
}

// 将P2P端口初始化抽离为独立函数，确保端口绑定完成后再返回
bool ChatWindow::initP2PPorts() {
    // 初始化P2P TCP服务端
    if (m_tcpP2PServer->isListening()) {
        m_tcpP2PServer->close(); // 关闭重复的监听
    }
    bool tcpServerListenOk = m_tcpP2PServer->listen(QHostAddress::Any, 0);
    if (!tcpServerListenOk) {
        showMessage("系统错误", "P2P TCP服务端启动失败：" + m_tcpP2PServer->errorString());
        return false;
    }
    m_p2pTcpPort = m_tcpP2PServer->serverPort();
    if (m_p2pTcpPort <= 0 || m_p2pTcpPort > 65535) {
        showMessage("系统错误", "P2P TCP服务端获取端口失败，端口无效");
        m_tcpP2PServer->close();
        return false;
    }
    showMessage("系统提示", QString("P2P TCP服务端启动成功，监听随机端口：%1").arg(m_p2pTcpPort));

    // 初始化UDP客户端
    if (m_udpP2PSocket->state() != QUdpSocket::UnconnectedState) {
        m_udpP2PSocket->close(); // 关闭旧连接
    }
    bool udpBindOk = m_udpP2PSocket->bind(QHostAddress::Any, 0);
    if (!udpBindOk) {
        showMessage("系统错误", "UDP客户端启动失败：" + m_udpP2PSocket->errorString());
        return false;
    }
    m_udpP2PPort = m_udpP2PSocket->localPort(); // 新增成员变量存储UDP端口
    if (m_udpP2PPort <= 0 || m_udpP2PPort > 65535) {
        showMessage("系统错误", "UDP Socket绑定成功，但获取到无效端口");
        m_udpP2PSocket->close();
        return false;
    }
    showMessage("系统提示", QString("UDP客户端已启动，监听端口：%1").arg(m_udpP2PPort));

    return true;
}

// 登录流程：先初始化P2P端口，再发送登录请求
void ChatWindow::login(const QString& nickname) {
    // 第一步：确保P2P端口初始化完成
    if (!initP2PPorts()) {
        showMessage("系统错误", "P2P端口初始化失败，无法登录");
        return;
    }

    // 第二步：发送登录请求（此时端口已正确绑定）
    sendLoginReq(nickname);
}

// 修正sendLoginReq函数
void ChatWindow::sendLoginReq(const QString& nickname) {
    // 前置校验：端口必须有效
    if (m_p2pTcpPort == 0 || m_udpP2PPort == 0) {
        showMessage("系统错误", "P2P端口未初始化，无法发送登录请求");
        return;
    }
    
    LoginReq req;
    req.nickname = nickname.toStdString();
    req.dataPort = m_p2pTcpPort; // 使用初始化后的TCP端口
    req.udpPort = m_udpP2PPort;  // 使用初始化后的UDP端口
    
    std::string jsonStr = nlohmann::json(req).dump();
    sendPacket(m_socket, static_cast<uint32_t>(MsgType::LOGIN_REQ), jsonStr, nextMsgId++, g_currentUserId);
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
                    showMessage("系统提示", QString("收到图片消息：%1（大小：%2字节），准备接收...")
                                .arg(QString::fromStdString(imgMsg.fileName))
                                .arg(imgMsg.fileSize));
                    
                    // 启动接收图片线程
                    std::thread recvImgThread(&ChatWindow::receiveImage, this, imgMsg);
                    recvImgThread.detach();
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

    // 3. 再建立P2P TCP连接并发送图片（增加重试逻辑）
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
        showMessage("系统提示", QString("图片数据发送成功：%1字节").arg(sent));
    } else {
        showMessage("系统错误", "图片数据发送失败：" + m_tcpP2PSocket->errorString());
    }

    // 关闭P2P连接
    m_tcpP2PSocket->disconnectFromHost();
}

void ChatWindow::sendImage() {
    sendImageDialog();
}

// 接收图片
void ChatWindow::receiveImage(const ImageMsg& msg) {
    QString savePath = QString("./recv_imgs/%1").arg(QString::fromStdString(msg.fileName));
    QDir dir;
    if (!dir.exists("./recv_imgs")) {
        dir.mkdir("./recv_imgs");
    }

    if (!m_tcpP2PServer->isListening()) {
        showMessage("系统提示", "P2P TCP服务端未启动，无法接收图片");
        return;
    }
    showMessage("系统提示", QString("等待发送方P2P连接，当前监听端口：%1").arg(m_tcpP2PServer->serverPort()));

    // 等待连接（改为信号槽，避免阻塞）
    QEventLoop loop;
    connect(m_tcpP2PServer, &QTcpServer::newConnection, &loop, &QEventLoop::quit);
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeoutTimer.start(10000); // 10秒超时

    loop.exec(QEventLoop::ExcludeUserInputEvents);

    // 检查是否超时
    if (!timeoutTimer.isActive() && !m_tcpP2PServer->hasPendingConnections()) {
        showMessage("系统提示", "等待P2P连接超时");
        return;
    }

    QTcpSocket* clientSocket = m_tcpP2PServer->nextPendingConnection();
    if (!clientSocket) {
        showMessage("系统提示", "未接收到P2P连接");
        return;
    }
    clientSocket->setParent(this); // 指定父对象，自动管理内存

    // 读取图片数据（循环读取，避免半包）
    QByteArray imgData;
    while (clientSocket->bytesAvailable() > 0 || clientSocket->waitForReadyRead(5000)) {
        imgData += clientSocket->readAll();
        // 校验数据大小，避免无限读取
        if (imgData.size() >= msg.fileSize) {
            break;
        }
    }

    // 保存图片
    QFile file(savePath);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(imgData);
        file.close();
        QMetaObject::invokeMethod(this, "showMessage",
            Qt::QueuedConnection,
            Q_ARG(QString, "系统提示"),
            Q_ARG(QString, QString("图片已保存至：%1（大小：%2字节）").arg(savePath).arg(imgData.size())));
    } else {
        QMetaObject::invokeMethod(this, "showMessage",
            Qt::QueuedConnection,
            Q_ARG(QString, "系统提示"),
            Q_ARG(QString, "保存图片失败：" + file.errorString()));
    }

    clientSocket->close();
    clientSocket->deleteLater();
}

// 发送文件
void ChatWindow::sendFile() {
    QString filePath = QFileDialog::getOpenFileName(this, "选择文件", "", "所有文件 (*.*)");
    if (filePath.isEmpty()) return;
    
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        showMessage("系统提示", "打开文件失败：" + file.errorString());
        return;
    }
    
    // 获取文件信息
    m_totalFileSize = file.size();
    m_sentFileSize = 0;
    m_transferFile = &file;
    
    // 计算MD5
    QCryptographicHash hash(QCryptographicHash::Md5);
    if (hash.addData(&file)) {
        QString fileMd5 = hash.result().toHex();
        
        // 构造文件元信息
        FileMsg fileMsg;
        fileMsg.senderId = m_userId;
        fileMsg.receiverId = ui->lineEdit_privateId->text().toInt();
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
        file.close();
    }
}

// 分片发送文件
void ChatWindow::sendFileFragment(FileMsg metaMsg) {
    const qint64 fragmentSize = 1024 * 1024; // 1MB
    qint64 offset = 0;
    
    for (uint32_t idx = 1; idx <= metaMsg.totalFragments; idx++) {
        // 读取分片数据
        m_transferFile->seek(offset);
        QByteArray fragmentData = m_transferFile->read(fragmentSize);
        
        // 构造分片消息
        FileMsg fragMsg = metaMsg;
        fragMsg.fragmentIdx = idx;
        fragMsg.fileData = fragmentData.toBase64().toStdString();
        fragMsg.dataLen = fragmentData.size();
        
        // 点对点发送
        std::string fragData = serialize(fragMsg);
        if (metaMsg.protocol == TransportProtocol::TCP) {
            if (m_tcpP2PSocket->state() != QTcpSocket::ConnectedState) {
                m_tcpP2PSocket->connectToHost(m_targetIp, m_targetTcpPort);
                m_tcpP2PSocket->waitForConnected(3000);
            }
            m_tcpP2PSocket->write(fragData.c_str(), fragData.size());
            m_tcpP2PSocket->flush();
        } else {
            m_udpP2PSocket->writeDatagram(fragData.c_str(), fragData.size(), m_targetIp, m_targetUdpPort);
        }
        
        // 更新进度
        m_sentFileSize += fragmentData.size();
        int progress = static_cast<int>((m_sentFileSize * 100) / m_totalFileSize);
        QMetaObject::invokeMethod(this, "updateTransferProgress",
            Qt::QueuedConnection,
            Q_ARG(int, progress));
        
        offset += fragmentSize;
        std::this_thread::sleep_for(std::chrono::milliseconds(10)); // 避免发送过快
    }
    
    // 发送完成
    QMetaObject::invokeMethod(this, "showMessage",
        Qt::QueuedConnection,
        Q_ARG(QString, m_nickname),
        Q_ARG(QString, QString("文件 %1 发送完成（MD5：%2）")
              .arg(QString::fromStdString(metaMsg.fileName))
              .arg(QString::fromStdString(metaMsg.fileMd5))));
    
    m_transferFile->close();
}

// 更新传输进度
void ChatWindow::updateTransferProgress(int progress) {
    ui->progressBar_transfer->setValue(progress);
}

// 接收文件
void ChatWindow::receiveFile(const FileMsg& metaMsg) {
    QString savePath = QString("./recv_files/%1").arg(QString::fromStdString(metaMsg.fileName));
    QDir dir;
    if (!dir.exists("./recv_files")) {
        dir.mkdir("./recv_files");
    }
    
    QFile file(savePath);
    if (!file.open(QIODevice::WriteOnly)) {
        QMetaObject::invokeMethod(this, "showMessage",
            Qt::QueuedConnection,
            Q_ARG(QString, "系统提示"),
            Q_ARG(QString, "打开保存文件失败：" + file.errorString()));
        return;
    }
    
    // 接收分片
    QMap<uint32_t, QByteArray> fragMap; // 分片缓存（解决乱序）
    // 修复：使用QTcpServer接收TCP连接
    if (metaMsg.protocol == TransportProtocol::TCP) {
        // 检查P2P服务端是否正常监听
        if (!m_tcpP2PServer->isListening()) {
            QMetaObject::invokeMethod(this, "showMessage",
                Qt::QueuedConnection,
                Q_ARG(QString, "系统提示"),
                Q_ARG(QString, "P2P TCP服务端未启动，无法接收文件分片"));
            file.close();
            return;
        }

        QEventLoop loop;
        connect(m_tcpP2PServer, &QTcpServer::newConnection, &loop, &QEventLoop::quit);
        
        while (fragMap.size() < metaMsg.totalFragments) {
            loop.exec(QEventLoop::ExcludeUserInputEvents);
            QTcpSocket* clientSocket = m_tcpP2PServer->nextPendingConnection();
            if (!clientSocket) break; // 防止空指针

            QByteArray fragData = clientSocket->readAll();
            FileMsg fragMsg = deserializeFileMsg(fragData.toStdString());
            fragMap[fragMsg.fragmentIdx] = QByteArray::fromBase64(QString::fromStdString(fragMsg.fileData).toUtf8());
            clientSocket->close();
            clientSocket->deleteLater();
            
            int progress = static_cast<int>((fragMap.size() * 100) / metaMsg.totalFragments);
            QMetaObject::invokeMethod(this, "updateTransferProgress",
                Qt::QueuedConnection,
                Q_ARG(int, progress));
        }
    } else {
        QUdpSocket udpSocket;
        udpSocket.bind(QHostAddress::Any, m_targetUdpPort);
        while (fragMap.size() < metaMsg.totalFragments) {
            while (udpSocket.hasPendingDatagrams()) {
                QByteArray fragData;
                fragData.resize(udpSocket.pendingDatagramSize());
                udpSocket.readDatagram(fragData.data(), fragData.size());
                FileMsg fragMsg = deserializeFileMsg(fragData.toStdString());
                fragMap[fragMsg.fragmentIdx] = QByteArray::fromBase64(QString::fromStdString(fragMsg.fileData).toUtf8());
            }
            int progress = static_cast<int>((fragMap.size() * 100) / metaMsg.totalFragments);
            QMetaObject::invokeMethod(this, "updateTransferProgress",
                Qt::QueuedConnection,
                Q_ARG(int, progress));
        }
    }
    
    // 合并分片
    for (uint32_t idx = 1; idx <= metaMsg.totalFragments; idx++) {
        file.write(fragMap[idx]);
    }
    file.close();
    
    // 校验MD5
    QFile checkFile(savePath);
    checkFile.open(QIODevice::ReadOnly);
    QCryptographicHash hash(QCryptographicHash::Md5);
    hash.addData(&checkFile);
    QString checkMd5 = hash.result().toHex();
    checkFile.close();
    
    if (checkMd5 == QString::fromStdString(metaMsg.fileMd5)) {
        QMetaObject::invokeMethod(this, "showMessage",
            Qt::QueuedConnection,
            Q_ARG(QString, "系统提示"),
            Q_ARG(QString, QString("文件 %1 接收完成，已保存至：%2（MD5校验通过）")
                  .arg(QString::fromStdString(metaMsg.fileName))
                  .arg(savePath)));
    } else {
        QMetaObject::invokeMethod(this, "showMessage",
            Qt::QueuedConnection,
            Q_ARG(QString, "系统提示"),
            Q_ARG(QString, QString("文件 %1 接收完成，但MD5校验失败！")
                  .arg(QString::fromStdString(metaMsg.fileName))));
    }
    
    // 重置进度条
    QMetaObject::invokeMethod(this, "updateTransferProgress",
        Qt::QueuedConnection,
        Q_ARG(int, 0));
}

// 处理点对点新连接的槽函数
void ChatWindow::onP2PNewConnection() {
    // 关键：在主线程中获取新连接的QTcpSocket，指定父对象为this（ChatWindow，主线程对象）
    // 避免指定父对象为m_tcpP2PServer（虽然也在主线程，但this更稳妥）
    m_tcpP2PSocket = m_tcpP2PServer->nextPendingConnection();
    m_tcpP2PSocket->setParent(this); // 明确指定父对象为主线程的ChatWindow

    // 绑定套接字的信号（数据接收、断开连接）
    connect(m_tcpP2PSocket, &QTcpSocket::readyRead, this, &ChatWindow::onP2PSocketReadyRead);
    connect(m_tcpP2PSocket, &QTcpSocket::disconnected, this, &ChatWindow::onP2PSocketDisconnected);
    connect(m_tcpP2PSocket, &QTcpSocket::errorOccurred, this, &ChatWindow::onP2PSocketError);

    showMessage("系统提示", QString("P2P 新连接建立：%1:%2").arg(m_tcpP2PSocket->peerAddress().toString()).arg(m_tcpP2PSocket->peerPort()));
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
    while (m_udpP2PSocket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(m_udpP2PSocket->pendingDatagramSize());
        QHostAddress senderAddr;
        quint16 senderPort;
        // 读取UDP数据
        m_udpP2PSocket->readDatagram(datagram.data(), datagram.size(), &senderAddr, &senderPort);
        
        showMessage("系统提示", QString("收到UDP数据：来自%1:%2，长度%3字节")
                    .arg(senderAddr.toString())
                    .arg(senderPort)
                    .arg(datagram.size()));
        
        // 判定是否为图片数据
        if (datagram.size() > 0) {
            // 保存UDP图片数据
            QString savePath = QString("./recv_imgs/udp_%1.jpg").arg(QTime::currentTime().toString("hhmmss"));
            QFile file(savePath);
            if (file.open(QIODevice::WriteOnly)) {
                file.write(datagram);
                file.close();
                showMessage("系统提示", QString("UDP图片已保存至：%1").arg(savePath));
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

void ChatWindow::onP2PSocketError(QAbstractSocket::SocketError socketError) {
    QTcpSocket* socket = qobject_cast<QTcpSocket*>(sender());
    if (socket) {
        showMessage("P2P 错误", QString("P2P套接字错误：%1").arg(socket->errorString()));
    }
}

void ChatWindow::onP2PSocketReadyRead() {
    QTcpSocket* socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket) return;

    // 读取P2P接收的图片数据（后续可完善保存逻辑）
    QByteArray imgData = socket->readAll();
    showMessage("系统提示", QString("收到P2P数据：%1字节").arg(imgData.size()));
}

void ChatWindow::onP2PSocketDisconnected() {
    QTcpSocket* socket = qobject_cast<QTcpSocket*>(sender());
    if (socket) {
        showMessage("系统提示", "P2P 连接已断开");
        socket->deleteLater(); // 安全删除套接字
        m_tcpP2PSocket = nullptr;
    }
}