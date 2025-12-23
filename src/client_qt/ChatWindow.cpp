#include "ChatWindow.h"
#include <QDateTime>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDebug>
#include <QAbstractSocket>
#include "protocol_qt.h"

// 构造函数（完整初始化+信号绑定）
ChatWindow::ChatWindow(int userId, const QString &nickname, QTcpSocket *serverSocket, QUdpSocket *udpSocket, QWidget *parent)
    : QWidget(parent),
      m_userId(userId),
      m_nickname(nickname),
      m_serverSocket(serverSocket),
      m_udpSocket(udpSocket),
      m_recvBuffer() // 初始化接收缓存（避免野指针）
{
    // 新增：打印包头大小（必须和服务端一致，都是 8 字节！）
    qDebug() << "[调试] 客户端 PacketHeader 大小：" << sizeof(PacketHeader);
    
    // 关键1：接管Socket生命周期（LoginWindow销毁时不会删除Socket）
    m_serverSocket->setParent(this);
    m_udpSocket->setParent(this);

    // 窗口基础配置
    setWindowTitle(QString("聊天窗口 - %1（ID：%2）").arg(nickname).arg(userId));
    setMinimumSize(600, 400); // 最小尺寸（避免窗口过小导致控件遮挡）
    resize(800, 500);         // 默认尺寸

    // 初始化UI控件
    m_chatList = new QListWidget(this);
    m_chatList->setStyleSheet("QListWidget { border: 1px solid #eee; padding: 5px; }"
                              "QListWidget::item { padding: 4px; }");

    // 在线用户列表相关控件
    QLabel *userListTitle = new QLabel("📋 在线用户", this);
    userListTitle->setStyleSheet("font-weight: bold; text-align: center; font-size: 14px;");
    userListTitle->setAlignment(Qt::AlignCenter);

    m_userList = new QListWidget(this);
    m_userList->setFixedWidth(200); // 固定宽度（避免被布局挤压）
    m_userList->addItem("📢 广播 (ID: 0)"); // 默认添加广播选项
    m_userList->setCurrentRow(0); // 默认选中广播
    m_selectedUserId = 0;         // 初始目标ID=广播

    // 消息输入区控件
    m_inputEdit = new QTextEdit(this);
    m_inputEdit->setPlaceholderText("输入消息（回车发送，Ctrl+Enter换行）");
    m_inputEdit->setMaximumHeight(80); // 限制输入框最大高度
    m_inputEdit->setStyleSheet("QTextEdit { border: 1px solid #eee; padding: 5px; }");

    m_sendBtn = new QPushButton("发送", this);
    m_sendBtn->setEnabled(false); // 初始禁用（无输入时）
    m_sendBtn->setStyleSheet("QPushButton { padding: 8px 16px; background-color: #4A90E2; color: white; border: none; border-radius: 4px; }"
                             "QPushButton:disabled { background-color: #ccc; }");

    // 心跳定时器初始化（3秒一次，与服务端超时时间匹配）
    m_heartbeatTimer = new QTimer(this);
    m_heartbeatTimer->setInterval(3000);
    connect(m_heartbeatTimer, &QTimer::timeout, this, &ChatWindow::sendHeartbeat);
    m_heartbeatTimer->start();

    // 布局设计（左侧：用户列表；右侧：聊天区域）
    QHBoxLayout *mainLayout = new QHBoxLayout(this);
    mainLayout->setSpacing(15);
    mainLayout->setContentsMargins(10, 10, 10, 10);

    // 左侧布局：用户列表标题 + 用户列表
    QVBoxLayout *leftLayout = new QVBoxLayout;
    leftLayout->addWidget(userListTitle);
    leftLayout->addWidget(m_userList);
    leftLayout->setSpacing(8);
    mainLayout->addLayout(leftLayout, 1); // 权重1（占1/4宽度）

    // 右侧布局：聊天记录 + 输入区
    QVBoxLayout *rightLayout = new QVBoxLayout;
    QHBoxLayout *inputLayout = new QHBoxLayout;

    inputLayout->addWidget(m_inputEdit);
    inputLayout->addWidget(m_sendBtn);
    inputLayout->setSpacing(8);

    rightLayout->addWidget(m_chatList);
    rightLayout->addLayout(inputLayout);
    rightLayout->setSpacing(8);
    rightLayout->setStretch(0, 1); // 聊天记录占满剩余空间
    mainLayout->addLayout(rightLayout, 3); // 权重3（占3/4宽度）

    // 关键2：信号绑定（修复笔误：sendBtn→m_sendBtn）
    connect(m_sendBtn, &QPushButton::clicked, this, &ChatWindow::onSendClicked);
    connect(m_serverSocket, &QTcpSocket::readyRead, this, &ChatWindow::onServerReadyRead);
    connect(m_udpSocket, &QUdpSocket::readyRead, this, &ChatWindow::onUdpReadyRead);
    connect(m_userList, &QListWidget::itemClicked, this, &ChatWindow::onUserSelected);
    connect(m_inputEdit, &QTextEdit::textChanged, this, &ChatWindow::onTextEdited);

    // 连接状态监听（断开/错误提示）
    connect(m_serverSocket, &QTcpSocket::disconnected, this, []() {
        QMessageBox::warning(nullptr, "连接提示", "与服务端的连接已断开！");
    });
    connect(m_serverSocket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        showMessage("系统", "连接错误：" + m_serverSocket->errorString());
    });

    // 关键3：登录后主动请求在线用户列表（检查连接状态）
    if (m_serverSocket->state() == QAbstractSocket::ConnectedState) {
        PacketHeader header;
        header.msgType = htonl(USER_LIST_REQ);
        header.dataLen = htonl(0);
        std::vector<char> sendData;
        sendData.insert(sendData.end(), reinterpret_cast<char*>(&header),
                        reinterpret_cast<char*>(&header) + sizeof(PacketHeader));

        qint64 bytesSent = m_serverSocket->write(sendData.data(), sendData.size());
        if (bytesSent == -1) {
            showMessage("系统", "发送用户列表请求失败：" + m_serverSocket->errorString());
        } else {
            showMessage("系统", "正在获取在线用户列表...");
            qDebug() << "[初始化] 已发送USER_LIST_REQ，字节数：" << bytesSent;

            // 新增：5秒超时未收到响应提示
            QTimer::singleShot(5000, this, [this]() {
                if (m_onlineUsers.empty()) {
                    showMessage("系统", "获取在线用户列表超时，请检查网络连接！");
                }
            });
        }
    }
}

