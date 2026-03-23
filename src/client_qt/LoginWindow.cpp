#include "LoginWindow.h"
#include "ui_LoginWindow.h"
#include "ChatWindow.h"
#include "protocol.h"
#include "protocol_qt.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>
#include <QMessageBox>
#include <iostream>
#include <ostream>
#include <QtEndian>
#include <QTcpServer>
#include <QHostAddress>
#include <QRandomGenerator>
#include <QThread>
#include <QCloseEvent>
#include <QFileDialog>
#include <QCryptographicHash>
#include <QDir> 

// 新增：UDP重传最大次数定义
#define MAX_UDP_RETRY 3

LoginWindow::LoginWindow(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::LoginWindow),
    m_serverSocket(nullptr),
    m_heartbeatTimer(new QTimer(this)),
    m_udpSocket(new QUdpSocket(this)),          
    m_udpRetryTimer(new QTimer(this)),          
    m_userId(-1),
    m_dataPort(0),
    m_nickname(""),
    m_avatarPath(""),
    m_currentChunk(0),                          
    m_totalChunks(0),                           
    UDP_CHUNK_SIZE(1024),                       
    UDP_RETRY_TIMEOUT(1000),                    
    m_currentRetryCount(0), 
    m_serverUdpPort(8889)   
{
    ui->setupUi(this);
    
    // ====================== UDP相关初始化（核心修改）======================
    // 1. 优化UDP端口绑定：自动适配可用端口 + 允许端口复用
    if (!bindUdpPort(9102)) {
        QMessageBox::warning(this, "警告", "UDP端口绑定失败，头像上传功能将不可用！");
    }
    
    // 2. 绑定UDP数据接收信号（核心修改：增加DirectConnection确保立即触发）
    connect(m_udpSocket, &QUdpSocket::readyRead, 
            this, &LoginWindow::onUdpReadyRead,
            Qt::DirectConnection); // 关键：直接连接，避免事件循环延迟
    
    // 3. 绑定UDP重传定时器信号
    connect(m_udpRetryTimer, &QTimer::timeout, this, &LoginWindow::retryCurrentChunk);
    
    // ====================== 原有逻辑保持不变 ======================
    // 绑定心跳定时器
    connect(m_heartbeatTimer, &QTimer::timeout, this, &LoginWindow::sendHeartbeat);
    m_heartbeatTimer->stop();
    
    // 绑定选择头像按钮
    connect(ui->pushButton_select_avatar, &QPushButton::clicked, this, [=]() {
        QString avatarPath = QFileDialog::getOpenFileName(
            this,
            "选择头像",
            QDir::homePath(),
            "图片文件 (*.jpg *.jpeg *.png *.bmp);;所有文件 (*.*)"
        );
        
        if (avatarPath.isEmpty()) return;
        
        m_avatarPath = avatarPath;
        
        // 显示头像预览
        QPixmap pixmap(avatarPath);
        ui->label_avatar_preview->setPixmap(pixmap.scaled(
            ui->label_avatar_preview->size(),
            Qt::KeepAspectRatio,
            Qt::SmoothTransformation
        ));
        ui->label_avatar_preview->setText("");
    });

    // 新增：创建头像缓存文件夹（避免后续读取失败）
    QDir avatarDir("./avatars");
    if (!avatarDir.exists()) {
        avatarDir.mkpath(".");
    }
}

LoginWindow::~LoginWindow() {
    if (m_serverSocket) {
        m_serverSocket->disconnectFromHost();
        m_serverSocket->deleteLater();
    }
    delete ui;
}

