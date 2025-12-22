#include "ChatWindow.h"
#include <QDateTime>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include "protocol_qt.h"

// 构造函数（完整实现，修复信号绑定）
ChatWindow::ChatWindow(int userId, const QString &nickname, QTcpSocket *serverSocket, QUdpSocket *udpSocket, QWidget *parent)
    : QWidget(parent), 
      m_userId(userId), 
      m_nickname(nickname),
      m_serverSocket(serverSocket), 
      m_udpSocket(udpSocket),
      m_recvBuffer() // 初始化TCP接收缓存（解决粘包/半包导致的“无效信息”）
{
    // 关键修复1：接管Socket生命周期，避免LoginWindow销毁时被删除（核心解决“连接断开”）
    m_serverSocket->setParent(this);
    m_udpSocket->setParent(this);

    // 窗口配置
    setWindowTitle(QString("聊天窗口 - %1（ID：%2）").arg(nickname).arg(userId));
    setFixedSize(800, 500);

    // 初始化控件（新增用户列表+标题标签）
    m_chatList = new QListWidget;
    
    // 新增：用户列表标题标签（替代 placeholder）
    QLabel *userListTitle = new QLabel("在线用户");
    userListTitle->setStyleSheet("font-weight: bold; text-align: center;");
    userListTitle->setAlignment(Qt::AlignCenter);

    m_userList = new QListWidget; // 在线用户列表UI
    m_userList->setFixedWidth(150);
    m_userList->addItem("广播 (ID: 0)"); // 默认添加广播选项（优化体验）

    m_inputEdit = new QTextEdit;
    m_inputEdit->setPlaceholderText("输入消息后按Ctrl+Enter发送");
    m_inputEdit->setMaximumHeight(80);
    m_sendBtn = new QPushButton("发送");
    m_sendBtn->setEnabled(false); // 初始禁用（无输入时，避免无效点击）

    // 关键：初始化心跳定时器（3秒发送一次，避免服务端超时）
    m_heartbeatTimer = new QTimer(this);
    m_heartbeatTimer->setInterval(3000); // 3秒一次（需与服务端超时时间匹配）
    connect(m_heartbeatTimer, &QTimer::timeout, this, &ChatWindow::sendHeartbeat);
    m_heartbeatTimer->start(); // 启动定时器

    // 布局（左侧：标题+用户列表；右侧：聊天区域）
    QHBoxLayout *mainLayout = new QHBoxLayout(this);
    QVBoxLayout *userListLayout = new QVBoxLayout; // 新增：用户列表+标题的布局
    QVBoxLayout *chatLayout = new QVBoxLayout;
    QHBoxLayout *inputLayout = new QHBoxLayout;

    // 左侧布局：标题标签 + 用户列表
    userListLayout->addWidget(userListTitle);
    userListLayout->addWidget(m_userList);
    userListLayout->setContentsMargins(0, 0, 10, 0); // 右侧留间距

    // 输入区布局
    inputLayout->addWidget(m_inputEdit);
    inputLayout->addWidget(m_sendBtn);

    // 右侧布局：聊天记录 + 输入区
    chatLayout->addWidget(m_chatList);
    chatLayout->addLayout(inputLayout);

    // 主布局：左侧用户列表 + 右侧聊天区域
    mainLayout->addLayout(userListLayout); // 替换直接添加 m_userList
    mainLayout->addLayout(chatLayout);
    mainLayout->setStretch(0, 1);
    mainLayout->setStretch(1, 3);

    // 信号绑定（补充连接状态监听，修复“连接断开”无提示问题）
    connect(m_sendBtn, &QPushButton::clicked, this, &ChatWindow::sendMessage);
    connect(m_serverSocket, &QTcpSocket::readyRead, this, &ChatWindow::onServerReadyRead);
    connect(m_udpSocket, &QUdpSocket::readyRead, this, &ChatWindow::onUdpReadyRead);
    connect(m_userList, &QListWidget::itemClicked, this, &ChatWindow::onUserSelected);
    connect(m_inputEdit, &QTextEdit::textChanged, this, [this]() {
        // 输入非空时启用发送按钮
        m_sendBtn->setEnabled(!m_inputEdit->toPlainText().trimmed().isEmpty());
    });

    // 关键修复2：监听连接断开/错误，及时提示用户
    connect(m_serverSocket, &QTcpSocket::disconnected, this, []() {
        QMessageBox::warning(nullptr, "连接提示", "与服务端的连接已断开！");
    });
    connect(m_serverSocket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        showMessage("系统", "连接错误：" + m_serverSocket->errorString());
    });

    // 关键修复3：发送用户列表请求前检查连接状态（避免无效请求）
    // 在ChatWindow构造函数中发送USER_LIST_REQ的位置补充日志
    if (m_serverSocket->state() == QAbstractSocket::ConnectedState) {
    PacketHeader header;
    header.msgType = USER_LIST_REQ;
    header.dataLen = 0;
    std::vector<char> sendData;
    sendData.insert(sendData.end(), reinterpret_cast<char*>(&header), 
                    reinterpret_cast<char*>(&header) + sizeof(PacketHeader));
    // 发送并打印日志
    qint64 bytesSent = m_serverSocket->write(sendData.data(), sendData.size());
    if (bytesSent == -1) {
        showMessage("系统", "发送用户列表请求失败：" + m_serverSocket->errorString());
    } else {
        showMessage("系统", "正在获取在线用户列表...");
        qDebug() << "已发送USER_LIST_REQ，字节数：" << bytesSent;
    }
}
}

