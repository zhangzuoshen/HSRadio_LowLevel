QT += core

TEMPLATE = app
TARGET = hsradio_manager_test

CONFIG += console c++17
CONFIG -= app_bundle

# 调度器 / 管理器接口调试工具（命令行参数驱动，对齐 HSRadioHardwareTest）。
# 后端是 MockRadioHardware，不需要 ALSA、不需要 i.MX9 真机：
#   qmake HSRadioManagerTest.pro && make
#   ./hsradio_manager_test --help
#   ./hsradio_manager_test --mock-demo --set-band FM --set-frequency 87500
#   ./hsradio_manager_test --mock-demo --seek --seek-lock-ms 0 --seek-sample-ms 0

INCLUDEPATH += $$PWD/include

SOURCES += \
    $$PWD/test/main.cpp \
    $$PWD/src/hardware/MockRadioHardware.cpp \
    $$PWD/src/scheduler/HSRadioOperationScheduler.cpp \
    $$PWD/src/manager/HSRadioManager.cpp

HEADERS += \
    $$PWD/include/hsradio/HSRadioTypes.h \
    $$PWD/include/hsradio/HSRadioSeekPolicy.h \
    $$PWD/include/hsradio/hardware/IRadioHardware.h \
    $$PWD/include/hsradio/hardware/MockRadioHardware.h \
    $$PWD/include/hsradio/scheduler/HSRadioOperationScheduler.h \
    $$PWD/include/hsradio/manager/HSRadioManager.h

unix:!macx {
    QMAKE_CXXFLAGS += -Wall -Wextra -Wpedantic
}