// 析构函数（QT自动管理子控件，无需额外释放）
ChatWindow::~ChatWindow() {}

// 发送按钮点击事件（调用sendMessage发送消息）
void ChatWindow::onSendClicked() {
    QString content = m_inputEdit->toPlainText().trimmed();
    if (!content.isEmpty()) {
        sendMessage(content);
        m_inputEdit->clear(); // 发送后清空输入框
    }
}

// 键盘事件（回车发送，Ctrl+Enter换行）
void ChatWindow::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (!(event->modifiers() & Qt::ControlModifier)) {
            // 无Ctrl修饰 → 发送消息
            QString content = m_inputEdit->toPlainText().trimmed();
            if (!content.isEmpty()) {
                sendMessage(content);
                m_inputEdit->clear();
            }
            event->accept(); // 拦截事件，避免默认换行
        } else {
            // 有Ctrl修饰 → 插入换行
            m_inputEdit->insertPlainText("\n");
            event->accept();
        }
    } else {
        QWidget::keyPressEvent(event); // 其他按键交给父类处理
    }
}

// 输入框文本变化（控制发送按钮启用/禁用）
void ChatWindow::onTextEdited() {
    bool isEnabled = !m_inputEdit->toPlainText().trimmed().isEmpty();
    m_sendBtn->setEnabled(isEnabled);
}

// 选择在线用户（更新目标用户ID）
void ChatWindow::onUserSelected(QListWidgetItem *item) {
    if (!item) return;

    QString itemText = item->text();
    // 解析用户ID（格式："📢 广播 (ID: 0)" 或 "👤 昵称 (ID: 1)"）
    int idStart = itemText.indexOf("ID: ") + 4;
    int idEnd = itemText.indexOf(")", idStart);
    if (idStart >= 4 && idEnd != -1) {
        m_selectedUserId = itemText.mid(idStart, idEnd - idStart).toInt();
        qDebug() << "[用户选择] 目标ID更新为：" << m_selectedUserId;
    } else {
        m_selectedUserId = 0; // 解析失败默认广播
        qDebug() << "[用户选择] 解析ID失败，默认广播";
    }
}

