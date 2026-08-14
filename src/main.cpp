#include <QGuiApplication>
#include <QNetworkProxy>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QUrl>

#include "tcpcontroller.h"
#include "lasercalibrationcontroller.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    // The arm and calibration device use direct TCP connections on the local
    // network. Bypass any proxy configuration inherited by Qt, because HTTP
    // and caching proxies cannot carry these raw device protocols.
    QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::NoProxy));

    TcpController controller;
    LaserCalibrationController laserController;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("tcpController", &controller);
    engine.rootContext()->setContextProperty("laserController", &laserController);
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));

    if (engine.rootObjects().isEmpty())
        return -1;

    return app.exec();
}
