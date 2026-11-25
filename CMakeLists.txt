cmake_minimum_required(VERSION 3.16)
project(SimpleQtQuick LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt5 5.14 REQUIRED COMPONENTS Quick Qml Network Mqtt Sql Concurrent Multimedia)

set(OPENCV_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/opencv-3.4.5")

qt5_add_resources(QT_RESOURCES qml/qml.qrc)

add_executable(simple_qt
    src/main.cpp
    src/audiofeedback.cpp
    src/audiofeedback.h
    src/tcpcontroller.cpp
    src/tcpcontroller.h
    src/laservisionprocessor.cpp
    src/laservisionprocessor.h
    src/lasercalibrationcontroller.cpp
    src/lasercalibrationcontroller.h
    ${QT_RESOURCES}
)

target_link_libraries(simple_qt PRIVATE
    Qt5::Quick
    Qt5::Qml
    Qt5::Network
    Qt5::Mqtt
    Qt5::Sql
    Qt5::Concurrent
    Qt5::Multimedia
    "${OPENCV_ROOT}/x64/mingw/lib/libopencv_core345.dll.a"
    "${OPENCV_ROOT}/x64/mingw/lib/libopencv_imgproc345.dll.a"
    "${OPENCV_ROOT}/x64/mingw/lib/libopencv_videoio345.dll.a"
)

target_include_directories(simple_qt PRIVATE "${OPENCV_ROOT}/include")

if(WIN32)
    foreach(OPENCV_DLL
            libopencv_core345.dll
            libopencv_imgproc345.dll
            libopencv_imgcodecs345.dll
            libopencv_videoio345.dll
            opencv_ffmpeg345_64.dll)
        add_custom_command(TARGET simple_qt POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${OPENCV_ROOT}/x64/mingw/bin/${OPENCV_DLL}"
                "$<TARGET_FILE_DIR:simple_qt>")
    endforeach()
endif()

if(WIN32)
    target_link_libraries(simple_qt PRIVATE wlanapi ole32)
endif()
