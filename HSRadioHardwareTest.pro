QT += core

TEMPLATE = app
TARGET = hsradio_hardware_test

CONFIG += console c++17
CONFIG -= app_bundle

INCLUDEPATH += $$PWD/include

SOURCES += \
    $$PWD/main.cpp \
    $$PWD/src/hardware/AlsaRadioBackend.cpp

HEADERS += \
    $$PWD/include/hsradio/HSRadioTypes.h \
    $$PWD/include/hsradio/hardware/IRadioHardware.h \
    $$PWD/include/hsradio/hardware/AlsaRadioBackend.h

unix:!macx {
    LIBS += -lasound
    QMAKE_CXXFLAGS += -Wall -Wextra -Wpedantic
}
