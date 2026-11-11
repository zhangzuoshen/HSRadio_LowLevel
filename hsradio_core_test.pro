QT += core
QT -= gui

TEMPLATE = app
TARGET = hsradio_core_test

CONFIG += console c++17
CONFIG -= app_bundle

# HSRadioCoreTest —— 纯客户端消息调试工具。
# 只通过平台消息框架收发 Group::Radio 上的 HSRadioMsg 消息，
# 不编入 HSRadioCore / Manager / Scheduler / 存储 / 硬件后端等服务端实现。
# 服务端是独立运行的 HSRadio 插件，本程序通过 HSMConnecter::regist 接入 DBus。
#
# out-of-source 构建（X9E 工具链）：
#   source /home/zhangzuoshen/SDK/3VX/environment-setup-aarch64-sdrv-linux
#   mkdir -p build/core-test && cd build/core-test
#   qmake ../../hsradio_core_test.pro && make
# 运行需要设备上的 DBus SessionBus（DBUS_SESSION_BUS_ADDRESS）。

INCLUDEPATH += $$PWD/include \
               $$PWD/../platform/HSMessageFramework/include \
               $$PWD/../platform/HSCommon/include \
               $$PWD/../platform/HSTL/include \
               $$PWD/../thirdparty

HEADERS += \
    $$PWD/hsradio_core_test/HSRadioCoreClient.h \
    $$PWD/include/hsradio/core/HSRadioMsgDefs.h

SOURCES += \
    $$PWD/hsradio_core_test/main.cpp \
    $$PWD/hsradio_core_test/HSRadioCoreClient.cpp

HSRADIO_SYSROOT = $$(SDKTARGETSYSROOT)
LIBS += -L$$HSRADIO_SYSROOT/app/usr/lib -lHSMessageFramework -lpthread

unix:!macx {
    QMAKE_CXXFLAGS += -Wall -Wextra -Wpedantic
}
