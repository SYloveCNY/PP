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
    if (m_serverSocket->state() == QAbstractSocket::ConnectedState) {
        PacketHeader header;
        header.msgType = USER_LIST_REQ;
        header.dataLen = 0;
        std::vector<char> sendData;
        sendData.insert(sendData.end(), reinterpret_cast<char*>(&header), 
                        reinterpret_cast<char*>(&header) + sizeof(PacketHeader));
        m_serverSocket->write(sendData.data(), sendData.size());
        showMessage("系统", "正在获取在线用户列表...");
    } else {
        showMessage("系统", "连接未建立，无法获取在线用户");
    }
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
                    updateOnlineUsers(users);
                } catch (const std::exception& e) {
                    showMessage("系统", "解析用户列表失败：" + QString::fromStdString(e.what()));
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