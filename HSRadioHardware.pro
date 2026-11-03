QT += core

TEMPLATE = lib
TARGET = hsradio_hardware

CONFIG += staticlib
CONFIG += c++17
CONFIG -= app_bundle

# HSRadio 硬件抽象层
# 兼容 Qt 5.12 与 C++17。

INCLUDEPATH += $$PWD/include

HEADERS += \
    $$PWD/include/hsradio/HSRadioTypes.h \
    $$PWD/include/hsradio/hardware/IRadioHardware.h \
    $$PWD/include/hsradio/hardware/HSRadioSAF775XBackend.h

SOURCES += \
    $$PWD/src/hardware/HSRadioSAF775XBackend.cpp

# ALSA Mixer 控制接口
unix:!macx {
    LIBS += -lasound
    QMAKE_CXXFLAGS += -Wall -Wextra -Wpedantic
}

# 目标工具链/系统根必须提供 ALSA 头文件与 libasound。
