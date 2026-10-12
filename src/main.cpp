#include <QGuiApplication>
#include <QNetworkProxy>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QUrl>

#include "tcpcontroller.h"
#include "lasercalibrationcontroller.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv); //创建一个GUI应用程序对象

    // The arm and calibration device use direct TCP connections on the local
    // network. Bypass any proxy configuration inherited by Qt, because HTTP
    // and caching proxies cannot carry these raw device protocols.
    QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    //不使用网络代理，因为连接的都是本地设备地址 局域网

    TcpController controller;      
    LaserCalibrationController laserController;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("tcpController", &controller);   //把控制器交给QML
    engine.rootContext()->setContextProperty("laserController", &laserController);
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));//加载qml文件

    if (engine.rootObjects().isEmpty())
        return -1;

    return app.exec();
}
