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

OPENCV_ENABLED = false

unix {
    OPENCV_INCLUDE_CANDIDATES = /usr/include/opencv4 /usr/local/include/opencv4
    for(path, OPENCV_INCLUDE_CANDIDATES) {
        exists($$path/opencv2/core/version.hpp) {
            OPENCV_INCLUDE_DIR = $$path
        }
    }

    !isEmpty(OPENCV_INCLUDE_DIR) {
        message("Using OpenCV headers from $$OPENCV_INCLUDE_DIR")
        OPENCV_ENABLED = true
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
        warning("OpenCV headers not found. Install OpenCV and make its include/lib paths visible to qmake to enable OpenCV camera capture.")
    }
}

win32 {
    OPENCV_INCLUDE_DIR = $$clean_path($$OPENCV_INCLUDE_DIR)
    isEmpty(OPENCV_INCLUDE_DIR): OPENCV_INCLUDE_DIR = $$clean_path($$(OPENCV_INCLUDE_DIR))
    OPENCV_LIB_DIR = $$clean_path($$OPENCV_LIB_DIR)
    isEmpty(OPENCV_LIB_DIR): OPENCV_LIB_DIR = $$clean_path($$(OPENCV_LIB_DIR))

    OPENCV_ROOT_CANDIDATES = \
        $$clean_path($$OpenCV_DIR) \
        $$clean_path($$OPENCV_DIR) \
        $$clean_path($$OPENCV_ROOT) \
        $$clean_path($$(OpenCV_DIR)) \
        $$clean_path($$(OPENCV_DIR)) \
        $$clean_path($$(OPENCV_ROOT)) \
        D:/APP/msys64/mingw64

    # Add default fallback paths only if you actually have OpenCV there:
    # OPENCV_ROOT_CANDIDATES += C:/opencv/build C:/tools/opencv/build

    for(root, OPENCV_ROOT_CANDIDATES) {
        isEmpty(root): next()

        OPENCV_INCLUDE_CANDIDATES = \
            $$root/include \
            $$root/include/opencv4 \
            $$root/../include \
            $$root/../include/opencv4 \
            $$root/../../include \
            $$root/../../include/opencv4 \
            $$root/../../../include \
            $$root/../../../include/opencv4

        win32-g++ {
            OPENCV_LIB_CANDIDATES = \
                $$root/x64/mingw/lib \
                $$root/lib \
                $$root/../x64/mingw/lib \
                $$root/../lib \
                $$root/../../lib \
                $$root/../../../lib
        } else {
            OPENCV_LIB_CANDIDATES = \
                $$root/x64/vc17/lib \
                $$root/x64/vc16/lib \
                $$root/x64/vc15/lib \
                $$root/lib \
                $$root/../x64/vc17/lib \
                $$root/../x64/vc16/lib \
                $$root/../x64/vc15/lib \
                $$root/../lib \
                $$root/../../lib \
                $$root/../../../lib
        }

        for(includeDir, OPENCV_INCLUDE_CANDIDATES) {
            exists($$includeDir/opencv2/core/version.hpp) {
                OPENCV_INCLUDE_DIR = $$includeDir
            }
        }

        for(libDir, OPENCV_LIB_CANDIDATES) {
            exists($$libDir) {
                OPENCV_LIB_DIR = $$libDir
            }
        }

        !isEmpty(OPENCV_INCLUDE_DIR):!isEmpty(OPENCV_LIB_DIR) {
            break()
        }
    }

    !isEmpty(OPENCV_INCLUDE_DIR):!isEmpty(OPENCV_LIB_DIR) {
        OPENCV_WORLD_LIBS =
        OPENCV_COMPONENT_LIBS =

        win32-g++ {
            CONFIG(debug, debug|release) {
                OPENCV_WORLD_LIBS += $$files($$OPENCV_LIB_DIR/libopencv_world*d.a, true)
            }
            OPENCV_WORLD_LIBS += $$files($$OPENCV_LIB_DIR/libopencv_world*.a, true)

            CONFIG(debug, debug|release) {
                OPENCV_COMPONENT_LIBS += $$files($$OPENCV_LIB_DIR/libopencv_core*d.a, true)
                OPENCV_COMPONENT_LIBS += $$files($$OPENCV_LIB_DIR/libopencv_imgproc*d.a, true)
                OPENCV_COMPONENT_LIBS += $$files($$OPENCV_LIB_DIR/libopencv_videoio*d.a, true)
            }
            OPENCV_COMPONENT_LIBS += $$files($$OPENCV_LIB_DIR/libopencv_core*.a, true)
            OPENCV_COMPONENT_LIBS += $$files($$OPENCV_LIB_DIR/libopencv_imgproc*.a, true)
            OPENCV_COMPONENT_LIBS += $$files($$OPENCV_LIB_DIR/libopencv_videoio*.a, true)
        } else {
            CONFIG(debug, debug|release) {
                OPENCV_WORLD_LIBS += $$files($$OPENCV_LIB_DIR/opencv_world*d.lib, true)
            }
            OPENCV_WORLD_LIBS += $$files($$OPENCV_LIB_DIR/opencv_world*.lib, true)

            CONFIG(debug, debug|release) {
                OPENCV_COMPONENT_LIBS += $$files($$OPENCV_LIB_DIR/opencv_core*d.lib, true)
                OPENCV_COMPONENT_LIBS += $$files($$OPENCV_LIB_DIR/opencv_imgproc*d.lib, true)
                OPENCV_COMPONENT_LIBS += $$files($$OPENCV_LIB_DIR/opencv_videoio*d.lib, true)
            }
            OPENCV_COMPONENT_LIBS += $$files($$OPENCV_LIB_DIR/opencv_core*.lib, true)
            OPENCV_COMPONENT_LIBS += $$files($$OPENCV_LIB_DIR/opencv_imgproc*.lib, true)
            OPENCV_COMPONENT_LIBS += $$files($$OPENCV_LIB_DIR/opencv_videoio*.lib, true)
        }

        !isEmpty(OPENCV_WORLD_LIBS) {
            message("Using OpenCV from $$OPENCV_INCLUDE_DIR and $$OPENCV_LIB_DIR")
            OPENCV_ENABLED = true
            DEFINES += LANLINKCHAT_HAS_OPENCV
            INCLUDEPATH += $$OPENCV_INCLUDE_DIR
            LIBS += $$OPENCV_WORLD_LIBS
        } else:contains(OPENCV_COMPONENT_LIBS, .+) {
            message("Using OpenCV components from $$OPENCV_INCLUDE_DIR and $$OPENCV_LIB_DIR")
            OPENCV_ENABLED = true
            DEFINES += LANLINKCHAT_HAS_OPENCV
            INCLUDEPATH += $$OPENCV_INCLUDE_DIR
            LIBS += $$OPENCV_COMPONENT_LIBS
        } else {
            warning("OpenCV libraries were not found in $$OPENCV_LIB_DIR. Set OpenCV_DIR/OPENCV_DIR/OPENCV_ROOT, or OPENCV_INCLUDE_DIR and OPENCV_LIB_DIR, before running qmake.")
        }
    } else {
        win32-g++ {
            warning("OpenCV MinGW libraries were not found on Windows. Install a MinGW build of OpenCV and set OpenCV_DIR/OPENCV_DIR/OPENCV_ROOT, or OPENCV_INCLUDE_DIR and OPENCV_LIB_DIR, before running qmake.")
        } else {
            warning("OpenCV MSVC libraries were not found on Windows. Set OpenCV_DIR/OPENCV_DIR/OPENCV_ROOT, or OPENCV_INCLUDE_DIR and OPENCV_LIB_DIR, before running qmake.")
        }
    }
}

