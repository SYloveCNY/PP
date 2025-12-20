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
void ChatWindow::sendMessage() {
    QString content = m_inputEdit->toPlainText().trimmed();
    if (content.isEmpty()) {
        QMessageBox::warning(this, "提示", "消息内容不能为空！");
        return;
    }

    // 优化1：未选择用户（默认广播）时，添加二次确认
    if (m_selectedUserId == 0) {
        QMessageBox::StandardButton ret = QMessageBox::question(
            this, "确认广播", 
            "未选择具体用户，是否发送广播消息（所有在线用户可见）？",
            QMessageBox::Yes | QMessageBox::No
        );
        if (ret != QMessageBox::Yes) {
            return; // 用户取消，不发送
        }
    }

    // 优化2：再次检查连接状态（双重保障）
    if (m_serverSocket->state() != QAbstractSocket::ConnectedState) {
        QMessageBox::warning(this, "错误", "与服务端的连接已断开，无法发送消息！");
        return;
    }

    try {
        CommonMsg msg;
        msg.fromUserId = m_userId;
        msg.fromNickname = m_nickname.toStdString();
        msg.content = content.toStdString();
        msg.toUserId = m_selectedUserId; // 已通过选择更新，0=广播

        qDebug() << "[sendMessage] 发送消息：" << content 
                 << " 目标用户ID：" << m_selectedUserId;

        // 序列化+发送（沿用之前的逻辑）
        std::vector<char> msgData = serializeCommonMsg(msg);
        PacketHeader header;
        header.msgType = COMMON_MSG;
        header.dataLen = static_cast<uint32_t>(msgData.size());

        std::vector<char> sendData;
        sendData.insert(sendData.end(), reinterpret_cast<char*>(&header), 
                        reinterpret_cast<char*>(&header) + sizeof(PacketHeader));
        sendData.insert(sendData.end(), msgData.begin(), msgData.end());

        qint64 bytesSent = m_serverSocket->write(sendData.data(), sendData.size());
        if (bytesSent == -1) {
            throw std::runtime_error(m_serverSocket->errorString().toStdString());
        }

        // 发送成功后清空输入框+显示自己的消息
        m_inputEdit->clear();
        QString senderText = (m_selectedUserId == 0) ? "我（广播）" : "我";
        showMessage(senderText, content);

    } catch (const std::exception& e) {
        QMessageBox::critical(this, "发送失败", 
                             "消息发送失败：" + QString::fromStdString(e.what()));
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

// 新增：用户选择槽函数（实现）
void ChatWindow::onUserSelected(QListWidgetItem *item) {
    if (!item) return;

    QString itemText = item->text();
    qDebug() << "[onUserSelected] 选择的用户：" << itemText;

    // 解析 item 文本中的用户ID（格式："昵称 (ID: X)" 或 "广播 (ID: 0)"）
    int idStart = itemText.indexOf("ID: ") + 4;
    int idEnd = itemText.indexOf(")", idStart);
    if (idStart < 4 || idEnd == -1) {
        m_selectedUserId = 0; // 解析失败默认广播
        qDebug() << "[onUserSelected] 解析ID失败，默认广播";
        return;
    }

    // 更新选中的用户ID
    m_selectedUserId = itemText.mid(idStart, idEnd - idStart).toInt();
    qDebug() << "[onUserSelected] 选中用户ID：" << m_selectedUserId;

    // 可选：高亮选中项（优化体验）
    m_userList->setCurrentItem(item);
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
void ChatWindow::updateOnlineUsers(const std::map<int, UserInfo>& users) {
    m_userList->clear(); // 清空现有列表（含初始的“广播”）
    m_userList->addItem("广播 (ID: 0)"); // 重新添加广播选项

    qDebug() << "[updateOnlineUsers] 服务端返回在线用户数：" << users.size();
    for (const auto& [userId, user] : users) {
        // 跳过当前用户自己（避免显示自己）
        if (userId == m_userId) {
            qDebug() << "[updateOnlineUsers] 跳过当前用户：ID=" << userId;
            continue;
        }

        // 构建用户条目（处理空昵称、确保格式正确）
        QString nickname = QString::fromStdString(user.nickname);
        if (nickname.isEmpty()) nickname = QString("用户%1").arg(userId);
        QString itemText = QString("%1 (ID: %2)").arg(nickname).arg(userId);
        
        // 添加到列表并验证
        QListWidgetItem* item = m_userList->addItem(itemText);
        if (item) {
            qDebug() << "[updateOnlineUsers] 成功添加用户：" << itemText;
        } else {
            qDebug() << "[updateOnlineUsers] 添加用户失败：" << itemText;
        }
    }

    // 可选：默认选中“广播”选项
    if (m_userList->count() > 0) {
        m_userList->setCurrentRow(0);
        m_selectedUserId = 0; // 默认广播
    }
}

// 接收服务端消息（修复updateOnlineUsers和m_onlineUsers调用）
void ChatWindow::onServerReadyRead() {
    // 追加新数据到缓存
    m_recvBuffer.append(m_serverSocket->readAll());

    // 循环处理缓存中的完整数据包
    while (m_recvBuffer.size() >= sizeof(PacketHeader)) {
        // 解析包头
        PacketHeader* header = reinterpret_cast<PacketHeader*>(m_recvBuffer.data());
        // 检查数据包是否完整（包头+数据）
        if (m_recvBuffer.size() < sizeof(PacketHeader) + header->dataLen) {
            break; // 数据不完整，等待下一次readyRead
        }

        // 提取 payload
        std::vector<char> payload(
            m_recvBuffer.begin() + sizeof(PacketHeader),
            m_recvBuffer.begin() + sizeof(PacketHeader) + header->dataLen
        );

        // 处理不同类型的消息
        switch (header->msgType) {
            case USER_LIST_RSP: {
                try {
                    std::map<int, UserInfo> users = deserializeUserList(payload);
                    qDebug() << "[USER_LIST_RSP] 解析到在线用户数：" << users.size();
                    for (const auto& [userId, user] : users) {
                        qDebug() << "  - 用户ID：" << userId 
                                << " 昵称：" << QString::fromStdString(user.nickname)
                                << " IP：" << QString::fromStdString(user.ip);
                    }

                    // 更新本地在线用户列表+UI
                    m_onlineUsers = users;
                    updateOnlineUsers(m_onlineUsers);

                    // 显示提示（含用户数）
                    QString tip = QString("在线用户列表获取成功（共%1人）").arg(users.size());
                    showMessage("系统", tip);

                } catch (const std::exception& e) {
                    QString errMsg = "解析用户列表失败：" + QString::fromStdString(e.what());
                    showMessage("系统", errMsg);
                    qDebug() << "[USER_LIST_RSP] 错误：" << errMsg;
                }
                break;
            }
            case COMMON_MSG: {
                try {
                    CommonMsg msg = deserializeCommonMsg(payload);
                    showMessage(QString::fromStdString(msg.fromNickname), QString::fromStdString(msg.content));
                } catch (const std::exception& e) {
                    showMessage("系统", "解析消息失败：" + QString::fromStdString(e.what()));
                }
                break;
            }
            case USER_ONLINE_NOTIFY: {
                try {
                    UserInfo user = deserializeUserInfo(payload);
                    m_onlineUsers[user.userId] = user; // 添加到本地在线列表
                    updateOnlineUsers(m_onlineUsers); // 刷新UI
                    showMessage("系统", QString("用户「%1」已上线").arg(QString::fromStdString(user.nickname)));
                } catch (const std::exception& e) {
                    showMessage("系统", "解析上线通知失败：" + QString::fromStdString(e.what()));
                }
                break;
            }
            case USER_OFFLINE_NOTIFY: {
                try {
                    UserInfo user = deserializeUserInfo(payload);
                    m_onlineUsers.erase(user.userId); // 从本地列表移除
                    updateOnlineUsers(m_onlineUsers); // 刷新UI
                    showMessage("系统", QString("用户「%1」已下线").arg(QString::fromStdString(user.nickname)));
                } catch (const std::exception& e) {
                    showMessage("系统", "解析下线通知失败：" + QString::fromStdString(e.what()));
                }
                break;
            }
            case USER_ONLINE_NOTIFY:
            case USER_OFFLINE_NOTIFY:
                // 现有处理逻辑...
                break;
            default:
                showMessage("系统", "收到无效信息（未知消息类型）");
                break;
        }

        // 移除已处理的数据包
        m_recvBuffer.remove(0, sizeof(PacketHeader) + header->dataLen);
    }
}

// 接收UDP点对点消息（已有实现，保持不变）
void ChatWindow::onUdpReadyRead() {
    QByteArray datagram;
    datagram.resize(m_udpSocket->pendingDatagramSize());
    QHostAddress senderAddr;
    quint16 senderPort;

    qint64 len = m_udpSocket->readDatagram(datagram.data(), datagram.size(), &senderAddr, &senderPort);
    if (len <= 0) return;

    try {
        CommonMsg msg = deserializeCommonMsg(std::vector<char>(datagram.begin(), datagram.end()));
        showMessage(QString::fromStdString(msg.fromNickname), QString::fromStdString(msg.content));
    } catch (const std::exception& e) {
        qDebug() << "解析UDP消息失败：" << e.what();
    }
}

// 显示消息（已有实现，保持不变）
void ChatWindow::showMessage(const QString &sender, const QString &content) {
    QString time = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    QString msg = QString("[%1] %2：%3").arg(time).arg(sender).arg(content);
    m_chatList->addItem(msg);
    m_chatList->scrollToBottom(); // 自动滚动到最新消息
}