// 析构函数实现（匹配头文件声明）
ChatWindow::~ChatWindow() {
    // 可选：释放资源（QT父对象会自动管理，可留空）
}

// 发送消息核心函数（已有实现，保持不变）
void ChatWindow::sendMessage(const QString &content) {
    if (content.isEmpty() || m_selectedUserId == -1) {
        QMessageBox::warning(this, "警告", "消息不能为空或未选择接收者！");
        return;
    }

    try {
        CommonMsg msg;
        msg.fromUserId = m_userId;                  // 本地用户ID（主机字节序）
        msg.toUserId = m_selectedUserId;            // 目标用户ID（主机字节序）
        msg.fromNickname = m_nickname.toStdString();// 昵称
        msg.content = content.toStdString();        // 消息内容

        // 序列化消息体（确保调用修复后的 serializeCommonMsg）
        std::vector<char> payload = serializeCommonMsg(msg);

        // 构造包头（关键：msgType 和 dataLen 转网络字节序）
        PacketHeader header;
        header.msgType = htonl(COMMON_MSG);         // 消息类型→网络字节序
        header.dataLen = htonl(static_cast<uint32_t>(payload.size())); // 长度→网络字节序

        // 构造完整数据包
        std::vector<char> sendData;
        sendData.insert(sendData.end(), reinterpret_cast<char*>(&header),
                        reinterpret_cast<char*>(&header) + sizeof(PacketHeader));
        sendData.insert(sendData.end(), payload.begin(), payload.end());

        // 发送数据
        qint64 sent = m_serverSocket->write(sendData.data(), sendData.size());
        if (sent == -1) {
            throw std::runtime_error("发送消息失败：" + m_serverSocket->errorString().toStdString());
        }

        // 本地显示自己的消息
        QString sender = "我";
        if (m_selectedUserId == 0) sender += "（广播）";
        else sender += QString("（发给ID:%1）").arg(m_selectedUserId);
        showMessage(sender, content);

        qDebug() << "[sendMessage] 发送成功：内容=" << content
                 << "，目标ID=" << m_selectedUserId
                 << "，总字节数=" << sendData.size();
    } catch (const std::exception& e) {
        QString err = "发送消息失败：" + QString::fromStdString(e.what());
        showMessage("系统", err);
        qDebug() << err;
    }
}

void ChatWindow::onSendClicked() {
    sendMessage();
}

// 新增：文本编辑框变化槽函数（实现）
void ChatWindow::onTextEdited() {
    // 可选：处理文本变化逻辑（如按钮状态控制）
    m_sendBtn->setEnabled(!m_inputEdit->toPlainText().trimmed().isEmpty());
}

// 重写键盘事件（捕捉回车发送，修复QTextEdit无returnPressed）
void ChatWindow::keyPressEvent(QKeyEvent *event) {
    // 按下Ctrl+Enter或Enter发送（避免单行回车换行）
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && 
        (event->modifiers() & Qt::ControlModifier)) {
        sendMessage();
    } else if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        // 普通Enter换行（可选，根据需求调整）
        m_inputEdit->insertPlainText("\n");
    } else {
        QWidget::keyPressEvent(event); // 其他按键默认处理
    }
}

