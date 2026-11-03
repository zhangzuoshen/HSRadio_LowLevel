QT += core

TEMPLATE = lib
TARGET = hsradio_hardware

CONFIG += staticlib
CONFIG += c++17
CONFIG -= app_bundle

# HSRadio hardware abstraction layer
# Compatible with Qt 5.12 and C++17.

INCLUDEPATH += $$PWD/include

HEADERS += \
    $$PWD/include/hsradio/HSRadioTypes.h \
    $$PWD/include/hsradio/hardware/IRadioHardware.h \
    $$PWD/include/hsradio/hardware/HSRadioSAF775XBackend.h

SOURCES += \
    $$PWD/src/hardware/HSRadioSAF775XBackend.cpp

# ALSA Mixer Control API
unix:!macx {
    LIBS += -lasound
    QMAKE_CXXFLAGS += -Wall -Wextra -Wpedantic
}

# The target toolchain/sysroot must provide ALSA headers and libasound.
