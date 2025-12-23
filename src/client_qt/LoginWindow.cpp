#include "LoginWindow.h"
#include <QMessageBox>
#include <QFont>
#include <iostream>

LoginWindow::LoginWindow(QWidget *parent) :
    QWidget(parent),
    titleLabel(new QLabel("🔐 登录", this)),
    nicknameLabel(new QLabel("昵称：", this)),
    nicknameEdit(new QLineEdit(this)),
    avatarLabel(new QLabel("头像：", this)),
    avatarEdit(new QLineEdit(this)),
    browseBtn(new QPushButton("浏览", this)),
    loginBtn(new QPushButton("登录", this)),
    mainLayout(new QVBoxLayout(this)),
    avatarLayout(new QHBoxLayout),
    serverSocket(new QTcpSocket(this))
{
    // 原有窗口设置、布局、信号槽连接...（不变）
    setWindowTitle("聊天客户端 - 登录");
    setFixedSize(300, 250);

    QFont titleFont;
    titleFont.setPointSize(16);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    titleLabel->setAlignment(Qt::AlignCenter);

    // 原有信号槽连接...
    connect(loginBtn, &QPushButton::clicked, this, &LoginWindow::onLoginClicked);
    connect(browseBtn, &QPushButton::clicked, this, [this]() {
        avatarPath = QFileDialog::getOpenFileName(this, "选择头像", "", "图片文件 (*.png *.jpg *.jpeg)").toStdString();
        avatarEdit->setText(QString::fromStdString(avatarPath));
    });
    connect(serverSocket, &QTcpSocket::errorOccurred, this, &LoginWindow::onSocketError);

    // 新增：连接 socket 的 readyRead 信号（接收服务端响应）
    connect(serverSocket, &QTcpSocket::readyRead, this, &LoginWindow::onReadyRead);

    // 原有布局设置...
    avatarLayout->addWidget(avatarEdit);
    avatarLayout->addWidget(browseBtn);
    mainLayout->addWidget(titleLabel);
    mainLayout->addSpacing(20);
    mainLayout->addWidget(nicknameLabel);
    mainLayout->addWidget(nicknameEdit);
    mainLayout->addSpacing(10);
    mainLayout->addWidget(avatarLabel);
    mainLayout->addLayout(avatarLayout);
    mainLayout->addSpacing(20);
    mainLayout->addWidget(loginBtn);
    setLayout(mainLayout);
}

LoginWindow::~LoginWindow() {}

void LoginWindow::onLoginClicked() {
    // 原有逻辑不变（获取昵称、构造请求、发送登录请求）...
    std::string nickname = nicknameEdit->text().trimmed().toStdString();
    if (nickname.empty()) {
        QMessageBox::warning(this, "警告", "请输入昵称！");
        return;
    }

    LoginReq req;
    req.nickname = nickname;
    req.dataPort = 0;
    if (!avatarPath.empty()) {
        QFile file(QString::fromStdString(avatarPath));
        if (file.open(QIODevice::ReadOnly)) {
            QByteArray avatarData = file.readAll();
            req.avatar.assign(avatarData.begin(), avatarData.end());
            file.close();
        } else {
            QMessageBox::warning(this, "警告", "头像文件读取失败！");
            return;
        }
    }

    std::vector<char> payload = serializeLoginReq(req);
    if (serverSocket->state() != QTcpSocket::ConnectedState) {
        serverSocket->connectToHost("127.0.0.1", 8888);
        if (!serverSocket->waitForConnected(3000)) {
            QMessageBox::critical(this, "错误", "连接服务端失败！");
            return;
        }
    }

    bool success = sendPacket(serverSocket, LOGIN_REQ, payload);
    if (!success) {
        QMessageBox::critical(this, "错误", "发送登录请求失败！");
    }
}

// 新增：接收服务端响应（解析 LOGIN_RSP）
void LoginWindow::onReadyRead() {
    // 读取服务端发送的完整数据包
    QByteArray recvData = serverSocket->readAll();
    if (recvData.size() < sizeof(PacketHeader)) {
        QMessageBox::warning(this, "警告", "服务端响应格式错误！");
        return;
    }

    // 解析包头
    PacketHeader header;
    memcpy(&header, recvData.data(), sizeof(PacketHeader));
    // 客户端接收时，也需要将网络字节序转本地字节序（和服务端对应）
    uint32_t msgType = ntohl(header.msgType);
    uint32_t dataLen = ntohl(header.dataLen);

    // 只处理登录响应（LOGIN_RSP）
    if (msgType != LOGIN_RSP) {
        return;
    }

    // 解析登录响应体
    if (recvData.size() < sizeof(PacketHeader) + dataLen) {
        QMessageBox::warning(this, "警告", "登录响应数据不完整！");
        return;
    }
    std::vector<char> payload(
        recvData.begin() + sizeof(PacketHeader),
        recvData.begin() + sizeof(PacketHeader) + dataLen
    );

    // 反序列化登录响应（serializeLoginRsp 的逆操作，来自 protocol_base.h）
    LoginRsp rsp = deserializeLoginRsp(payload);
    if (rsp.success) {
        // 登录成功：发射 loginSuccess 信号（传递关键信息给 main.cpp）
        emit loginSuccess(
            rsp.userId,
            rsp.nickname,
            serverSocket
        );
        this->hide(); // 隐藏登录窗口
    } else {
        // 登录失败
        QMessageBox::critical(this, "错误", QString::fromStdString(rsp.msg));
    }
}

void LoginWindow::onSocketError(QAbstractSocket::SocketError error) {
    Q_UNUSED(error);
    QMessageBox::critical(this, "网络错误", "Socket 错误：" + serverSocket->errorString());
}