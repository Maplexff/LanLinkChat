QT += core gui widgets network multimedia

CONFIG += c++17

TEMPLATE = app
TARGET = LanLinkChat

SOURCES += \
    src/main.cpp \
    src/network/discoveryservice.cpp \
    src/network/peerconnection.cpp \
    src/network/peermanager.cpp \
    src/network/protocol.cpp \
    src/network/videodecodeworker.cpp \
    src/ui/mainwindow.cpp \
    src/ui/opencvcamerathread.cpp \
    src/ui/videoencodeworker.cpp \
    src/ui/videoframewidget.cpp

HEADERS += \
    src/model/chattypes.h \
    src/model/peerinfo.h \
    src/network/discoveryservice.h \
    src/network/peerconnection.h \
    src/network/peermanager.h \
    src/network/protocol.h \
    src/network/videodecodeworker.h \
    src/ui/mainwindow.h \
    src/ui/opencvcamerathread.h \
    src/ui/videoencodeworker.h \
    src/ui/videoframewidget.h

FORMS += \
    src/ui/mainwindow.ui

INCLUDEPATH += $$PWD/src

unix {
    OPENCV_INCLUDE_CANDIDATES = /usr/include/opencv4 /usr/local/include/opencv4
    for(path, OPENCV_INCLUDE_CANDIDATES) {
        exists($$path/opencv2/core/version.hpp) {
            OPENCV_INCLUDE_DIR = $$path
        }
    }

    !isEmpty(OPENCV_INCLUDE_DIR) {
        message("Using OpenCV headers from $$OPENCV_INCLUDE_DIR")
        DEFINES += LANLINKCHAT_HAS_OPENCV
        INCLUDEPATH += $$OPENCV_INCLUDE_DIR
        LIBS += -lopencv_core -lopencv_imgproc -lopencv_videoio
        exists($$[QT_INSTALL_LIBS]/libavutil.so.59): QMAKE_LIBS += $$[QT_INSTALL_LIBS]/libavutil.so.59
        exists(/usr/lib/x86_64-linux-gnu/libva.so.2) {
            QMAKE_LFLAGS += -Wl,--copy-dt-needed-entries
            QMAKE_LFLAGS += -Wl,--allow-shlib-undefined
            QMAKE_LIBS += /usr/lib/x86_64-linux-gnu/libva.so.2
        }
    } else {
        warning("OpenCV headers not found. Install libopencv-dev to enable OpenCV camera capture.")
    }
}

win32:CONFIG(release, debug|release): DESTDIR = $$PWD/bin/release
else:win32:CONFIG(debug, debug|release): DESTDIR = $$PWD/bin/debug
else:unix: DESTDIR = $$PWD/bin
