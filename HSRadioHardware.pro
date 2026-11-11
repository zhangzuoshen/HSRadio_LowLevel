QT += core
QT -= gui

CONFIG += qt plugin
TEMPLATE = lib
TARGET = HSRadio

# 平台公共配置：注入 MODULE_NAME、HS_OS_ARM/HS_OS_LINUX、HS_SYSROOT_DIR，
# 以及平台各模块的 INCLUDEPATH / LIBS。相对路径按平台工程布局
# 换算到本仓库同级的 platform 目录。
include($${PWD}/../platform/HSCommon/Common.pri)

# Common.pri 默认 c++14；HSRadioTypes.h 用了 inline constexpr，必须 C++17。
CONFIG -= c++14
CONFIG += c++17
CONFIG -= app_bundle

# 产物仍落在 shadow build 目录（build/<子目录>/lib），不写进源码树或平台目录。
# 用子目录而非构建目录本身，避免 qmake 生成 "mv libHSRadio.so ./libHSRadio.so"
# 这种自移动规则（虽被忽略，但会打印 Error 1）。
DESTDIR = $$OUT_PWD/lib

INCLUDEPATH += $$PWD/include \
               $$PWD/../platform/HSMessageFramework/include \
               $$PWD/../platform/HSCommon/include \
               $$PWD/../platform/HSTL/include \
               $$PWD/../thirdparty

DEFINES += HS_RADIO_LIBRARY

HEADERS += \
    $$PWD/include/hsradio/HSRadioTypes.h \
    $$PWD/include/hsradio/HSRadioSeekPolicy.h \
    $$PWD/include/hsradio/hardware/IRadioHardware.h \
    $$PWD/include/hsradio/hardware/MockRadioHardware.h \
    $$PWD/include/hsradio/manager/HSRadioManager.h \
    $$PWD/include/hsradio/scheduler/HSRadioOperationScheduler.h \
    $$PWD/include/hsradio/storage/HSRadioStationStore.h \
    $$PWD/include/hsradio/storage/HSRadioRepository.h \
    $$PWD/include/hsradio/HSRadioLogging.h \
    $$PWD/include/hsradio/core/HSRadioMsgDefs.h \
    $$PWD/include/hsradio/core/HSRadioCore.h \
    $$PWD/include/hsradio/core/HSRadioPlugin.h

SOURCES += \
    $$PWD/src/hardware/MockRadioHardware.cpp \
    $$PWD/src/manager/HSRadioManager.cpp \
    $$PWD/src/scheduler/HSRadioOperationScheduler.cpp \
    $$PWD/src/storage/HSRadioStationStore.cpp \
    $$PWD/src/storage/HSRadioRepository.cpp \
    $$PWD/src/HSRadioLogging.cpp \
    $$PWD/src/core/HSRadioCore.cpp \
    $$PWD/src/core/HSRadioPlugin.cpp

# 真实 ALSA 后端默认编译；CONFIG+=hsradio_no_alsa 时只编 Mock 与业务层。
!contains(CONFIG, hsradio_no_alsa) {
    DEFINES += HSRADIO_ENABLE_ALSA
    HEADERS += $$PWD/include/hsradio/hardware/HSRadioSAF775XBackend.h
    SOURCES += $$PWD/src/hardware/HSRadioSAF775XBackend.cpp
}

LIBS += \
    -L$${DESTDIR} -L$${HS_SYSROOT_DIR}/app/ -lHSLogger -lHSMessageFramework -lHSDiaglog

!contains(CONFIG, hsradio_no_alsa) {
    contains(DEFINES, HS_OS_ARM) {
        LIBS += -L$${HS_SYSROOT_DIR}/app/usr/lib/ -lasound -lpthread
    }
    else:contains(DEFINES, HS_OS_LINUX) {
        LIBS += -lasound -lpthread
    }
}

# 供平台侧引用 Core 接口；插件元数据由 HSRadioPlugin.h 提供。
HSRadioCoreHeaders.path = /usr/include/hsradio/core
HSRadioCoreHeaders.files = $$PWD/include/hsradio/core/HSRadioCore.h \
                           $$PWD/include/hsradio/core/HSRadioMsgDefs.h \
                           $$PWD/include/hsradio/core/HSRadioPlugin.h
INSTALLS += HSRadioCoreHeaders

unix:!macx {
    QMAKE_CXXFLAGS += -Wall -Wextra -Wpedantic
}

# 目标工具链/系统根必须提供 ALSA 头文件与 libasound。
