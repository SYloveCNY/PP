#include <QApplication>
#include "LoginWindow.h"
#include "ChatWindow.h"

int main(int argc, char *argv[]) {
    QApplication a(argc, argv);
    LoginWindow loginWin;
    loginWin.show();

    // 连接登录成功信号，创建ChatWindow
    QObject::connect(&loginWin, &LoginWindow::loginSuccess, 
                     [](int userId, const QString &nickname, QTcpSocket *serverSocket, QUdpSocket *udpSocket) {
        ChatWindow *chatWin = new ChatWindow(userId, nickname, serverSocket, udpSocket);
        chatWin->show();
    });

    return a.exec();
}