win32:CONFIG(release, debug|release): DESTDIR = $$PWD/bin/release
else:win32:CONFIG(debug, debug|release): DESTDIR = $$PWD/bin/debug
else:unix: DESTDIR = $$PWD/bin

win32:equals(OPENCV_ENABLED, true) {
    OPENCV_BIN_DIR = $$clean_path($$OPENCV_BIN_DIR)
    isEmpty(OPENCV_BIN_DIR): OPENCV_BIN_DIR = $$clean_path($$(OPENCV_BIN_DIR))

    isEmpty(OPENCV_BIN_DIR) {
        OPENCV_BIN_CANDIDATES = \
            $$OPENCV_ROOT/bin \
            $$OpenCV_DIR/bin \
            $$OPENCV_DIR/bin \
            $$clean_path($$(OPENCV_ROOT)/bin) \
            $$clean_path($$(OpenCV_DIR)/bin) \
            $$clean_path($$(OPENCV_DIR)/bin) \
            $$OPENCV_LIB_DIR/../bin \
            $$OPENCV_LIB_DIR/../../bin

        for(binDir, OPENCV_BIN_CANDIDATES) {
            exists($$binDir/libopencv_core*.dll) {
                OPENCV_BIN_DIR = $$clean_path($$binDir)
                break()
            }
        }
    }

    !isEmpty(OPENCV_BIN_DIR) {
        OPENCV_RUNTIME_PATTERNS = \
            libopencv_core*.dll \
            libopencv_imgcodecs*.dll \
            libopencv_imgproc*.dll \
            libopencv_videoio*.dll \
            avcodec-*.dll \
            avdevice-*.dll \
            avfilter-*.dll \
            avformat-*.dll \
            avutil-*.dll \
            libgsm*.dll \
            libglib-*.dll \
            libgobject-*.dll \
            libgio-*.dll \
            libgmodule-*.dll \
            libgthread-*.dll \
            libgst*.dll \
            libgcc_s_seh-*.dll \
            libgfortran-*.dll \
            libgomp-*.dll \
            libtbb*.dll \
            libstdc++-*.dll \
            libwinpthread-*.dll \
            libcaca*.dll \
            libopenblas*.dll \
            libopenal-*.dll \
            libjpeg*.dll \
            libpng*.dll \
            libtiff*.dll \
            libwebp*.dll \
            libopenjp*.dll \
            libOpenEXR*.dll \
            libImath*.dll \
            libIex*.dll \
            libIlmThread*.dll \
            swresample-*.dll \
            swscale-*.dll \
            zlib1.dll \
            postproc-*.dll

        OPENCV_RUNTIME_DLLS =
        for(pattern, OPENCV_RUNTIME_PATTERNS) {
            OPENCV_RUNTIME_DLLS += $$files($$OPENCV_BIN_DIR/$$pattern, true)
        }
        OPENCV_RUNTIME_DLLS = $$unique(OPENCV_RUNTIME_DLLS)

        !isEmpty(OPENCV_RUNTIME_DLLS) {
            message("OpenCV and FFmpeg runtime DLLs will be copied from $$OPENCV_BIN_DIR to $$DESTDIR")
            for(dll, OPENCV_RUNTIME_DLLS) {
                QMAKE_POST_LINK += $$quote(xcopy /Y \"$$shell_path($$dll)\" \"$$shell_path($$DESTDIR/)\" $$escape_expand(\n\t))
            }
        } else {
            warning("OpenCV runtime DLLs were not found in $$OPENCV_BIN_DIR. Add this folder to PATH or set OPENCV_BIN_DIR before running.")
        }
    } else {
        warning("OpenCV runtime DLL directory was not found. Add your OpenCV bin folder to PATH or set OPENCV_BIN_DIR before running.")
    }
}
