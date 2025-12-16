#include "ChatWindow.h"
#include <QDateTime>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include "protocol_qt.h"

// 构造函数（完整实现，修复信号绑定）
ChatWindow::ChatWindow(int userId, const QString &nickname, QTcpSocket *serverSocket, QUdpSocket *udpSocket, QWidget *parent)
    : QWidget(parent), m_userId(userId), m_nickname(nickname),
      m_serverSocket(serverSocket), m_udpSocket(udpSocket) {
    // 窗口配置
    setWindowTitle(QString("聊天窗口 - %1（ID：%2）").arg(nickname).arg(userId));
    setFixedSize(800, 500); // 扩大窗口，容纳用户列表

    // 初始化控件（新增用户列表+标题标签）
    m_chatList = new QListWidget;
    
    // 新增：用户列表标题标签（替代 placeholder）
    QLabel *userListTitle = new QLabel("在线用户");
    userListTitle->setStyleSheet("font-weight: bold; text-align: center;");
    userListTitle->setAlignment(Qt::AlignCenter);

    m_userList = new QListWidget; // 在线用户列表UI
    m_userList->setFixedWidth(150);
    // 移除这行错误代码：m_userList->setPlaceholderText("在线用户");

    m_inputEdit = new QTextEdit;
    m_inputEdit->setPlaceholderText("输入消息后按Ctrl+Enter发送");
    m_inputEdit->setMaximumHeight(80);
    m_sendBtn = new QPushButton("发送");

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

    // 信号绑定（保持不变）
    connect(m_sendBtn, &QPushButton::clicked, this, &ChatWindow::sendMessage);
    connect(m_serverSocket, &QTcpSocket::readyRead, this, &ChatWindow::onServerReadyRead);
    connect(m_udpSocket, &QUdpSocket::readyRead, this, &ChatWindow::onUdpReadyRead);
    connect(m_userList, &QListWidget::itemClicked, this, &ChatWindow::onUserSelected);
    connect(m_inputEdit, &QTextEdit::textChanged, this, &ChatWindow::onTextEdited);

    // 发送用户列表请求（保持不变）
    PacketHeader header;
    header.msgType = USER_LIST_REQ;
    header.dataLen = 0;
    std::vector<char> sendData;
    sendData.insert(sendData.end(), reinterpret_cast<char*>(&header), 
                    reinterpret_cast<char*>(&header) + sizeof(PacketHeader));
    m_serverSocket->write(sendData.data(), sendData.size());
}

// 析构函数实现（匹配头文件声明）
ChatWindow::~ChatWindow() {
    // 可选：释放资源（QT父对象会自动管理，可留空）
}

// 发送消息核心函数（已有实现，保持不变）
void ChatWindow::sendMessage() {
    QString content = m_inputEdit->toPlainText().trimmed();
    if (content.isEmpty()) return;

    // 检查TCP连接状态
    if (m_serverSocket->state() != QAbstractSocket::ConnectedState) {
        QMessageBox::warning(this, "错误", "连接已断开，无法发送消息");
        return;
    }

    try {
        CommonMsg msg;
        msg.fromUserId = m_userId;
        msg.fromNickname = m_nickname.toStdString();
        msg.content = content.toStdString();
        msg.toUserId = m_selectedUserId; // 0=广播，选中用户则为目标ID

        // 序列化消息（调用protocol_qt.h中的全局sendPacket，而非类内函数）
        std::vector<char> msgData = serializeCommonMsg(msg);
        PacketHeader header;
        header.msgType = COMMON_MSG;
        header.dataLen = static_cast<uint32_t>(msgData.size());

        std::vector<char> sendData;
        sendData.insert(sendData.end(), reinterpret_cast<char*>(&header), 
                        reinterpret_cast<char*>(&header) + sizeof(PacketHeader));
        sendData.insert(sendData.end(), msgData.begin(), msgData.end());

        // 直接调用socket发送（避免依赖类内sendPacket）
        m_serverSocket->write(sendData.data(), sendData.size());

        // 显示自己的消息
        showMessage("我", content);
        m_inputEdit->clear();
    } catch (const std::exception& e) {
        QMessageBox::critical(this, "错误", "消息序列化失败：" + QString::fromStdString(e.what()));
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
    // 解析用户列表项（格式："昵称 (ID: 1)"）
    QString text = item->text();
    int idStart = text.indexOf("ID: ") + 4;
    int idEnd = text.indexOf(")", idStart);
    if (idStart < 4 || idEnd == -1) {
        m_selectedUserId = 0; // 解析失败则广播
        return;
    }
    m_selectedUserId = text.mid(idStart, idEnd - idStart).toInt();
    qDebug() << "选中用户ID：" << m_selectedUserId;
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

// 新增：更新在线用户列表（实现，解决未声明错误）
void ChatWindow::updateOnlineUsers(const std::map<int, UserInfo> &users) {
    m_userList->clear(); // 清空现有列表
    // 添加"广播"选项
    m_userList->addItem(QString("广播 (ID: 0)"));
    // 添加所有在线用户
    for (const auto &pair : users) {
        const UserInfo &user = pair.second;
        QString itemText = QString("%1 (ID: %2)").arg(QString::fromStdString(user.nickname)).arg(user.userId);
        m_userList->addItem(itemText);
    }
}

// 接收服务端消息（修复updateOnlineUsers和m_onlineUsers调用）
void ChatWindow::onServerReadyRead() {
    QByteArray data = m_serverSocket->readAll();
    if (data.size() < sizeof(PacketHeader)) {
        showMessage("系统", "收到无效消息");
        return;
    }

    PacketHeader *header = reinterpret_cast<PacketHeader*>(data.data());
    std::vector<char> payload(data.begin() + sizeof(PacketHeader), data.end());

    switch (header->msgType) {
        case USER_LIST_RSP: {
            std::map<int, UserInfo> users = deserializeUserList(payload);
            m_onlineUsers = users; // 更新本地用户列表
            updateOnlineUsers(m_onlineUsers); // 调用已声明的函数
            QString tip = QString("当前在线用户（%1人）").arg(users.size());
            showMessage("系统", tip);
            break;
        }
        case COMMON_MSG: {
            CommonMsg msg = deserializeCommonMsg(payload);
            showMessage(QString::fromStdString(msg.fromNickname), QString::fromStdString(msg.content));
            break;
        }
        case USER_ONLINE_NOTIFY: {
            UserInfo user = deserializeUserInfo(payload);
            m_onlineUsers[user.userId] = user; // 新增在线用户（m_onlineUsers已声明）
            updateOnlineUsers(m_onlineUsers);   // 刷新UI
            showMessage("系统", QString("用户「%1」上线了").arg(QString::fromStdString(user.nickname)));
            break;
        }
        case USER_OFFLINE_NOTIFY: {
            UserInfo user = deserializeUserInfo(payload);
            m_onlineUsers.erase(user.userId); // 移除离线用户（m_onlineUsers已声明）
            updateOnlineUsers(m_onlineUsers); // 刷新UI
            showMessage("系统", QString("用户「%1」下线了").arg(QString::fromStdString(user.nickname)));
            break;
        }
        default:
            showMessage("系统", "收到未知消息类型");
            break;
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