// 新增：UDP端口自动绑定函数（核心修复：增加端口复用参数）
bool LoginWindow::bindUdpPort(int startPort) {
    // 尝试从startPort开始的10个端口
    for (int port = startPort; port < startPort + 10; port++) {
        // 修复：明确绑定IPv4地址
        if (m_udpSocket->bind(QHostAddress(QHostAddress::LocalHost), port, 
                              QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
            qDebug() << "UDP端口" << port << "绑定成功（IPv4）";
            return true;
        } else {
            qDebug() << "[警告] UDP端口" << port << "绑定失败：" << m_udpSocket->errorString();
        }
    }
    qCritical() << "UDP端口绑定失败：连续10个端口都被占用";
    return false;
}

// 修复发包函数：增加数据分片处理（解决大JSON包问题）
bool LoginWindow::sendPacket(QTcpSocket* socket, uint32_t msgType, const QByteArray& data) {
    if (!socket || socket->state() != QAbstractSocket::ConnectedState) {
        QMessageBox::warning(this, "错误", "Socket未连接或无效");
        return false;
    }

    // 分片大小限制（4KB）
    const int MAX_PACKET_SIZE = 4096;
    QByteArray sendData = data;
    
    // 如果数据超过限制，分片发送
    if (sendData.size() > MAX_PACKET_SIZE) {
        // 先发送头部（标记为分片开始）
        PacketHeader header;
        header.msgType = msgType;
        header.dataLen = sendData.size();
        header.msgId = QRandomGenerator::global()->generate(); // 随机消息ID
        header.senderId = m_userId <= 0 ? 0 : m_userId;
        
        PacketHeader netHeader = htonHeader(header);
        socket->write((char*)&netHeader, sizeof(PacketHeader));
        socket->flush();
        
        // 分片发送数据
        int offset = 0;
        while (offset < sendData.size()) {
            int chunkSize = qMin(MAX_PACKET_SIZE, sendData.size() - offset);
            QByteArray chunk = sendData.mid(offset, chunkSize);
            socket->write(chunk);
            socket->flush();
            offset += chunkSize;
            QThread::msleep(10); // 避免发送过快
        }
        return true;
    } else {
        // 正常发送（小数据包）
        PacketHeader header;
        header.msgType = msgType;
        header.dataLen = data.size();
        header.msgId = 0;
        header.senderId = m_userId <= 0 ? 0 : m_userId;

        PacketHeader netHeader = htonHeader(header);
        qint64 headerBytes = socket->write((char*)&netHeader, sizeof(PacketHeader));
        if (headerBytes != sizeof(PacketHeader)) {
            QMessageBox::warning(this, "错误", "发送协议头失败");
            return false;
        }

        if (!data.isEmpty()) {
            qint64 dataBytes = socket->write(data);
            if (dataBytes != data.size()) {
                return false;
            }
        }

        socket->flush();
        return true;
    }
}

uint16_t LoginWindow::getRandomAvailablePort() {
    const uint16_t minPort = 1024;
    const uint16_t maxPort = 65535;
    
    for (int i = 0; i < 10; ++i) {
        uint16_t randomPort = QRandomGenerator::global()->bounded(minPort, maxPort + 1);
        QTcpServer tempServer;
        if (tempServer.listen(QHostAddress::Any, randomPort)) {
            tempServer.close();
            return randomPort;
        }
    }
    
    return 9999;
}

// 修复登录逻辑：分离头像上传和登录流程
void LoginWindow::on_pushButton_login_clicked() {
    QString nickname = ui->lineEdit_nickname->text().trimmed();
    if (nickname.isEmpty()) {
        QMessageBox::warning(this, "警告", "请输入昵称！");
        return;
    }
    m_dataPort = getRandomAvailablePort();

    // 清理旧Socket
    if (m_serverSocket) {
        m_serverSocket->disconnectFromHost();
        m_serverSocket->deleteLater();
        m_serverSocket = nullptr;
    }

    m_serverSocket = new QTcpSocket(this);

    // 绑定错误处理
    connect(m_serverSocket, &QTcpSocket::errorOccurred, this, [=](QAbstractSocket::SocketError error) {
        QMessageBox::critical(this, "Socket错误", "连接/通信失败：" + m_serverSocket->errorString());
    });

    // 绑定登录响应处理
    connect(m_serverSocket, &QTcpSocket::readyRead, this, &LoginWindow::onLoginResponseReadyRead);
    
    // 连接服务器
    m_serverSocket->connectToHost("127.0.0.1", 8888);
    if (!m_serverSocket->waitForConnected(5000)) {
        QMessageBox::critical(this, "错误", "连接服务端失败！请检查服务端是否启动");
        m_serverSocket->deleteLater();
        m_serverSocket = nullptr;
        return;
    }

    // 构造登录请求（核心修改：添加udpPort字段）
    QJsonObject loginObj;
    loginObj["nickname"] = nickname;
    loginObj["dataPort"] = m_dataPort;
    loginObj["udpPort"] = m_udpSocket->localPort(); // 上报客户端绑定的UDP端口
    QJsonDocument doc(loginObj);
    QByteArray jsonData = doc.toJson(QJsonDocument::Compact);

    // 发送登录请求
    if (!sendPacket(m_serverSocket, static_cast<uint32_t>(MsgType::LOGIN_REQ), jsonData)) {
        QMessageBox::critical(this, "错误", "发送登录请求失败！");
        m_serverSocket->abort();
        m_serverSocket->deleteLater();
        m_serverSocket = nullptr;
        return;
    }
}

// 修复登录响应处理：登录成功后异步上传头像
void LoginWindow::onLoginResponseReadyRead() {
    if (!m_serverSocket || m_serverSocket->state() != QAbstractSocket::ConnectedState) {
        return;
    }

    while (m_serverSocket->bytesAvailable() >= sizeof(PacketHeader)) {
        PacketHeader netRspHeader;
        qint64 headerRead = m_serverSocket->read((char*)&netRspHeader, sizeof(PacketHeader));
        if (headerRead != sizeof(PacketHeader)) {
            QMessageBox::critical(this, "错误", "读取响应头部失败");
            m_serverSocket->abort();
            m_serverSocket->deleteLater();
            m_serverSocket = nullptr;
            return;
        }

        PacketHeader hostRspHeader;
        hostRspHeader.msgType = qFromBigEndian(netRspHeader.msgType);
        hostRspHeader.dataLen = qFromBigEndian(netRspHeader.dataLen);

        if (static_cast<MsgType>(hostRspHeader.msgType) != MsgType::LOGIN_RSP) {
            if (hostRspHeader.dataLen > 0) {
                m_serverSocket->read(hostRspHeader.dataLen);
            }
            continue;
        }

        // 读取完整响应数据
        QByteArray rspJsonData;
        if (hostRspHeader.dataLen > 0) {
            rspJsonData = m_serverSocket->read(hostRspHeader.dataLen);
            while (rspJsonData.size() < hostRspHeader.dataLen) {
                if (!m_serverSocket->waitForReadyRead(100)) {
                    QMessageBox::critical(this, "错误", "读取登录响应数据超时");
                    m_serverSocket->abort();
                    m_serverSocket->deleteLater();
                    m_serverSocket = nullptr;
                    return;
                }
                rspJsonData += m_serverSocket->read(hostRspHeader.dataLen - rspJsonData.size());
            }
        }

        // 解析响应
        QJsonDocument rspDoc = QJsonDocument::fromJson(rspJsonData);
        if (!rspDoc.isObject()) {
            QMessageBox::critical(this, "错误", "解析登录响应失败：" + QString(rspJsonData));
            m_serverSocket->abort();
            m_serverSocket->deleteLater();
            m_serverSocket = nullptr;
            return;
        }

        QJsonObject rspObj = rspDoc.object();
        bool success = rspObj["success"].toBool();
        QString msg = rspObj["msg"].toString();
        m_userId = rspObj["userId"].toInt();
        m_nickname = ui->lineEdit_nickname->text().trimmed();

        if (success) {
            // 解绑登录响应槽函数
            disconnect(m_serverSocket, &QTcpSocket::readyRead, this, &LoginWindow::onLoginResponseReadyRead);            
            m_heartbeatTimer->start(15000);
            
            // 异步上传头像（不阻塞聊天窗口打开）
            if (!m_avatarPath.isEmpty()) {
                QTimer::singleShot(1000, this, &LoginWindow::uploadAvatar);
            } else {
                // 加载历史头像
                QTimer::singleShot(500, this, [=]() {
                    loadAndShowAvatar(m_userId);
                });
            }
            
            // 打开聊天窗口
            ChatWindow* chatWindow = new ChatWindow;
            chatWindow->setLoginInfo(m_userId, m_nickname, m_serverSocket);

            // ========== 新增：传递当前用户头像到ChatWindow ==========
            QString avatarPath = QString("/home/sy/Dev/PP/avatars/%1_1.jpg").arg(m_userId);
            QPixmap selfAvatar(avatarPath);
            if (!selfAvatar.isNull()) {
                chatWindow->setSelfAvatar(selfAvatar);
            }

            chatWindow->show();
            this->hide();
            std::cout << "登录成功，UID：" << m_userId << std::endl;
        } else {
            QMessageBox::warning(this, "登录失败", msg);
            m_heartbeatTimer->stop();
            m_serverSocket->abort();
            m_serverSocket->deleteLater();
            m_serverSocket = nullptr;
        }
        break;
    }
}

// 优化头像上传：使用protocol.h的序列化函数，避免手动构造JSON
void LoginWindow::sendHeartbeat() {
    if (!m_serverSocket || m_serverSocket->state() != QAbstractSocket::ConnectedState) {
        m_heartbeatTimer->stop();
        std::cout << "心跳包发送跳过：未登录或服务端连接断开" << std::endl; // 新增日志
        return;
    }
    sendPacket(m_serverSocket, static_cast<uint32_t>(MsgType::HEARTBEAT), QByteArray());
}

void LoginWindow::onReadyRead() {}

void LoginWindow::closeEvent(QCloseEvent *event) {
    m_heartbeatTimer->stop();

    if (m_serverSocket && m_serverSocket->state() == QAbstractSocket::ConnectedState) {
        sendPacket(m_serverSocket, static_cast<uint32_t>(MsgType::LOGOUT_REQ), QByteArray());
        m_serverSocket->flush();
        
        QTimer::singleShot(100, this, [=]() {
            m_serverSocket->disconnectFromHost();
        });
    }

    event->accept();
}

// UDP数据接收处理（主要处理ACK）【核心修改：简化校验+增加详细日志】
void LoginWindow::onUdpReadyRead() {
    // 核心日志：确认槽函数被触发
    qDebug() << "\n===================== UDP接收触发 =====================";
    
    while (m_udpSocket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(m_udpSocket->pendingDatagramSize());
        QHostAddress senderAddr;
        quint16 senderPort;
        
        qint64 readLen = m_udpSocket->readDatagram(datagram.data(), datagram.size(), &senderAddr, &senderPort);
        qDebug() << "UDP包来源：" << senderAddr.toString() << ":" << senderPort << "，读取长度：" << readLen;
        qDebug() << "UDP包实际大小：" << datagram.size() << "，预期ACK大小：" << sizeof(UDPAckHeader);
        
        // ========== 修复1：Qt 6 兼容IPv6映射的IPv4地址 ==========
        // 将senderAddr转换为IPv4地址（如果是映射地址）
        QHostAddress realSenderAddr = senderAddr;
        if (senderAddr.protocol() == QAbstractSocket::IPv6Protocol) {
            // Qt 6 替代 isIPv4MappedAddress() 的实现
            Q_IPV6ADDR ipv6Addr = senderAddr.toIPv6Address();
            // 检查是否是IPv4映射的IPv6地址（前12字节特征：00 00 ... FF FF）
            bool isIPv4Mapped = (memcmp(ipv6Addr.c, "\0\0\0\0\0\0\0\0\0\0\xff\xff", 12) == 0);
            
            if (isIPv4Mapped) {
                // 提取真实IPv4地址（从IPv6字符串中移除::ffff:前缀）
                QString ipv4Str = senderAddr.toString().replace("::ffff:", "");
                realSenderAddr = QHostAddress(ipv4Str);
            }
        }
        
        // 校验真实IPv4地址（127.0.0.1）
        if (realSenderAddr != QHostAddress("127.0.0.1")) {
            qDebug() << "忽略非本地服务端包：" << senderAddr.toString() << "（真实地址：" << realSenderAddr.toString() << "）";
            continue;
        }
        
        // 临时注释严格的大小校验（先保证能解析）
        // if (datagram.size() != sizeof(UDPAckHeader)) {
        //     qWarning() << "ACK包大小错误，预期：" << sizeof(UDPAckHeader) << "实际：" << datagram.size();
        //     continue;
        // }
        
        // 安全校验：避免空指针/越界访问
        if (datagram.size() < sizeof(UDPAckHeader)) {
            qWarning() << "ACK包数据不足，无法解析：" << datagram.size() << "字节";
            continue;
        }
        
        // 强制解析ACK包（忽略大小校验，先确认数据）
        UDPAckHeader* ack = reinterpret_cast<UDPAckHeader*>(datagram.data());
        uint32_t msgType = qFromBigEndian(ack->msgType);
        uint32_t userId = qFromBigEndian(ack->userId);
        uint32_t chunkId = qFromBigEndian(ack->chunkId);
        bool success = ack->success;
        
        // 打印所有解析字段（调试用）
        qDebug() << "解析结果：msgType=" << msgType 
                 << "，userId=" << userId 
                 << "，chunkId=" << chunkId 
                 << "，success=" << success;
        
        // 简化校验逻辑：只要是当前用户的ACK就处理
        if (msgType == static_cast<uint32_t>(MsgType::AVATAR_UDP_ACK) && 
            userId == m_userId) {
            
            m_udpRetryTimer->stop();
            m_currentRetryCount = 0;
            qDebug() << "✅ 分片" << chunkId << "ACK确认成功";
            
            // 只处理当前分片的ACK（避免乱序）
            if (chunkId == m_currentChunk) {
                m_currentChunk++;
                if (m_currentChunk < m_totalChunks) {
                    sendUdpAvatarChunk();
                } else {
                    sendUdpAvatarFinish();
                }
            }
        } else {
            qDebug() << "❌ 非目标ACK：msgType=" << msgType << "，userId=" << userId << "（当前用户：" << m_userId << "）";
        }
    }
}

// 发送单个UDP分片
void LoginWindow::sendUdpAvatarChunk() {
    if (m_currentChunk >= m_totalChunks) {
        return;
    }
    
    // 先停止旧的定时器，避免重复启动
    m_udpRetryTimer->stop();
    
    int offset = m_currentChunk * UDP_CHUNK_SIZE;
    int chunkSize = qMin(UDP_CHUNK_SIZE, m_avatarData.size() - offset);
    QByteArray chunkData = m_avatarData.mid(offset, chunkSize);
    
    // 构造UDP头部
    UDPAvatarHeader header;
    header.msgType = qToBigEndian(static_cast<uint32_t>(MsgType::AVATAR_UDP_CHUNK));
    header.userId = qToBigEndian(m_userId);
    header.chunkId = qToBigEndian(m_currentChunk);
    header.totalChunks = qToBigEndian(m_totalChunks);
    header.chunkSize = qToBigEndian(chunkSize);
    header.totalSize = qToBigEndian(m_avatarData.size());
    
    // 填充MD5和文件名
    memset(header.fileMd5, 0, 33);
    strncpy(header.fileMd5, m_fileMd5.toStdString().c_str(), 32);
    memset(header.fileName, 0, 64);
    strncpy(header.fileName, m_fileName.toStdString().c_str(), 63);
    
    // 组合数据包（头部 + 分片数据）
    QByteArray sendData;
    sendData.append((char*)&header, sizeof(UDPAvatarHeader));
    sendData.append(chunkData);
    
    // 发送到服务器
    qint64 sent = m_udpSocket->writeDatagram(
        sendData, 
        QHostAddress("127.0.0.1"), 
        8889  
    );
    
    if (sent > 0) {
        qDebug() << "发送分片" << m_currentChunk << "，大小：" << sent;
        // 启动重传定时器
        m_udpRetryTimer->start(UDP_RETRY_TIMEOUT);
    } else {
        qWarning() << "分片" << m_currentChunk << "发送失败：" << m_udpSocket->errorString();
        // 立即重传
        QTimer::singleShot(100, this, &LoginWindow::retryCurrentChunk);
    }
}

// 修复retryCurrentChunk函数：增加重传次数限制
void LoginWindow::retryCurrentChunk() {
    m_udpRetryTimer->stop(); 
    m_currentRetryCount++;
    if (m_currentRetryCount > MAX_UDP_RETRY) {
        m_udpRetryTimer->stop();
        QMessageBox::warning(this, "警告", QString("分片%1上传失败（重传%2次后仍超时），头像上传中断！").arg(m_currentChunk).arg(MAX_UDP_RETRY));
        m_currentChunk = 0;
        m_totalChunks = 0;
        m_currentRetryCount = 0;
        m_avatarData.clear();
        return;
    }
    
    qWarning() << "分片" << m_currentChunk << "超时，重传(" << m_currentRetryCount << "/" << MAX_UDP_RETRY << ")...";
    sendUdpAvatarChunk();
    m_udpRetryTimer->start(UDP_RETRY_TIMEOUT); 
}

// 发送上传完成包
void LoginWindow::sendUdpAvatarFinish() {
    // 构造结束包
    UDPAvatarHeader header;
    header.msgType = qToBigEndian(static_cast<uint32_t>(MsgType::AVATAR_UDP_FINISH));
    header.userId = qToBigEndian(m_userId);
    header.chunkId = qToBigEndian(0);
    header.totalChunks = qToBigEndian(0);
    header.chunkSize = qToBigEndian(0);
    header.totalSize = qToBigEndian(m_avatarData.size());
    
    memset(header.fileMd5, 0, 33);
    strncpy(header.fileMd5, m_fileMd5.toStdString().c_str(), 32);
    memset(header.fileName, 0, 64);
    strncpy(header.fileName, m_fileName.toStdString().c_str(), 63);
    
    QByteArray sendData;
    sendData.append((char*)&header, sizeof(UDPAvatarHeader));
    
    // 发送结束包
    m_udpSocket->writeDatagram(sendData, QHostAddress("127.0.0.1"), 8889);
    
    // 重置状态
    m_currentChunk = 0;
    m_totalChunks = 0;
    m_currentRetryCount = 0; 
    m_avatarData.clear();
    
    QMessageBox::information(this, "成功", "头像通过UDP上传完成！");

    // ========== 新增：上传完成后立即加载头像 ==========
    loadAndShowAvatar(m_userId);
}

// 图片上传函数
void LoginWindow::uploadAvatar() {
    if (m_userId <= 0) {
        QMessageBox::warning(this, "错误", "未登录，无法上传头像");
        return;
    }
    
    // 读取头像文件
    QFile file(m_avatarPath);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, "错误", "无法打开头像文件：" + file.errorString());
        return;
    }
    m_avatarData = file.readAll();
    file.close();
    
    // 计算MD5
    QCryptographicHash hash(QCryptographicHash::Md5);
    hash.addData(m_avatarData);
    m_fileMd5 = hash.result().toHex();
    m_fileName = QFileInfo(m_avatarPath).fileName();
    
    // 计算总分片数
    m_totalChunks = (m_avatarData.size() + UDP_CHUNK_SIZE - 1) / UDP_CHUNK_SIZE;
    m_currentChunk = 0;
    m_currentRetryCount = 0; 
    
    qDebug() << "开始UDP上传头像，总大小： " << m_avatarData.size() << " ，总分片： " << m_totalChunks;
    
    // 发送第一个分片
    sendUdpAvatarChunk();
}

