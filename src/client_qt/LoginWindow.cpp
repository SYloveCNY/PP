#include "LoginWindow.h"
#include <QFileDialog>
#include <QMessageBox>
#include <vector>   // 新增
#include <string>   // 新增

LoginWindow::LoginWindow(QWidget *parent) : QWidget(parent) {
    // 新增：打印包头大小（必须和服务端一致，都是 8 字节！）
    qDebug() << "[调试] 客户端 PacketHeader 大小：" << sizeof(PacketHeader);
    // 窗口配置
    setWindowTitle("聊天客户端 - 登录");
    setFixedSize(400, 300);

    // 初始化TCP Socket（与服务端通信）
    serverSocket = new QTcpSocket(this);
    connect(serverSocket, &QTcpSocket::connected, this, &LoginWindow::onConnected);
    connect(serverSocket, &QTcpSocket::readyRead, this, &LoginWindow::onReadyRead);

    // 新增：初始化UDP Socket（点对点通信，动态分配端口）
    udpSocket = new QUdpSocket(this);
    // 绑定动态端口：QHostAddress::Any表示监听所有网卡，端口0表示让系统分配可用端口
    if (!udpSocket->bind(QHostAddress::Any, 0)) {
        QMessageBox::warning(this, "警告", "UDP端口绑定失败：" + udpSocket->errorString());
        loginBtn->setEnabled(false);  // 绑定失败则禁用登录
        return;
    }

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

// LoginWindow.cpp 中的onConnected函数
void LoginWindow::onConnected() {
    try {
        LoginReq req;
        req.nickname = nicknameEdit->text().trimmed().toStdString();
        QString avatarPath = avatarEdit->text().trimmed();
        
        // 读取头像（空路径则avatar为空）
        if (!avatarPath.isEmpty()) {
            QFile file(avatarPath);
            if (file.open(QIODevice::ReadOnly)) {
                QByteArray data = file.readAll();
                req.avatar = std::vector<char>(data.begin(), data.end());
                file.close();
            } else {
                QMessageBox::warning(this, "警告", "无法打开头像文件");
                loginBtn->setEnabled(true);
                return;
            }
        }

        // 替换固定端口9999为动态分配的UDP端口
        // udpSocket->localPort()：获取系统分配的可用端口（quint16类型）
        req.dataPort = static_cast<uint16_t>(udpSocket->localPort());
        qDebug() << "动态分配的点对点端口：" << req.dataPort; 

        // 序列化登录请求
        std::vector<char> reqData = serializeLoginReq(req);
        
        // 组装PacketHeader
        PacketHeader header;
        header.msgType = LOGIN_REQ;
        header.dataLen = static_cast<uint32_t>(reqData.size());

        // 序列化header + 数据
        std::vector<char> sendData;
        sendData.insert(sendData.end(), reinterpret_cast<char*>(&header), 
                        reinterpret_cast<char*>(&header) + sizeof(PacketHeader));
        sendData.insert(sendData.end(), reqData.begin(), reqData.end());

        // 发送数据
        serverSocket->write(sendData.data(), sendData.size());
    } catch (const std::exception& e) {
        QMessageBox::critical(this, "错误", "序列化失败：" + QString::fromStdString(e.what()));
        loginBtn->setEnabled(true);
        return;
    }
}

void LoginWindow::onReadyRead() {
    QByteArray data = serverSocket->readAll();
    if (data.size() < sizeof(PacketHeader)) {
        QMessageBox::warning(this, "警告", "收到无效响应");
        return;
    }

    PacketHeader* header = reinterpret_cast<PacketHeader*>(data.data());
    if (header->msgType == LOGIN_RSP) {
        std::vector<char> payload(data.begin() + sizeof(PacketHeader), data.end());
        LoginRsp rsp = deserializeLoginRsp(payload);
        if (rsp.success) {
            QMessageBox::information(this, "成功", QString("登录成功！用户ID：%1").arg(rsp.userId));
            
            // 关键1：解除socket与LoginWindow的关联，避免被销毁
            serverSocket->setParent(nullptr);
            udpSocket->setParent(nullptr);

            // 关键2：登录成功后立即发送用户列表请求
            PacketHeader listReqHeader;
            listReqHeader.msgType = USER_LIST_REQ;
            listReqHeader.dataLen = 0;
            std::vector<char> listReqData;
            listReqData.insert(listReqData.end(), reinterpret_cast<char*>(&listReqHeader),
                              reinterpret_cast<char*>(&listReqHeader) + sizeof(PacketHeader));
            serverSocket->write(listReqData.data(), listReqData.size());
            qDebug() << "登录成功，已发送用户列表请求";

            // 打开聊天窗口
            emit loginSuccess(rsp.userId, QString::fromStdString(rsp.nickname), serverSocket, udpSocket);
            this->close();
        } else {
            QMessageBox::critical(this, "失败", QString("登录失败：%1").arg(QString::fromStdString(rsp.msg)));
            loginBtn->setText("登录");
            loginBtn->setEnabled(true);
        }
    }
}