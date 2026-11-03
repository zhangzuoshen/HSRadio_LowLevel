#include "hsradio/HSRadioLogging.h"

#include <QCoreApplication>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(hsradioManager, "hsradio.manager")
Q_LOGGING_CATEGORY(hsradioHardwareMock, "hsradio.hardware.mock")
Q_LOGGING_CATEGORY(hsradioHardwareSaf775x, "hsradio.hardware.saf775x")
Q_LOGGING_CATEGORY(hsradioStorageStore, "hsradio.storage.store")
Q_LOGGING_CATEGORY(hsradioStorageRepository, "hsradio.storage.repository")

namespace {

// 默认日志级别 Info：关闭所有 HSRadio 类别的 Debug 输出。
// 仅当用户未通过环境变量 QT_LOGGING_RULES 显式配置时才应用，
// 以免覆盖用户的调试开关设置。
struct HSRadioLoggingInitializer {
    HSRadioLoggingInitializer()
    {
        if (qEnvironmentVariableIsEmpty("QT_LOGGING_RULES")) {
            QLoggingCategory::setFilterRules(
                "hsradio.manager.debug=false\n"
                "hsradio.hardware.mock.debug=false\n"
                "hsradio.hardware.saf775x.debug=false\n"
                "hsradio.storage.store.debug=false\n"
                "hsradio.storage.repository.debug=false\n");
        }
    }
};

HSRadioLoggingInitializer g_hsRadioLoggingInitializer;

} // namespace
