#include "ChatWindow.h"
#include <QMessageBox>
#include <QDateTime>
#include "protocol_qt.h"

ChatWindow::ChatWindow(int userId, const QString &nickname, QTcpSocket *serverSocket, QUdpSocket *udpSocket, QWidget *parent)
    : QWidget(parent), m_userId(userId), m_nickname(nickname), 
      m_serverSocket(serverSocket), m_udpSocket(udpSocket) {  // 直接使用传递的UDP Socket
    setWindowTitle(QString("聊天窗口 - %1（ID：%2）").arg(nickname).arg(userId));
    setFixedSize(600, 400);

    // 绑定UDP的readyRead信号（接收点对点消息）
    connect(m_udpSocket, &QUdpSocket::readyRead, this, &ChatWindow::onUdpReadyRead);

    // 控件初始化（保持不变）
    m_chatList = new QListWidget;
    m_inputEdit = new QLineEdit;
    m_inputEdit->setPlaceholderText("输入消息后按回车或点击发送");
    m_sendBtn = new QPushButton("发送");

    // 布局（保持不变）
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    QHBoxLayout *inputLayout = new QHBoxLayout;
    inputLayout->addWidget(m_inputEdit);
    inputLayout->addWidget(m_sendBtn);

    mainLayout->addWidget(m_chatList);
    mainLayout->addLayout(inputLayout);

    // 信号绑定（保持不变）
    connect(m_sendBtn, &QPushButton::clicked, this, &ChatWindow::onSendClicked);
    connect(m_inputEdit, &QLineEdit::returnPressed, this, &ChatWindow::onSendClicked);
    connect(m_serverSocket, &QTcpSocket::readyRead, this, &ChatWindow::onServerReadyRead);

    // 发送用户列表请求（保持不变）
    PacketHeader header;
    header.msgType = USER_LIST_REQ;
    header.dataLen = 0;
    std::vector<char> sendData;
    sendData.insert(sendData.end(), reinterpret_cast<char*>(&header), 
                    reinterpret_cast<char*>(&header) + sizeof(PacketHeader));
    m_serverSocket->write(sendData.data(), sendData.size());
}

ChatWindow::~ChatWindow() {
    m_serverSocket->close();
    m_udpSocket->close();
}

// 修复：函数定义匹配声明（std::vector）
void ChatWindow::sendPacket(QTcpSocket *socket, MsgType msgType, const std::vector<char> &data) {
    PacketHeader header;
    header.msgType = msgType;
    header.dataLen = data.size();
    QByteArray sendData;
    sendData.append((char*)&header, sizeof(PacketHeader));
    sendData.append(data.data(), data.size());
    socket->write(sendData);
}

// 修复：函数定义匹配声明（std::map）
void ChatWindow::updateOnlineUsers(const std::map<int, UserInfo> &users) {
    m_onlineUsers = users;
    m_userList->clear();
    for (auto &[userId, user] : users) {
        QString itemText = QString("%1（ID：%2）").arg(QString::fromStdString(user.nickname)).arg(userId);
        if (userId == m_userId) {
            itemText += "（自己）";
        }
        QListWidgetItem *item = new QListWidgetItem(itemText);
        item->setData(Qt::UserRole, userId);
        m_userList->addItem(item);
    }
}

void ChatWindow::showMessage(const QString &sender, const QString &content) {
    QString time = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    m_msgDisplay->append(QString("[%1] %2：\n%3\n").arg(time).arg(sender).arg(content));
}

