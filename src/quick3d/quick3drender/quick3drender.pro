TARGET   = Qt3DQuickRender

QT      += core core-private qml qml-private 3dcore 3drender 3dquick 3dcore-private 3drender-private
CONFIG -= precompile_header

DEFINES += QT3DQUICKRENDER_LIBRARY

MODULE   = 3dquickrender

load(qt_module)

gcov {
    CONFIG += static
    QMAKE_CXXFLAGS += -fprofile-arcs -ftest-coverage
    QMAKE_LFLAGS += -fprofile-arcs -ftest-coverage
}

SOURCES += \
    qt3dquickrender_global.cpp \
    qt3dquickrendernodefactory.cpp

HEADERS += \
    qt3dquickrendernodefactory_p.h \
    qt3dquickrender_global_p.h \
    qt3dquickrender_global.h

!contains(QT_CONFIG, egl):DEFINES += QT_NO_EGL

# otherwise mingw headers do not declare common functions like ::strcasecmp
win32-g++*:QMAKE_CXXFLAGS_CXX11 = -std=gnu++0x

include(./items/items.pri)
