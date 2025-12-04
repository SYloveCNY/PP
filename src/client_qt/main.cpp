#include <QApplication>
#include "LoginWindow.h"
#include "ChatWindow.h"

int main(int argc, char *argv[]) {
    QApplication a(argc, argv);

    LoginWindow loginWnd;
    ChatWindow *chatWnd = nullptr;

    // 登录成功后打开聊天窗口
    QObject::connect(&loginWnd, &LoginWindow::loginSuccess, [&](int userId, const QString &nickname, QTcpSocket *serverSocket) {
        chatWnd = new ChatWindow(userId, nickname, serverSocket);
        chatWnd->show();
    });

    loginWnd.show();
    return a.exec();
}
