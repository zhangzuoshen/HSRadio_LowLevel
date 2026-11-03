QT += core

TEMPLATE = app
TARGET = hsradio_manager_test

CONFIG += console c++17
CONFIG -= app_bundle

# 调度器/管理器接口测试。
# 只用 MockRadioHardware，不需要 ALSA，也不需要 i.MX9 真机：
#   qmake HSRadioManagerTest.pro && make && ./hsradio_manager_test

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
