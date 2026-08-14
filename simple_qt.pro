QT += core gui quick qml network mqtt sql concurrent

CONFIG += c++17
TEMPLATE = app
TARGET = simple_qt

SOURCES += \
    src/main.cpp \
    src/tcpcontroller.cpp \
    src/laservisionprocessor.cpp \
    src/lasercalibrationcontroller.cpp

HEADERS += \
    src/tcpcontroller.h \
    src/laservisionprocessor.h \
    src/lasercalibrationcontroller.h

RESOURCES += \
    qml/qml.qrc

OPENCV_RUNTIME_DLLS = libopencv_core345.dll \
    libopencv_imgproc345.dll \
    libopencv_imgcodecs345.dll \
    libopencv_videoio345.dll \
    opencv_ffmpeg345_64.dll

win32 {
    OPENCV_ROOT = $$PWD/third_party/opencv-3.4.5
    INCLUDEPATH += $$OPENCV_ROOT/include
    LIBS += -L$$OPENCV_ROOT/x64/mingw/lib \
            -lopencv_core345 -lopencv_imgproc345 -lopencv_videoio345 \
            -lwlanapi -lole32

    CONFIG(debug, debug|release): OPENCV_DEST = $$OUT_PWD/debug
    else: OPENCV_DEST = $$OUT_PWD/release
    OPENCV_BIN = $$OPENCV_ROOT/x64/mingw/bin
    for(dll, OPENCV_RUNTIME_DLLS) {
        QMAKE_POST_LINK += $$QMAKE_COPY /Y \
            $$shell_quote($$shell_path($$OPENCV_BIN/$$dll)) \
            $$shell_quote($$shell_path($$OPENCV_DEST)) $$escape_expand(\n\t)
    }
}
