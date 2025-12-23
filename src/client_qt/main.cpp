#include <QApplication>
#include "LoginWindow.h"
#include "ChatWindow.h"
#include <QUdpSocket> // ChatWindow 需要 QUdpSocket，必须包含

int main(int argc, char *argv[]) {
    QApplication a(argc, argv);

    // 只创建登录窗口（不提前创建 ChatWindow）
    LoginWindow loginWin;

    // 连接登录成功信号（动态创建 ChatWindow，传递5个参数）
    QObject::connect(
        &loginWin,
        &LoginWindow::loginSuccess,
        [](int userId, const std::string& nickname, QTcpSocket* serverSocket) {
            // 1. 创建 UDP Socket（ChatWindow 第4个参数需要）
            QUdpSocket *udpSocket = new QUdpSocket;
            // 2. 动态创建 ChatWindow（参数顺序和类型完全匹配构造函数）
            ChatWindow *chatWin = new ChatWindow(
                userId,                                  // 第1个参数：userId（int）
                QString::fromStdString(nickname),        // 第2个参数：nickname（QString）
                serverSocket,                            // 第3个参数：serverSocket（QTcpSocket*）
                udpSocket,                               // 第4个参数：udpSocket（QUdpSocket*）
                nullptr                                  // 第5个参数：parent（QWidget*，可选）
            );
            // 3. 显示聊天窗口
            chatWin->show();
            // 可选：设置窗口关闭时释放资源
            QObject::connect(chatWin, &QWidget::destroyed, [udpSocket]() {
                udpSocket->deleteLater(); // 延迟释放 UDP Socket
            });
        }
    );

    loginWin.show();
    return a.exec();
}