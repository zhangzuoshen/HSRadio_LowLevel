QT += core

TEMPLATE = app
TARGET = hsradio_manager_test

CONFIG += console c++17
CONFIG -= app_bundle

# HSRadioManager 业务层接口调试工具（命令行参数驱动，所有操作经 HSRadioManager）。
# 后端由构建决定：带 ALSA 时用 HSRadioSAF775XBackend，否则回退 MockRadioHardware。
# 在 build/<子目录> 下 out-of-source 构建，禁止在源码根目录直接 qmake/make：
#   /path/to/qmake ../../HSRadioManagerTest.pro && make
#   ./hsradio_manager_test --help
#   ./hsradio_manager_test --set-frequency 94900 --set-frequency 96700 --print-state
#   ./hsradio_manager_test --seek --seek-mode full --no-progress

INCLUDEPATH += $$PWD/include

SOURCES += \
    $$PWD/hsradio_manager_test/main.cpp \
    $$PWD/src/hardware/MockRadioHardware.cpp \
    $$PWD/src/scheduler/HSRadioOperationScheduler.cpp \
    $$PWD/src/manager/HSRadioManager.cpp \
    $$PWD/src/storage/HSRadioStationStore.cpp \
    $$PWD/src/storage/HSRadioRepository.cpp \
    $$PWD/src/HSRadioLogging.cpp

HEADERS += \
    $$PWD/include/hsradio/HSRadioTypes.h \
    $$PWD/include/hsradio/HSRadioSeekPolicy.h \
    $$PWD/include/hsradio/hardware/IRadioHardware.h \
    $$PWD/include/hsradio/hardware/MockRadioHardware.h \
    $$PWD/include/hsradio/scheduler/HSRadioOperationScheduler.h \
    $$PWD/include/hsradio/manager/HSRadioManager.h \
    $$PWD/include/hsradio/storage/HSRadioStationStore.h \
    $$PWD/include/hsradio/storage/HSRadioRepository.h \
    $$PWD/include/hsradio/HSRadioLogging.h

unix:!macx {
    QMAKE_CXXFLAGS += -Wall -Wextra -Wpedantic
}

# 真实 ALSA 后端：默认编译；CONFIG+=hsradio_no_alsa 时排除（仅 Mock）。
# 与 HSRadioHardware.pro 保持一致——交叉编译/目标平台编入 SAF775X，宿主无 ALSA 头时排除。
!contains(CONFIG, hsradio_no_alsa) {
    DEFINES += HSRADIO_ENABLE_ALSA
    HEADERS += $$PWD/include/hsradio/hardware/HSRadioSAF775XBackend.h
    SOURCES += $$PWD/src/hardware/HSRadioSAF775XBackend.cpp
    unix:!macx {
        LIBS += -lasound
    }
}
