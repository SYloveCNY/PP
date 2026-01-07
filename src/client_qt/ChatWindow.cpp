#include "ChatWindow.h"
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
{
    this->setWindowTitle("聊天窗口");
    this->setFixedSize(600, 400);

    // 初始化UI
    initUI();

    // 初始化心跳定时器（30秒一次）
    connect(m_heartbeatTimer, &QTimer::timeout, this, &ChatWindow::sendHeartbeat);
    m_heartbeatTimer->start(30000);
}

ChatWindow::~ChatWindow() = default;

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

// 初始化UI
void ChatWindow::initUI() {
    QVBoxLayout* mainLayout = new QVBoxLayout(this);

    // 1. 用户信息标签
    m_userInfoLabel = new QLabel(this);
    mainLayout->addWidget(m_userInfoLabel);

    // 2. 聊天记录框
    m_chatRecordEdit = new QTextEdit(this);
    m_chatRecordEdit->setReadOnly(true);
    mainLayout->addWidget(m_chatRecordEdit);

    // 3. 输入区域
    QHBoxLayout* inputLayout = new QHBoxLayout();
    m_inputEdit = new QLineEdit(this);
    QPushButton* sendBtn = new QPushButton("发送", this);
    QPushButton* sendImgBtn = new QPushButton("发送图片", this);

    inputLayout->addWidget(m_inputEdit);
    inputLayout->addWidget(sendBtn);
    inputLayout->addWidget(sendImgBtn);
    mainLayout->addLayout(inputLayout);

    // 连接按钮信号
    connect(sendBtn, &QPushButton::clicked, this, &ChatWindow::sendMessage);
    connect(sendImgBtn, &QPushButton::clicked, this, &ChatWindow::sendImageDialog);
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

    // 读取头部
    QByteArray headerData = socket->read(sizeof(PacketHeader));
    if (headerData.size() != sizeof(PacketHeader)) return;

    PacketHeader* netHeader = reinterpret_cast<PacketHeader*>(headerData.data());
    PacketHeader hostHeader = ntohHeader(*netHeader);

    // 读取数据
    QByteArray data = socket->read(hostHeader.dataLen);
    std::string dataStr = data.toStdString();

    // 处理不同类型的消息
    MsgType msgType = static_cast<MsgType>(hostHeader.msgType);
    switch (msgType) {
        // ========== 新增：处理上下线通知 ==========
        case MsgType::USER_STATUS_NOTIFY: {
            // 反序列化通知
            nlohmann::json notifyJson = nlohmann::json::parse(dataStr);
            UserStatusNotify notify = notifyJson.get<UserStatusNotify>();
            
            // 显示系统通知
            QString notifyText = QString("%1（ID：%2）%3")
                .arg(QString::fromStdString(notify.nickname))
                .arg(notify.userId)
                .arg(notify.isOnline ? "上线了" : "下线了");
            
            // 调用你现有的showMessage函数显示通知
            showMessage("系统通知", notifyText);
            
            // 可选：更新用户列表（如果需要）
            // updateUserList(); 
            break;
        }
        case MsgType::USER_LIST_RSP: {
            UserListRsp rsp = deserialize<UserListRsp>(dataStr);
            updateUserList(rsp.users);
            break;
        }
        case MsgType::COMMON_MSG: {
            showMessage("服务器", QString::fromStdString(dataStr));
            break;
        }
        default:
            break;
    }
}

// 更新用户列表（简化实现）
void ChatWindow::updateUserList(const std::vector<UserInfo>& users) {
    QString userListStr = "在线用户：\n";
    for (const auto& user : users) {
        userListStr += QString("%1（ID：%2，IP：%3，端口：%4）\n")
            .arg(QString::fromStdString(user.nickname))
            .arg(user.userId)
            .arg(QString::fromStdString(user.ip))
            .arg(user.dataPort);
    }
    m_chatRecordEdit->append(userListStr);
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
    QString content = m_inputEdit->text().trimmed();
    if (content.isEmpty() || !m_socket) return;

    // 序列化消息
    std::string data = content.toStdString();
    sendPacket(m_socket, static_cast<uint32_t>(MsgType::COMMON_MSG), data, nextMsgId++, m_userId);

    // 显示自己发送的消息
    showMessage(m_nickname, content);
    m_inputEdit->clear();
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

// 显示聊天消息（仅保留一个实现，解决重复定义）
void ChatWindow::showMessage(const QString& sender, const QString& content) {
    QString msg = QString("[%1] %2：%3").arg(QTime::currentTime().toString())
        .arg(sender)
        .arg(content);
    m_chatRecordEdit->append(msg);
}