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

// Qt 5.12 的 qC*(cat, ...) 是可变参数宏；只用流式输出而不传可变参数时，
// -Wpedantic 会报 "ISO C++11 requires at least one argument for the ... in a variadic macro"。
// 统一经下列宏补一个空的可变参数，展开后仍走 QMessageLogger 的无参流式重载，
// 日志类别、级别与输出内容均不变；新增日志一律使用 HSRADIO_LOG_*。
#define HSRADIO_LOG_DEBUG(cat)    qCDebug(cat, )
#define HSRADIO_LOG_INFO(cat)     qCInfo(cat, )
#define HSRADIO_LOG_WARNING(cat)  qCWarning(cat, )
#define HSRADIO_LOG_CRITICAL(cat) qCCritical(cat, )