void ChatWindow::onServerReadyRead() {
    QByteArray data = m_serverSocket->readAll();
    PacketHeader *header = (PacketHeader*)data.data();
    // 修复：vector → std::vector
    std::vector<char> payload(data.begin() + sizeof(PacketHeader), data.end());

    switch (header->msgType) {
        case USER_LIST_RSP: {
            // 修复：map → std::map
            std::map<int, UserInfo> users = deserializeUserList(payload);
            updateOnlineUsers(users);
            break;
        }
        case COMMON_MSG: {
            CommonMsg msg = deserializeCommonMsg(payload);
            showMessage(QString::fromStdString(msg.fromNickname), QString::fromStdString(msg.content));
            break;
        }
        case USER_ONLINE_NOTIFY: {
            UserInfo user = deserializeUserInfo(payload);
            m_onlineUsers[user.userId] = user;
            updateOnlineUsers(m_onlineUsers);
            showMessage("系统通知", QString("%1（ID：%2）已上线").arg(QString::fromStdString(user.nickname)).arg(user.userId));
            break;
        }
        case USER_OFFLINE_NOTIFY: {
            int userId = *(int*)payload.data();
            // 修复：string → std::string
            std::string nickname(payload.begin() + sizeof(int), payload.end());
            m_onlineUsers.erase(userId);
            updateOnlineUsers(m_onlineUsers);
            showMessage("系统通知", QString("%1（ID：%2）已下线").arg(QString::fromStdString(nickname)).arg(userId));
            break;
        }
        default:
            break;
    }
}

// 新增：处理UDP接收的点对点消息
void ChatWindow::onUdpReadyRead() {
    QByteArray datagram;
    datagram.resize(m_udpSocket->pendingDatagramSize());
    QHostAddress senderAddr;
    quint16 senderPort;

    // 读取UDP数据报
    qint64 len = m_udpSocket->readDatagram(datagram.data(), datagram.size(), &senderAddr, &senderPort);
    if (len <= 0) {
        return;
    }

    // 解析点对点消息（假设是CommonMsg格式，可根据实际协议调整）
    try {
        CommonMsg msg = deserializeCommonMsg(std::vector<char>(datagram.begin(), datagram.end()));
        showMessage(QString::fromStdString(msg.fromNickname), QString::fromStdString(msg.content));
    } catch (const std::exception& e) {
        qDebug() << "解析UDP消息失败：" << e.what();
    }
}

void ChatWindow::onSendClicked() {
    QString content = m_msgInput->text().trimmed();
    if (content.isEmpty() || m_selectedUserId == -1) {
        return;
    }

    CommonMsg msg;
    msg.fromUserId = m_userId;
    msg.toUserId = m_selectedUserId; // 修复：彻底删除to_user_id，只用toUserId
    msg.fromNickname = m_nickname.toStdString();
    msg.content = content.toStdString();

    // 修复：vector → std::vector
    std::vector<char> msgData;
    auto nicknameData = serializeString(msg.fromNickname);
    auto contentData = serializeString(msg.content);
    msgData.insert(msgData.end(), (char*)&msg.fromUserId, (char*)&msg.fromUserId + sizeof(int));
    msgData.insert(msgData.end(), (char*)&msg.toUserId, (char*)&msg.toUserId + sizeof(int)); // 修复：toUserId
    msgData.insert(msgData.end(), nicknameData.begin(), nicknameData.end());
    msgData.insert(msgData.end(), contentData.begin(), contentData.end());

    sendPacket(m_serverSocket, COMMON_MSG, msgData);
    showMessage("我", content);
    m_msgInput->clear();
}

void ChatWindow::onTextEdited() {
    m_sendBtn->setEnabled(!m_msgInput->text().trimmed().isEmpty() && m_selectedUserId != -1);
}

void ChatWindow::onUserSelected(QListWidgetItem *item) {
    m_selectedUserId = item->data(Qt::UserRole).toInt();
    if (m_selectedUserId == m_userId) {
        m_selectedUserId = -1;
        QMessageBox::warning(this, "警告", "不能选择自己发送消息！");
        return;
    }
    m_sendBtn->setEnabled(!m_msgInput->text().trimmed().isEmpty());
}

void ChatWindow::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (event->modifiers() & Qt::ControlModifier) {
            m_msgInput->insert("\n");
        } else {
            onSendClicked();
        }
        event->accept();
    } else {
        QWidget::keyPressEvent(event);
    }
}