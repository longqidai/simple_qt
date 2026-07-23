cmake_minimum_required(VERSION 3.16)
project(SimpleQtQuick LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt5 5.14 REQUIRED COMPONENTS Quick Qml Network Mqtt)

qt5_add_resources(QT_RESOURCES qml/qml.qrc)

add_executable(simple_qt
    src/main.cpp
    src/tcpcontroller.cpp
    src/tcpcontroller.h
    ${QT_RESOURCES}
)

target_link_libraries(simple_qt PRIVATE
    Qt5::Quick
    Qt5::Qml
    Qt5::Network
    Qt5::Mqtt
)

if(WIN32)
    target_link_libraries(simple_qt PRIVATE wlanapi ole32)
endif()
