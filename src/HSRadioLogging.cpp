#include "hsradio/HSRadioLogging.h"

#include <QLoggingCategory>

// 默认级别直接由类别定义给出：Info 及以上启用，Debug 默认关闭。
// 不再用全局 QLoggingCategory::setFilterRules()：它是进程级覆盖式规则，
// 会与应用自己设置的规则互相覆盖，静态初始化时机也不可控；
// 类别自带默认级别时，QT_LOGGING_RULES 仍可按需重新打开 Debug。
Q_LOGGING_CATEGORY(hsradioManager, "hsradio.manager", QtInfoMsg)
Q_LOGGING_CATEGORY(hsradioHardwareMock, "hsradio.hardware.mock", QtInfoMsg)
Q_LOGGING_CATEGORY(hsradioHardwareSaf775x, "hsradio.hardware.saf775x", QtInfoMsg)
Q_LOGGING_CATEGORY(hsradioStorageStore, "hsradio.storage.store", QtInfoMsg)
Q_LOGGING_CATEGORY(hsradioStorageRepository, "hsradio.storage.repository", QtInfoMsg)
Q_LOGGING_CATEGORY(hsradioSeek, "hsradio.seek", QtInfoMsg)