// 发送消息核心逻辑（与服务端协议一致）
void ChatWindow::sendMessage(const QString &content) {
    if (m_serverSocket->state() != QAbstractSocket::ConnectedState) {
        QMessageBox::warning(this, "发送失败", "与服务端的连接已断开！");
        return;
    }
    if (m_selectedUserId == -1) {
        QMessageBox::warning(this, "发送失败", "未选择接收者！");
        return;
    }

    try {
        CommonMsg msg;
        msg.fromUserId = m_userId;
        msg.toUserId = m_selectedUserId;
        msg.fromNickname = m_nickname.toStdString();
        msg.content = content.toStdString();

        std::vector<char> payload = serializeCommonMsg(msg);
        PacketHeader header;
        header.msgType = htonl(COMMON_MSG);
        header.dataLen = htonl(static_cast<uint32_t>(payload.size()));

        std::vector<char> sendData;
        sendData.insert(sendData.end(), reinterpret_cast<char*>(&header),
                        reinterpret_cast<char*>(&header) + sizeof(PacketHeader));
        sendData.insert(sendData.end(), payload.begin(), payload.end());

        // 👇 彻底删除这行导致编译错误的代码（waitForBytesWritten已自带超时）
        // m_serverSocket->setSocketOption(QAbstractSocket::SendTimeoutOption, QVariant(5000));

        // 确保数据完整发送（原有逻辑不变，waitForBytesWritten已控制超时）
        qint64 totalBytes = sendData.size();
        qint64 bytesSent = 0;
        while (bytesSent < totalBytes) {
            qint64 sent = m_serverSocket->write(sendData.data() + bytesSent, totalBytes - bytesSent);
            if (sent == -1) {
                throw std::runtime_error("Socket写入失败：" + m_serverSocket->errorString().toStdString());
            }
            bytesSent += sent;

            // 等待数据写入内核缓冲区（5秒超时，已覆盖发送超时需求）
            if (!m_serverSocket->waitForBytesWritten(5000)) {
                throw std::runtime_error("发送超时：" + m_serverSocket->errorString().toStdString());
            }
        }

        // 本地显示消息（原有逻辑不变）
        QString sender = "我";
        if (m_selectedUserId == 0) sender += "（广播）";
        else {
            auto it = m_onlineUsers.find(m_selectedUserId);
            sender += it != m_onlineUsers.end() 
                      ? QString("（发给%1）").arg(QString::fromStdString(it->second.nickname))
                      : QString("（发给ID:%1）").arg(m_selectedUserId);
        }
        showMessage(sender, content);
        qDebug() << "[消息发送] 成功：总字节数=" << sendData.size();

    } catch (const std::exception& e) {
        QString errMsg = "发送消息失败：" + QString::fromStdString(e.what());
        showMessage("系统", errMsg);
        qDebug() << "[消息发送] 失败：" << errMsg;
    }
}

// 更新在线用户列表UI（确保显示正常，无遮挡）
void ChatWindow::updateOnlineUsers(const std::map<int, UserInfo>& onlineUsers) {
    qDebug() << "[用户列表更新] 收到在线用户数：" << onlineUsers.size();

    m_userList->clear();
    // 重新添加广播选项
    QListWidgetItem* broadcastItem = new QListWidgetItem("📢 广播 (ID: 0)");
    broadcastItem->setData(Qt::UserRole, 0);
    m_userList->addItem(broadcastItem);

    // 添加所有在线用户（跳过自己）
    for (const auto& [userId, user] : onlineUsers) {
        if (userId == m_userId) {
            qDebug() << "[用户列表更新] 跳过自己：ID=" << userId << "，昵称=" << QString::fromStdString(user.nickname);
            continue;
        }

        QString nickname = QString::fromStdString(user.nickname);
        if (nickname.isEmpty() || nickname.trimmed().isEmpty()) {
            nickname = QString("用户%1").arg(userId);
        }
        QString itemText = QString("👤 %1 (ID: %2)").arg(nickname).arg(userId);

        QListWidgetItem* userItem = new QListWidgetItem(itemText);
        userItem->setData(Qt::UserRole, userId);
        m_userList->addItem(userItem);

        qDebug() << "[用户列表更新] 添加用户：" << itemText;
    }

    // 恢复选中状态（默认选中广播）
    m_userList->setCurrentRow(0);
    m_selectedUserId = 0;

    // 强制UI刷新（解决Qt布局延迟）
    m_userList->update();
    m_userList->repaint();
    qDebug() << "[用户列表更新] UI刷新完成，总条目数：" << m_userList->count();
}