// 更新在线用户列表
void ChatWindow::updateOnlineUsers(const std::map<int, UserInfo>& onlineUsers) {
    qDebug() << "[updateOnlineUsers] 开始更新UI，当前在线用户数=" << onlineUsers.size();

    // ==================================
    // 第一步：清空列表+基础设置（避免重复/隐藏）
    // ==================================
    m_userList->clear();
    // 强制设置列表属性（防止被布局挤压/隐藏）
    m_userList->setFixedWidth(220); // 固定宽度，确保能看到
    m_userList->setMinimumHeight(300); // 最小高度
    m_userList->setVisible(true); // 强制显示
    m_userList->setSelectionMode(QAbstractItemView::SingleSelection); // 单选模式
    m_userList->setStyleSheet("QListWidget { border: 1px solid #ccc; border-radius: 4px; padding: 5px; }"
                              "QListWidget::item { padding: 8px; margin: 2px; border-radius: 2px; }"
                              "QListWidget::item:selected { background-color: #4A90E2; color: white; }");

    // ==================================
    // 第二步：添加「广播」选项（默认选中）
    // ==================================
    QListWidgetItem* broadcastItem = new QListWidgetItem("📢 广播 (ID: 0)");
    broadcastItem->setData(Qt::UserRole, 0); // 存储目标用户ID（0=广播）
    m_userList->addItem(broadcastItem);

    // ==================================
    // 第三步：添加所有在线用户（跳过自己）
    // ==================================
    for (const auto& [userId, user] : onlineUsers) {
        // 跳过当前登录用户自己
        if (userId == m_userId) {
            qDebug() << "[updateOnlineUsers] 跳过自己：ID=" << userId << "，昵称=" << QString::fromStdString(user.nickname);
            continue;
        }

        // 构建用户条目（昵称+ID，避免昵称为空）
        QString nickname = QString::fromStdString(user.nickname);
        if (nickname.isEmpty() || nickname.trimmed().isEmpty()) {
            nickname = QString("用户%1").arg(userId);
        }
        QString itemText = QString("👤 %1 (ID: %2)").arg(nickname).arg(userId);

        // 添加到列表+存储用户ID（方便后续发送消息）
        QListWidgetItem* userItem = new QListWidgetItem(itemText);
        userItem->setData(Qt::UserRole, userId); // 关联用户ID
        m_userList->addItem(userItem);

        qDebug() << "[updateOnlineUsers] 添加用户：" << itemText;
    }

    // ==================================
    // 第四步：默认选中「广播」+ 更新选中状态
    // ==================================
    if (m_userList->count() > 0) {
        m_userList->setCurrentItem(m_userList->item(0));
        m_selectedUserId = 0; // 默认广播
        qDebug() << "[updateOnlineUsers] 默认选中：广播（ID=0）";
    } else {
        m_selectedUserId = -1;
        qDebug() << "[updateOnlineUsers] 无在线用户（除自己外）";
    }

    // ==================================
    // 第五步：强制UI刷新（解决Qt布局延迟问题）
    // ==================================
    m_userList->update();
    m_userList->repaint();
    qDebug() << "[updateOnlineUsers] UI更新完成，列表总条目数=" << m_userList->count();
}

// 选择在线用户（更新目标用户ID）
void ChatWindow::onUserSelected(QListWidgetItem *item) {
    if (!item) return;
    QString itemText = item->text();
    int idStart = itemText.indexOf("ID: ") + 4;
    int idEnd = itemText.indexOf(")", idStart);
    if (idStart >= 4 && idEnd != -1) {
        m_selectedUserId = itemText.mid(idStart, idEnd - idStart).toInt();
        qDebug() << "[选择用户] 目标用户ID：" << m_selectedUserId;
    } else {
        m_selectedUserId = 0; // 解析失败默认广播
    }
}

