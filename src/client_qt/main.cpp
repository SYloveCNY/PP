#include <QApplication>
#include "LoginWindow.h"

int main(int argc, char *argv[]) {
    QApplication a(argc, argv);  // Qt应用入口

    // 只创建登录窗口并显示，登录成功后自动打开聊天窗口
    LoginWindow w;
    w.setWindowTitle("聊天客户端 - 登录");
    w.show();

    return a.exec();  // 启动应用事件循环
}