// 接收服务端TCP消息（核心数据解析逻辑）
void ChatWindow::onServerReadyRead() {
    // 检查Socket状态
    if (m_serverSocket->state() != QAbstractSocket::ConnectedState) {
        qDebug() << "[TCP接收] 错误：Socket未连接，状态：" << m_serverSocket->state();
        return;
    }
    if (m_serverSocket->error() != QAbstractSocket::UnknownSocketError) {
        QString errMsg = "Socket异常：" + m_serverSocket->errorString();
        qDebug() << "[TCP接收] 错误：" << errMsg;
        showMessage("系统", errMsg);
        return;
    }

    // 读取数据到缓存
    QByteArray newData = m_serverSocket->readAll();
    if (newData.isEmpty()) {
        qDebug() << "[TCP接收] 收到空包（正常连接保持）";
        return;
    }

    qDebug() << "[TCP接收] 字节数：" << newData.size()
             << "，十六进制：" << newData.toHex()
             << "，缓存总长度：" << m_recvBuffer.size() + newData.size();
    m_recvBuffer.append(newData);

    // 循环解析完整数据包（处理粘包/半包）
    while (m_recvBuffer.size() >= sizeof(PacketHeader)) {
        // 解析包头（转主机字节序）
        PacketHeader header;
        memcpy(&header, m_recvBuffer.data(), sizeof(PacketHeader));
        header.msgType = ntohl(header.msgType);
        header.dataLen = ntohl(header.dataLen);

        qDebug() << "[包头解析] 消息类型：" << header.msgType
                 << "，数据长度：" << header.dataLen
                 << "，包头长度：" << sizeof(PacketHeader);

        // 检查数据包完整性
        uint32_t totalPacketLen = sizeof(PacketHeader) + header.dataLen;
        if (m_recvBuffer.size() < totalPacketLen) {
            qDebug() << "[数据包不完整] 缓存长度：" << m_recvBuffer.size()
                     << "，需要长度：" << totalPacketLen << "，等待后续数据";
            break;
        }

        // 新增：检查消息类型是否合法
        if (header.msgType < LOGIN_REQ || header.msgType > HEARTBEAT) {
            qDebug() << "[无效消息] 收到非法msgType：" << header.msgType << "，丢弃数据包";
            m_recvBuffer.remove(0, totalPacketLen);
            continue;
        }

        // 提取payload
        std::vector<char> payload(m_recvBuffer.begin() + sizeof(PacketHeader),
                                  m_recvBuffer.begin() + totalPacketLen);
        // 移除已处理数据
        m_recvBuffer.remove(0, totalPacketLen);
        qDebug() << "[payload提取] 长度：" << payload.size() << "，剩余缓存：" << m_recvBuffer.size();

        // 处理不同类型消息
        switch (header.msgType) {
            case USER_LIST_RSP: {
                // 在线用户列表响应
                try {
                    std::map<int, UserInfo> users = deserializeUserList(payload);
                    qDebug() << "[USER_LIST_RSP] 解析成功，在线用户数：" << users.size();
                    for (const auto& [id, info] : users) {
                        qDebug() << "  - ID:" << id << "，昵称:" << QString::fromStdString(info.nickname) << "，IP:" << QString::fromStdString(info.ip);
                    }
                    m_onlineUsers = users;
                    updateOnlineUsers(users);
                    showMessage("系统", "在线用户列表加载完成（共" + QString::number(users.size()) + "人）");
                } catch (const std::exception& e) {
                    QString errMsg = "解析用户列表失败：" + QString::fromStdString(e.what());
                    showMessage("系统", errMsg);
                    qDebug() << "[USER_LIST_RSP] 失败：" << errMsg;
                }
                break;
            }
            case USER_ONLINE_NOTIFY: {
                // 新用户上线通知
                try {
                    UserInfo newUser = deserializeUserInfo(payload);
                    qDebug() << "[USER_ONLINE_NOTIFY] 新用户上线：ID=" << newUser.userId << "，昵称=" << QString::fromStdString(newUser.nickname);
                    m_onlineUsers[newUser.userId] = newUser;
                    updateOnlineUsers(m_onlineUsers);
                    showMessage("系统", QString::fromStdString(newUser.nickname) + " 已上线");
                } catch (const std::exception& e) {
                    QString errMsg = "解析上线通知失败：" + QString::fromStdString(e.what());
                    showMessage("系统", errMsg);
                    qDebug() << "[USER_ONLINE_NOTIFY] 失败：" << errMsg;
                }
                break;
            }
            case USER_OFFLINE_NOTIFY: {
                // 用户下线通知
                try {
                    UserInfo offlineUser = deserializeUserInfo(payload);
                    qDebug() << "[USER_OFFLINE_NOTIFY] 用户下线：ID=" << offlineUser.userId << "，昵称=" << QString::fromStdString(offlineUser.nickname);
                    m_onlineUsers.erase(offlineUser.userId);
                    updateOnlineUsers(m_onlineUsers);
                    showMessage("系统", QString::fromStdString(offlineUser.nickname) + " 已下线");
                } catch (const std::exception& e) {
                    QString errMsg = "解析下线通知失败：" + QString::fromStdString(e.what());
                    showMessage("系统", errMsg);
                    qDebug() << "[USER_OFFLINE_NOTIFY] 失败：" << errMsg;
                }
                break;
            }
            case COMMON_MSG: {
                // 普通聊天消息（点对点/广播）
                try {
                    CommonMsg chatMsg = deserializeCommonMsg(payload);
                    qDebug() << "[COMMON_MSG] 解析成功：发送者ID=" << chatMsg.fromUserId
                             << "，昵称=" << QString::fromStdString(chatMsg.fromNickname)
                             << "，目标ID=" << chatMsg.toUserId
                             << "，内容=" << QString::fromStdString(chatMsg.content);

                    QString senderName = QString::fromStdString(chatMsg.fromNickname);
                    if (chatMsg.toUserId == 0) {
                        senderName += "（广播）";
                    }
                    showMessage(senderName, QString::fromStdString(chatMsg.content));
                } catch (const std::exception& e) {
                    QString errMsg = "解析聊天消息失败：" + QString::fromStdString(e.what());
                    showMessage("系统", errMsg);
                    qDebug() << "[COMMON_MSG] 失败：" << errMsg;
                }
                break;
            }
            case HEARTBEAT: {
                // 服务端心跳响应（可选：无需显示，仅日志）
                qDebug() << "[HEARTBEAT] 收到服务端心跳回应，连接正常";
                break;
            }
            default: {
                qDebug() << "[未知消息] 收到未定义消息类型：" << header.msgType;
                showMessage("系统", "收到未知类型消息（可能是版本不兼容）");
                break;
            }
        }
    }
}