// 接收UDP点对点消息（已有实现，保持不变）
void ChatWindow::onServerReadyRead() {
    // ==================================
    // 第一步：检查Socket状态（排除连接问题）
    // ==================================
    if (m_serverSocket->state() != QAbstractSocket::ConnectedState) {
        qDebug() << "[onServerReadyRead] 错误：Socket未连接，状态码：" << m_serverSocket->state();
        return;
    }
    if (m_serverSocket->error() != QAbstractSocket::UnknownSocketError) {
        qDebug() << "[onServerReadyRead] 错误：Socket异常，错误信息：" << m_serverSocket->errorString();
        QMessageBox::warning(this, "网络错误", "与服务端连接异常：" + m_serverSocket->errorString());
        return;
    }

    // ==================================
    // 第二步：读取数据到缓存（处理粘包/半包）
    // ==================================
    QByteArray newData = m_serverSocket->readAll();
    if (newData.isEmpty()) {
        qDebug() << "[onServerReadyRead] 收到TCP空包（正常连接保持，跳过）";
        return;
    }

    // 打印接收详情（十六进制+长度，方便抓包核对）
    qDebug() << "[onServerReadyRead] 接收数据：字节数=" << newData.size()
             << "，十六进制=" << newData.toHex()
             << "，缓存当前总长度=" << m_recvBuffer.size() + newData.size();
    m_recvBuffer.append(newData);

    // ==================================
    // 第三步：循环解析完整数据包
    // ==================================
    while (m_recvBuffer.size() >= sizeof(PacketHeader)) {
    // 1. 读取原始包头（网络字节序）
    PacketHeader header;
    memcpy(&header, m_recvBuffer.data(), sizeof(PacketHeader));
    
    // 2. 网络字节序 → 主机字节序（核心修复！之前遗漏这步）
    header.msgType = ntohl(header.msgType);   // 消息类型转字节序
    header.dataLen = ntohl(header.dataLen);   // 数据长度转字节序
    
    // 打印转换后的包头（确认正确）
    qDebug() << "[解析包头] 转换后 msgType=" << header.msgType
             << "，dataLen=" << header.dataLen
             << "，包头长度=" << sizeof(PacketHeader);

    // 3. 检查数据包完整性（后续逻辑不变）
    uint32_t totalPacketLen = sizeof(PacketHeader) + header.dataLen;
    if (m_recvBuffer.size() < totalPacketLen) {
        qDebug() << "[数据不完整] 缓存长度=" << m_recvBuffer.size()
                 << "，需要长度=" << totalPacketLen
                 << "，等待后续数据...";
        break;
    }

        // 提取payload（消息体）
        std::vector<char> payload(
            m_recvBuffer.begin() + sizeof(PacketHeader),
            m_recvBuffer.begin() + totalPacketLen
        );
        qDebug() << "[提取Payload] 长度=" << payload.size() << "，开始处理消息类型=" << header.msgType;

        // 移除已处理的数据包（避免重复解析）
        m_recvBuffer.remove(0, totalPacketLen);
        qDebug() << "[处理完成] 剩余缓存长度=" << m_recvBuffer.size();

        // ==================================
        // 第四步：处理不同类型消息
        // ==================================
        switch (header.msgType) {
            // ------------------------------
            // 在线用户列表响应（登录后获取）
            // ------------------------------
            case USER_LIST_RSP: {
                try {
                    std::map<int, UserInfo> onlineUsers = deserializeUserList(payload);
                    qDebug() << "[USER_LIST_RSP] 解析成功，在线用户数=" << onlineUsers.size();
                    for (const auto& [userId, user] : onlineUsers) {
                        qDebug() << "  - 用户：ID=" << userId
                                 << "，昵称=" << QString::fromStdString(user.nickname)
                                 << "，IP=" << QString::fromStdString(user.ip);
                    }
                    // 更新本地用户列表+UI
                    m_onlineUsers = onlineUsers;
                    updateOnlineUsers(m_onlineUsers);
                    showMessage("系统", "在线用户列表加载完成（共" + QString::number(onlineUsers.size()) + "人）");
                } catch (const std::exception& e) {
                    QString errMsg = "解析用户列表失败：" + QString::fromStdString(e.what());
                    qDebug() << "[USER_LIST_RSP] 错误：" << errMsg;
                    showMessage("系统", errMsg);
                }
                break;
            }

            // ------------------------------
            // 用户上线通知（实时推送）
            // ------------------------------
            case USER_ONLINE_NOTIFY: {
                try {
                    UserInfo newUser = deserializeUserInfo(payload);
                    qDebug() << "[USER_ONLINE_NOTIFY] 新用户上线：ID=" << newUser.userId
                             << "，昵称=" << QString::fromStdString(newUser.nickname);
                    // 添加到本地列表+更新UI
                    m_onlineUsers[newUser.userId] = newUser;
                    updateOnlineUsers(m_onlineUsers);
                    showMessage("系统", QString::fromStdString(newUser.nickname) + " 已上线");
                } catch (const std::exception& e) {
                    QString errMsg = "解析上线通知失败：" + QString::fromStdString(e.what());
                    qDebug() << "[USER_ONLINE_NOTIFY] 错误：" << errMsg;
                    showMessage("系统", errMsg);
                }
                break;
            }

            // ------------------------------
            // 用户下线通知（实时推送）
            // ------------------------------
            case USER_OFFLINE_NOTIFY: {
                try {
                    UserInfo offlineUser = deserializeUserInfo(payload);
                    qDebug() << "[USER_OFFLINE_NOTIFY] 用户下线：ID=" << offlineUser.userId
                             << "，昵称=" << QString::fromStdString(offlineUser.nickname);
                    // 从本地列表移除+更新UI
                    m_onlineUsers.erase(offlineUser.userId);
                    updateOnlineUsers(m_onlineUsers);
                    showMessage("系统", QString::fromStdString(offlineUser.nickname) + " 已下线");
                } catch (const std::exception& e) {
                    QString errMsg = "解析下线通知失败：" + QString::fromStdString(e.what());
                    qDebug() << "[USER_OFFLINE_NOTIFY] 错误：" << errMsg;
                    showMessage("系统", errMsg);
                }
                break;
            }

            // ------------------------------
            // 普通消息（点对点/广播）
            // ------------------------------
            case COMMON_MSG: {
                try {
                    CommonMsg chatMsg = deserializeCommonMsg(payload);
                    qDebug() << "[COMMON_MSG] 解析成功："
                             << "fromUserId=" << chatMsg.fromUserId
                             << "，fromNickname=" << QString::fromStdString(chatMsg.fromNickname)
                             << "，toUserId=" << chatMsg.toUserId
                             << "，content=" << QString::fromStdString(chatMsg.content);

                    // 格式化发送者名称（广播标注）
                    QString senderName = QString::fromStdString(chatMsg.fromNickname);
                    if (chatMsg.toUserId == 0) {
                        senderName += "（广播）";
                    }

                    // 显示到聊天窗口（带时间戳）
                    showMessage(senderName, QString::fromStdString(chatMsg.content));
                } catch (const std::exception& e) {
                    QString errMsg = "解析聊天消息失败：" + QString::fromStdString(e.what());
                    qDebug() << "[COMMON_MSG] 错误：" << errMsg;
                    showMessage("系统", errMsg);
                }
                break;
            }

            // ------------------------------
            // 未知消息类型
            // ------------------------------
            default:
                qDebug() << "[未知消息] 收到未定义消息类型：" << header.msgType;
                showMessage("系统", "收到未知类型消息，可能是版本不兼容");
                break;
        }
    }
}

