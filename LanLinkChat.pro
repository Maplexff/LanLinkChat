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
    src/ui/mainwindow.cpp

HEADERS += \
    src/model/chattypes.h \
    src/model/peerinfo.h \
    src/network/discoveryservice.h \
    src/network/peerconnection.h \
    src/network/peermanager.h \
    src/network/protocol.h \
    src/ui/mainwindow.h

INCLUDEPATH += $$PWD/src

win32:CONFIG(release, debug|release): DESTDIR = $$PWD/bin/release
else:win32:CONFIG(debug, debug|release): DESTDIR = $$PWD/bin/debug
else:unix: DESTDIR = $$PWD/bin