// 显示聊天消息（带时间戳，自动滚动到底部）
void ChatWindow::showMessage(const QString &sender, const QString &content) {
    qDebug() << "[消息显示] 发送者：" << sender << "，内容：" << content;

    // 格式化消息（时间戳+发送者+内容）
    QString timeStr = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    QString msgStr = QString("[%1] %2：%3").arg(timeStr).arg(sender).arg(content);
    m_chatList->addItem(msgStr);
    m_chatList->scrollToBottom(); // 自动滚动到最新消息
}

// 发送心跳包（维持与服务端的连接）
void ChatWindow::sendHeartbeat() {
    if (m_serverSocket->state() != QAbstractSocket::ConnectedState) {
        qDebug() << "[心跳] 连接已断开，跳过发送";
        return;
    }

    PacketHeader header;
    header.msgType = htonl(HEARTBEAT);
    header.dataLen = htonl(0);
    std::vector<char> sendData;
    sendData.insert(sendData.end(), reinterpret_cast<char*>(&header),
                    reinterpret_cast<char*>(&header) + sizeof(PacketHeader));

    qint64 bytesSent = m_serverSocket->write(sendData.data(), sendData.size());
    if (bytesSent == -1) {
        qDebug() << "[心跳] 发送失败：" << m_serverSocket->errorString();
    } else {
        qDebug() << "[心跳] 发送成功，字节数：" << bytesSent;
    }
}

// UDP消息接收（预留扩展，当前空实现）
void ChatWindow::onUdpReadyRead() {
    qDebug() << "[UDP接收] 收到UDP数据（当前未处理，预留扩展）";
    // 后续扩展UDP点对点通信时，添加解析逻辑
}