// 显示消息（已有实现，保持不变）
void ChatWindow::showMessage(const QString &sender, const QString &content) {
    qDebug() << "[显示消息] 发送者：" << sender << "，内容：" << content;

    // 临时弹窗（无论UI是否有问题，都会显示）
    QMessageBox::information(this, "收到消息", QString("%1：%2").arg(sender, content));

    QString time = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    QString msg = QString("[%1] %2：%3").arg(time).arg(sender).arg(content);
    m_chatList->addItem(msg);
    m_chatList->scrollToBottom(); // 自动滚动到最新消息
}

// 发送心跳包
void ChatWindow::sendHeartbeat() {
    if (m_serverSocket->state() != QAbstractSocket::ConnectedState) {
        qDebug() << "[心跳] 连接已断开，跳过发送";
        return;
    }

    PacketHeader header;
    header.msgType = HEARTBEAT;
    header.dataLen = 0;
    std::vector<char> sendData;
    sendData.insert(sendData.end(), reinterpret_cast<char*>(&header),
                    reinterpret_cast<char*>(&header) + sizeof(PacketHeader));

    qint64 bytesSent = m_serverSocket->write(sendData.data(), sendData.size());
    if (bytesSent == -1) {
        qDebug() << "[心跳] 发送失败：" << m_serverSocket->errorString();
    }
}

void ChatWindow::onUdpReadyRead() {
    // 目前UDP未使用，添加基础日志（后续扩展时再完善逻辑）
    qDebug() << "[UDP接收] 收到UDP数据（当前未处理）";
    
    // 若不需要日志，直接空实现也可：
    // （空实现不影响程序运行，仅占位避免链接错误）
}