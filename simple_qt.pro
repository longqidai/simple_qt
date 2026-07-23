QT += core gui quick qml network mqtt

CONFIG += c++17
TEMPLATE = app
TARGET = simple_qt

SOURCES += \
    src/main.cpp \
    src/tcpcontroller.cpp

HEADERS += \
    src/tcpcontroller.h

RESOURCES += \
    qml/qml.qrc

win32:LIBS += -lwlanapi -lole32
