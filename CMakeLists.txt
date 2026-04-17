cmake_minimum_required(VERSION 3.21)

project(LanLinkChat VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    Network
    Multimedia
)

qt_add_executable(LanLinkChat
    src/main.cpp
    src/model/chattypes.h
    src/model/peerinfo.h
    src/network/discoveryservice.cpp
    src/network/discoveryservice.h
    src/network/peerconnection.cpp
    src/network/peerconnection.h
    src/network/peermanager.cpp
    src/network/peermanager.h
    src/network/protocol.cpp
    src/network/protocol.h
    src/network/videodecodeworker.cpp
    src/network/videodecodeworker.h
    src/ui/mainwindow.cpp
    src/ui/mainwindow.h
    src/ui/mainwindow.ui
    src/ui/videoencodeworker.cpp
    src/ui/videoencodeworker.h
    src/ui/videoframewidget.cpp
    src/ui/videoframewidget.h
)

target_link_libraries(LanLinkChat PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::Multimedia
)

target_include_directories(LanLinkChat PRIVATE src)

qt_finalize_executable(LanLinkChat)
