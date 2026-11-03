#pragma once

#include <QLoggingCategory>

// HSRadio 日志类别定义。
// 级别约定（见 README "Logging" 段）：
//   qCInfo     关键过程
//   qCDebug    冗余调试信息
//   qCWarning  错误 / 警告
//   qCCritical 导致业务崩溃或业务功能失效
// 默认日志级别为 Info（Debug 默认关闭）。需要查看调试信息时设置：
//   export QT_LOGGING_RULES="hsradio.*.debug=true"
Q_DECLARE_LOGGING_CATEGORY(hsradioManager)
Q_DECLARE_LOGGING_CATEGORY(hsradioHardwareMock)
Q_DECLARE_LOGGING_CATEGORY(hsradioHardwareSaf775x)
Q_DECLARE_LOGGING_CATEGORY(hsradioStorageStore)
Q_DECLARE_LOGGING_CATEGORY(hsradioStorageRepository)
