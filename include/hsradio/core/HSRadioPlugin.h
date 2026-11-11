#pragma once

#include "hsradio/core/HSRadioCore.h"

// Qt5 插件壳：Q_PLUGIN_METADATA 只放在这里，HSRadioCore 保持纯净
// （可单测、也可被其他宿主直接实例化）。
//
// 平台插件加载器通过 IID_HSMWatcher 找到本插件，moc 生成的
// qt_plugin_instance() 会返回一个 HSRadioPlugin 实例；
// HSMWatcher 相关的虚方法全部由 HSRadioCore 实现。
//
// 链接期需要 HSMessageFramework / HSLogger（提供 HSMWatcher、
// HSMConnecter 的实现），由 HSRadioHardware.pro 插件工程负责。

class HSRadioPlugin : public HSRadioCore
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID IID_HSMWatcher)
    Q_INTERFACES(HSMWatcher)

public:
    explicit HSRadioPlugin(QObject *parent = Q_NULLPTR);
};
