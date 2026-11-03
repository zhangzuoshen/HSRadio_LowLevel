QT += core

TEMPLATE = lib
TARGET = hsradio_hardware

CONFIG += staticlib
CONFIG += c++17
CONFIG -= app_bundle

# HSRadio 硬件抽象层 + 业务接口
# 兼容 Qt 5.12 与 C++17。
#
# SAF775X 的 ALSA 真实后端默认编译（交叉编译/目标平台）。
# 在无 ALSA 头的开发机（如 Ubuntu）上调试时：
#   qmake CONFIG+=hsradio_no_alsa ../HSRadioHardware.pro
# 此时只编译 Mock 与业务层。

INCLUDEPATH += $$PWD/include

HEADERS += \
    $$PWD/include/hsradio/HSRadioTypes.h \
    $$PWD/include/hsradio/HSRadioSeekPolicy.h \
    $$PWD/include/hsradio/hardware/IRadioHardware.h \
    $$PWD/include/hsradio/hardware/MockRadioHardware.h \
    $$PWD/include/hsradio/manager/HSRadioManager.h \
    $$PWD/include/hsradio/scheduler/HSRadioOperationScheduler.h

SOURCES += \
    $$PWD/src/hardware/MockRadioHardware.cpp \
    $$PWD/src/manager/HSRadioManager.cpp \
    $$PWD/src/scheduler/HSRadioOperationScheduler.cpp

# 真实 ALSA 后端（默认编译；CONFIG+=hsradio_no_alsa 时排除）
!contains(CONFIG, hsradio_no_alsa) {
    HEADERS += $$PWD/include/hsradio/hardware/HSRadioSAF775XBackend.h
    SOURCES += $$PWD/src/hardware/HSRadioSAF775XBackend.cpp
    unix:!macx {
        LIBS += -lasound
    }
}

unix:!macx {
    QMAKE_CXXFLAGS += -Wall -Wextra -Wpedantic
}

# 目标工具链/系统根必须提供 ALSA 头文件与 libasound。
