#include "LoginWindow.h"
#include <QFileDialog>
#include <QMessageBox>
#include <vector>   // 新增
#include <string>   // 新增

LoginWindow::LoginWindow(QWidget *parent) : QWidget(parent) {
    // 窗口配置
    setWindowTitle("聊天客户端 - 登录");
    setFixedSize(400, 300);

    // 控件初始化
    QLabel *titleLabel = new QLabel("🔐 登录");
    titleLabel->setStyleSheet("font-size: 24px; font-weight: bold;");
    titleLabel->setAlignment(Qt::AlignCenter);

    QLabel *nicknameLabel = new QLabel("昵称：");
    nicknameEdit = new QLineEdit;
    nicknameEdit->setPlaceholderText("请输入昵称");

    QLabel *avatarLabel = new QLabel("头像：");
    avatarEdit = new QLineEdit;
    avatarEdit->setPlaceholderText("无头像直接回车");
    QPushButton *browseBtn = new QPushButton("浏览");
    connect(browseBtn, &QPushButton::clicked, [this]() {
        QString path = QFileDialog::getOpenFileName(this, "选择头像", ".", "图片文件 (*.jpg *.png)");
        if (!path.isEmpty()) {
            avatarEdit->setText(path);
        }
    });

    loginBtn = new QPushButton("登录");
    loginBtn->setStyleSheet("background-color: #2196F3; color: white; font-size: 16px;");
    connect(loginBtn, &QPushButton::clicked, this, &LoginWindow::onLoginClicked);

    // 布局
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    QHBoxLayout *avatarLayout = new QHBoxLayout;
    avatarLayout->addWidget(avatarEdit);
    avatarLayout->addWidget(browseBtn);

    mainLayout->addWidget(titleLabel);
    mainLayout->addSpacing(20);
    mainLayout->addWidget(nicknameLabel);
    mainLayout->addWidget(nicknameEdit);
    mainLayout->addWidget(avatarLabel);
    mainLayout->addLayout(avatarLayout);
    mainLayout->addSpacing(30);
    mainLayout->addWidget(loginBtn);

    // 服务端连接
    serverSocket = new QTcpSocket(this);
    connect(serverSocket, &QTcpSocket::connected, this, &LoginWindow::onConnected);
    connect(serverSocket, &QTcpSocket::readyRead, this, &LoginWindow::onReadyRead);
}

void LoginWindow::onLoginClicked() {
    QString nickname = nicknameEdit->text().trimmed();
    if (nickname.isEmpty()) {
        QMessageBox::warning(this, "警告", "昵称不能为空！");
        return;
    }

    // 连接服务端
    serverSocket->connectToHost("127.0.0.1", 8888);
    loginBtn->setText("连接中...");
    loginBtn->setEnabled(false);
}

void LoginWindow::onConnected() {
    // 发送登录请求
    LoginReq req;
    req.nickname = nicknameEdit->text().trimmed().toStdString();
    QString avatarPath = avatarEdit->text().trimmed();
    if (!avatarPath.isEmpty()) {
        QFile file(avatarPath);
        if (file.open(QIODevice::ReadOnly)) {
            QByteArray data = file.readAll();
            req.avatar = std::vector<char>(data.begin(), data.end()); // 修复：添加std::
            file.close();
        }
    }
    req.dataPort = 9999;

    // 序列化
    std::vector<char> reqData; // 修复：添加std::
    auto nicknameData = serializeString(req.nickname);
    auto avatarData = serializeVector(req.avatar);
    reqData.insert(reqData.end(), nicknameData.begin(), nicknameData.end());
    reqData.insert(reqData.end(), avatarData.begin(), avatarData.end());
    reqData.insert(reqData.end(), (char*)&req.dataPort, (char*)&req.dataPort + sizeof(uint16_t));

    // 发送数据包
    PacketHeader header;
    header.msgType = MsgType::LOGIN_REQ;
    header.dataLen = reqData.size();
    QByteArray sendData;
    sendData.append((char*)&header, sizeof(PacketHeader));
    sendData.append(reqData.data(), reqData.size());
    serverSocket->write(sendData);
}

void LoginWindow::onReadyRead() {
    QByteArray data = serverSocket->readAll();
    PacketHeader *header = (PacketHeader*)data.data();
    if (header->msgType == MsgType::LOGIN_RSP) {
        LoginRsp rsp = deserializeLoginRsp(std::vector<char>(data.begin() + sizeof(PacketHeader), data.end())); // 修复：添加std::
        if (rsp.success) {
            QMessageBox::information(this, "成功", QString("登录成功！用户ID：%1\n提示：%2").arg(rsp.userId).arg(QString::fromStdString(rsp.msg)));
            emit loginSuccess(rsp.userId, QString::fromStdString(rsp.nickname), serverSocket);
            this->close();
        } else {
            QMessageBox::critical(this, "失败", QString("登录失败：%1").arg(QString::fromStdString(rsp.msg)));
            loginBtn->setText("登录");
            loginBtn->setEnabled(true);
        }
    }
}