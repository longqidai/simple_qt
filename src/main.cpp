#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QUrl>

#include "tcpcontroller.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    TcpController controller;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("tcpController", &controller);
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));

    if (engine.rootObjects().isEmpty())
        return -1;

    return app.exec();
}
