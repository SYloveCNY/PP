#include <QApplication>
#include <QFile>
#include <QTextStream>
#include <QDebug>
#include "LoginWindow.h"

int main(int argc, char *argv[]) {
    QApplication a(argc, argv);

    QString styleFilePath = "/home/sy/Dev/PP/res/style.qss";
    QFile styleFile(styleFilePath);

    if (styleFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream styleStream(&styleFile);
        QString styleSheet = styleStream.readAll();
        a.setStyleSheet(styleSheet);
        styleFile.close();
        qDebug() << "[成功] 样式表加载完成，路径：" << styleFilePath;
    } else {
        qCritical() << "[失败] 无法加载样式表：" << styleFile.errorString();
        qCritical() << "检查文件路径是否正确：" << styleFilePath;
    }

    LoginWindow w;
    w.show();

    return a.exec();
}