// 加载并显示头像的核心函数
void LoginWindow::loadAndShowAvatar(int userId) {
    // 1. 拼接服务端生成的头像文件路径（和服务端一致）
    QString avatarPath = QString("/home/sy/Dev/PP/avatars/%1_1.jpg").arg(userId);
    qDebug() << "[头像加载] 尝试加载：" << avatarPath;

    // 2. 检查文件是否存在
    QFile avatarFile(avatarPath);
    if (!avatarFile.exists()) {
        qWarning() << "[头像加载] 文件不存在：" << avatarPath;
        return;
    }
    qDebug() << "[头像加载] 文件大小：" << avatarFile.size() << "字节";

    // 3. 解码图片（兼容Qt解码问题）
    QPixmap avatarPixmap;
    if (!avatarPixmap.load(avatarPath)) {
        qWarning() << "[头像加载] QPixmap解码失败，尝试QImage";
        QImage avatarImage(avatarPath);
        if (avatarImage.isNull()) {
            qWarning() << "[头像加载] QImage也解码失败";
            return;
        }
        avatarPixmap = QPixmap::fromImage(avatarImage);
    }

    // 4. 渲染到UI控件（关键：替换为你实际的头像控件名）
    // 如果你想在登录窗口显示：用预览控件 ui->label_avatar_preview
    if (ui && ui->label_avatar_preview) {
        ui->label_avatar_preview->setPixmap(
            avatarPixmap.scaled(
                ui->label_avatar_preview->size(),
                Qt::KeepAspectRatio,
                Qt::SmoothTransformation
            )
        );
        ui->label_avatar_preview->setScaledContents(true); // 自动适配控件大小
        ui->label_avatar_preview->show();
        ui->label_avatar_preview->repaint(); // 强制刷新UI
        qDebug() << "[头像加载] 成功渲染到登录窗口预览控件";
    }
}
