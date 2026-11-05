// HSRadioManagerTest —— HSRadioManager 业务层接口调试工具。
//
// 所有操作由命令行参数点名触发，统一经 HSRadioManager 门面执行，
// 不直接调用调度器原子命令；每一步打印 [PASS]/[FAIL] 与回读值，
// 不做写死的断言、不自动跑用例。
// 后端由构建决定：带 ALSA 时默认真实 SAF775X，否则回退 MockRadioHardware。
// 工具自身输出统一走 hsradio.managertest 日志类别。
//
//   ./hsradio_manager_test --help
//   ./hsradio_manager_test --set-frequency 94900 --print-state
//   ./hsradio_manager_test --seek --seek-mode full --no-progress

#include "hsradio/HSRadioTypes.h"
#include "hsradio/HSRadioLogging.h"
#include "hsradio/hardware/MockRadioHardware.h"
#if defined(HSRADIO_ENABLE_ALSA)
#include "hsradio/hardware/HSRadioSAF775XBackend.h"
#endif
#include "hsradio/scheduler/HSRadioOperationScheduler.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QEventLoop>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QVector>
#include <QtGlobal>

#include <functional>
#include <memory>

#include "hsradio/manager/HSRadioManager.h"
#include "hsradio/storage/HSRadioStationStore.h"
#include "hsradio/storage/HSRadioRepository.h"

// 测试工具专用日志类别：声明与定义都留在本文件，不暴露到公共头文件与库，
// 避免仅用于调试工具的类别影响其他模块。QtInfoMsg 维持项目默认的 Info 级别
// （Debug 默认关闭），需要时仍可用 QT_LOGGING_RULES 覆盖。
Q_DECLARE_LOGGING_CATEGORY(hsradioManagerTest)
Q_LOGGING_CATEGORY(hsradioManagerTest, "hsradio.managertest", QtInfoMsg)

using namespace HSRadio;

namespace {

QString errorCodeName(ErrorCode code)
{
    switch (code) {
    case ErrorCode::None: return QStringLiteral("None");
    case ErrorCode::InvalidParameter: return QStringLiteral("InvalidParameter");
    case ErrorCode::HardwareNotReady: return QStringLiteral("HardwareNotReady");
    case ErrorCode::AlsaOpenFailed: return QStringLiteral("AlsaOpenFailed");
    case ErrorCode::AlsaControlNotFound: return QStringLiteral("AlsaControlNotFound");
    case ErrorCode::AlsaControlTypeUnsupported: return QStringLiteral("AlsaControlTypeUnsupported");
    case ErrorCode::AlsaReadFailed: return QStringLiteral("AlsaReadFailed");
    case ErrorCode::AlsaWriteFailed: return QStringLiteral("AlsaWriteFailed");
    case ErrorCode::HardwareOperationFailed: return QStringLiteral("HardwareOperationFailed");
    }
    return QStringLiteral("UnknownError");
}


bool parseIntegerOption(const QCommandLineParser &parser,
                        const QCommandLineOption &option,
                        int *value)
{
    bool ok = false;
    const int parsed = parser.value(option).toInt(&ok);
    if (!ok)
        return false;
    *value = parsed;
    return true;
}

bool reportResult(const QString &operation, ErrorCode code, const QString &detail)
{
    if (code == ErrorCode::None) {
        HSRADIO_LOG_INFO(hsradioManagerTest).noquote() << "[PASS]" << operation;
        return true;
    }
    HSRADIO_LOG_CRITICAL(hsradioManagerTest).noquote()
        << "[FAIL]" << operation
        << "| code =" << errorCodeName(code)
        << (detail.isEmpty() ? QString() : QStringLiteral("| %1").arg(detail));
    return false;
}


QString bandName(Band band)
{
    return band == Band::FM ? QStringLiteral("FM") : QStringLiteral("AM");
}

QString signalText(const SignalInfo &info)
{
    QStringList fields;
    if (info.hasRssi)
        fields << QStringLiteral("RSSI=%1").arg(info.rssi);
    if (info.hasUsn)
        fields << QStringLiteral("USN=%1").arg(info.usn);
    if (info.hasWam)
        fields << QStringLiteral("WAM=%1").arg(info.wam);
    if (info.hasOffset)
        fields << QStringLiteral("OFFSET=%1").arg(info.offset);
    if (info.hasStereo)
        fields << QStringLiteral("Stereo=%1")
                      .arg(info.stereo ? QStringLiteral("yes") : QStringLiteral("no"));
    return fields.isEmpty() ? QStringLiteral("no signal data")
                            : fields.join(QStringLiteral(", "));
}

// 调度器是异步的：主线程泵事件直到条件成立。
bool waitUntil(const std::function<bool()> &predicate, int timeoutMs = 15000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate()) {
        if (timer.elapsed() > timeoutMs)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    return true;
}

// 收集调度器回传结果，供各条命令同步等待与打印。
struct Probe {
    QHash<quint64, ErrorCode> codes;
    QString lastError;
};

bool runAtom(Probe *probe, quint64 id, const QString &label, bool *anyFailed)
{
    if (id == 0) {
        HSRADIO_LOG_CRITICAL(hsradioManagerTest).noquote() << "[FAIL]" << label << "| scheduler not ready";
        *anyFailed = true;
        return false;
    }
    probe->lastError.clear();
    if (!waitUntil([&] { return probe->codes.contains(id); })) {
        HSRADIO_LOG_CRITICAL(hsradioManagerTest).noquote() << "[FAIL]" << label << "| timed out waiting for operationFinished";
        *anyFailed = true;
        return false;
    }
    const bool ok = reportResult(label, probe->codes.value(id), probe->lastError);
    if (!ok)
        *anyFailed = true;
    return ok;
}

} // namespace

int main(int argc, char *argv[])
{
    qSetMessagePattern("[%{time yyyy-MM-dd h:mm:ss.zzz}] [%{if-debug}D%{endif}%{if-info}I%{endif}%{if-warning}W%{endif}%{if-critical}C%{endif}%{if-fatal}F%{endif}] [%{pid}:%{threadid}] [%{category}] - %{message}");

    QCoreApplication app(argc, argv);
    ::registerHSRadioMetaTypes();  // 跨线程信号元类型注册（库内亦自注册，此处显式保险）
    QCoreApplication::setApplicationName(QStringLiteral("HSRadioManagerTest"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("HSRadioManager 业务层接口调试工具；带 ALSA 构建时默认使用真实 SAF775X 后端，否则使用 MockRadioHardware。"));
    parser.addHelpOption();
    parser.addVersionOption();


    // —— 输出控制 ——
    const QCommandLineOption noProgressOption(
        QStringLiteral("no-progress"),
        QStringLiteral("搜台时不打印逐频点进度。"));

    parser.addOption(noProgressOption);
    const QCommandLineOption managerStateFileOption(
        QStringLiteral("state-file"),
        QStringLiteral("状态文件路径（默认 /data/hsradio/radio_state.json）。"),
        QStringLiteral("path"),
        QStringLiteral("/data/hsradio/radio_state.json"));
    const QCommandLineOption managerConfigFileOption(
        QStringLiteral("config-file"),
        QStringLiteral("配置文件路径（默认 /data/hsradio/radio_config.json）。"),
        QStringLiteral("path"),
        QStringLiteral("/data/hsradio/radio_config.json"));
    const QCommandLineOption managerSetBandOption(
        QStringLiteral("set-band"),
        QStringLiteral("manager.setBand：FM 或 AM。"),
        QStringLiteral("FM|AM"));
    const QCommandLineOption managerSetFrequencyOption(
        QStringLiteral("set-frequency"),
        QStringLiteral("manager.setFrequency：kHz。"),
        QStringLiteral("kHz"));
    const QCommandLineOption managerStepUpOption(
        QStringLiteral("step-up"),
        QStringLiteral("manager.stepUp()。"));
    const QCommandLineOption managerStepDownOption(
        QStringLiteral("step-down"),
        QStringLiteral("manager.stepDown()。"));
    const QCommandLineOption managerSetMuteOption(
        QStringLiteral("set-mute"),
        QStringLiteral("manager.setMute：on 或 off。"),
        QStringLiteral("on|off"));
    const QCommandLineOption managerRefreshOption(
        QStringLiteral("refresh"),
        QStringLiteral("manager.refreshSignal()。"));
    const QCommandLineOption managerSeekOption(
        QStringLiteral("seek"),
        QStringLiteral("manager.startSeek()：用配置里的频段范围与阈值扫描。"));
    const QCommandLineOption managerSeekDownOption(
        QStringLiteral("seek-down"),
        QStringLiteral("向下搜台（默认向上）。"));
    const QCommandLineOption managerSeekModeOption(
        QStringLiteral("seek-mode"),
        QStringLiteral("full=全频段重建，next=找下一个有效台（默认 full）。"),
        QStringLiteral("full|next"),
        QStringLiteral("full"));
    const QCommandLineOption managerSeekCancelAfterOption(
        QStringLiteral("seek-cancel-after"),
        QStringLiteral("N ms 后调用 stopSeek() 调试取消路径。"),
        QStringLiteral("ms"));
    const QCommandLineOption managerAddStationOption(
        QStringLiteral("add-station"),
        QStringLiteral("添加电台 kHz[:name]（可重复）。"),
        QStringLiteral("spec"));
    const QCommandLineOption managerAddFavOption(
        QStringLiteral("add-fav"),
        QStringLiteral("添加收藏 kHz[:name]（可重复）。"),
        QStringLiteral("spec"));
    const QCommandLineOption managerRemoveStationOption(
        QStringLiteral("remove-station"),
        QStringLiteral("删除电台（按 id）。"),
        QStringLiteral("id"));
    const QCommandLineOption managerRemoveFavOption(
        QStringLiteral("remove-fav"),
        QStringLiteral("删除收藏（按 id）。"),
        QStringLiteral("id"));
    const QCommandLineOption managerListStationsOption(
        QStringLiteral("list-stations"),
        QStringLiteral("列出当前频段电台。"));
    const QCommandLineOption managerListFavsOption(
        QStringLiteral("list-favs"),
        QStringLiteral("列出当前频段收藏。"));
    const QCommandLineOption managerRestoreOption(
        QStringLiteral("restore"),
        QStringLiteral("manager.restoreLastState()。"));
    const QCommandLineOption managerFactoryResetOption(
        QStringLiteral("factory-reset"),
        QStringLiteral("manager.factoryReset()：状态与配置回默认。"));
    const QCommandLineOption managerPrintStateOption(
        QStringLiteral("print-state"),
        QStringLiteral("打印存储与配置状态摘要。"));

    parser.addOption(managerStateFileOption);
    parser.addOption(managerConfigFileOption);
    parser.addOption(managerSetBandOption);
    parser.addOption(managerSetFrequencyOption);
    parser.addOption(managerStepUpOption);
    parser.addOption(managerStepDownOption);
    parser.addOption(managerSetMuteOption);
    parser.addOption(managerRefreshOption);
    parser.addOption(managerSeekOption);
    parser.addOption(managerSeekDownOption);
    parser.addOption(managerSeekModeOption);
    parser.addOption(managerSeekCancelAfterOption);
    parser.addOption(managerAddStationOption);
    parser.addOption(managerAddFavOption);
    parser.addOption(managerRemoveStationOption);
    parser.addOption(managerRemoveFavOption);
    parser.addOption(managerListStationsOption);
    parser.addOption(managerListFavsOption);
    parser.addOption(managerRestoreOption);
    parser.addOption(managerFactoryResetOption);
    parser.addOption(managerPrintStateOption);

    parser.process(app);


    // —— 后端由构建决定：带 ALSA 时用真实 SAF775X，否则回退 Mock ——
    std::unique_ptr<IRadioHardware> hardware;
    QString backendLabel;
#if defined(HSRADIO_ENABLE_ALSA)
    HSRadioSAF775XConfig safConfig;   // 全部走默认值
    hardware.reset(new HSRadioSAF775XBackend(safConfig));
    backendLabel = QStringLiteral("initialize(HSRadioSAF775XBackend)");
    HSRADIO_LOG_INFO(hsradioManagerTest).noquote()
        << QStringLiteral("Backend: HSRadioSAF775XBackend (ALSA, defaults)");
#else
    hardware.reset(new MockRadioHardware);
    backendLabel = QStringLiteral("initialize(MockRadioHardware)");
    HSRADIO_LOG_INFO(hsradioManagerTest).noquote()
        << QStringLiteral("Backend: MockRadioHardware (ALSA build unavailable)");
#endif


    HSRadioOperationScheduler scheduler;
    Probe probe;

    bool initialized = false;
    ErrorCode initCode = ErrorCode::HardwareOperationFailed;

    QObject::connect(&scheduler, &HSRadioOperationScheduler::initialized, &app,
                     [&](ErrorCode code) { initialized = true; initCode = code; });
    QObject::connect(&scheduler, &HSRadioOperationScheduler::operationFinished, &app,
                     [&](quint64 id, ErrorCode code) { probe.codes.insert(id, code); });
    QObject::connect(&scheduler, &HSRadioOperationScheduler::errorOccurred, &app,
                     [&](ErrorCode code, const QString &message) {
                         probe.lastError = QStringLiteral("code=%1, message=%2")
                                               .arg(errorCodeName(code), message);
                         HSRADIO_LOG_WARNING(hsradioManagerTest).noquote() << "[ERROR]" << probe.lastError;
                     });
    QObject::connect(&scheduler, &HSRadioOperationScheduler::seekHit, &app,
                     [&](quint64, int frequencyKHz, const SignalInfo &info) {
                                          HSRADIO_LOG_INFO(hsradioManagerTest).noquote()
                             << QStringLiteral("[FOUND] %1 kHz | %2")
                                    .arg(frequencyKHz)
                                    .arg(signalText(info));
                     });
    QObject::connect(&scheduler, &HSRadioOperationScheduler::seekProgress, &app,
                     [&](quint64, int frequencyKHz) {
                                          if (!parser.isSet(noProgressOption))
                             HSRADIO_LOG_INFO(hsradioManagerTest).noquote() << QStringLiteral("[SEEK] scanning %1 kHz").arg(frequencyKHz);
                     });

    const Result initResult = scheduler.initialize(std::move(hardware));
    if (!initResult.ok()) {
        HSRADIO_LOG_CRITICAL(hsradioManagerTest).noquote() << "[FAIL] initialize |" << initResult.message;
        return 2;
    }
    if (!waitUntil([&] { return initialized; })) {
        HSRADIO_LOG_CRITICAL(hsradioManagerTest).noquote() << "[FAIL] initialize | timed out waiting for initialized signal";
        return 1;
    }
    if (!reportResult(backendLabel, initCode, QString())) {
        scheduler.shutdown();
        return 1;
    }

    bool anyFailed = false;

    // —— 统一经 HSRadioManager 门面执行业务接口 ——
    {
        const QString stateFile = parser.isSet(managerStateFileOption)
            ? parser.value(managerStateFileOption) : QStringLiteral("/data/hsradio/radio_state.json");
        const QString configFile = parser.isSet(managerConfigFileOption)
            ? parser.value(managerConfigFileOption) : QStringLiteral("/data/hsradio/radio_config.json");
        HSRadioStationStore store(stateFile);
        HSRadioRepository repository(configFile);
        HSRadioManager manager(&scheduler, &store, &repository);

        QObject::connect(&manager, &HSRadioManager::initialized, &app, [&](ErrorCode code){
            HSRADIO_LOG_INFO(hsradioManagerTest).noquote() << QStringLiteral("[MGR] initialized: %1").arg(errorCodeName(code));
        });
        QObject::connect(&manager, &HSRadioManager::bandChanged, &app, [&](Band b){
            HSRADIO_LOG_INFO(hsradioManagerTest).noquote() << QStringLiteral("[MGR] bandChanged: %1").arg(bandName(b));
        });
        QObject::connect(&manager, &HSRadioManager::frequencyChanged, &app, [&](int f){
            HSRADIO_LOG_INFO(hsradioManagerTest).noquote() << QStringLiteral("[MGR] frequencyChanged: %1 kHz").arg(f);
        });
        QObject::connect(&manager, &HSRadioManager::signalInfoChanged, &app, [&](const SignalInfo &info){
            HSRADIO_LOG_INFO(hsradioManagerTest).noquote() << QStringLiteral("[MGR] signalInfoChanged: %1").arg(signalText(info));
        });
        QObject::connect(&manager, &HSRadioManager::stateChanged, &app, [&](const HardwareState &s){
            HSRADIO_LOG_INFO(hsradioManagerTest).noquote() << QStringLiteral("[MGR] stateChanged: band=%1 freq=%2 mute=%3")
                .arg(bandName(s.band)).arg(s.frequencyKHz)
                .arg(s.muted ? QStringLiteral("on") : QStringLiteral("off"));
        });
        bool mgrSeekDone = false; bool mgrSeekFound = false; int mgrSeekFinal = 0;
        QObject::connect(&manager, &HSRadioManager::seekStarted, &app, [&](quint64 id, bool up){
            HSRADIO_LOG_INFO(hsradioManagerTest).noquote() << QStringLiteral("[MGR][SEEK] started id=%1 dir=%2")
                .arg(id).arg(up ? QStringLiteral("up") : QStringLiteral("down"));
        });
        QObject::connect(&manager, &HSRadioManager::seekProgress, &app, [&](quint64, int f){
            if (!parser.isSet(noProgressOption))
                HSRADIO_LOG_INFO(hsradioManagerTest).noquote() << QStringLiteral("[MGR][SEEK] scanning %1 kHz").arg(f);
        });
        QObject::connect(&manager, &HSRadioManager::seekFinished, &app, [&](quint64 id, int f, bool found){
            mgrSeekDone = true; mgrSeekFound = found; mgrSeekFinal = f;
            HSRADIO_LOG_INFO(hsradioManagerTest).noquote() << QStringLiteral("[MGR][SEEK] finished id=%1 stop=%2 found=%3")
                .arg(id).arg(f).arg(found ? QStringLiteral("true") : QStringLiteral("false"));
        });
        QObject::connect(&manager, &HSRadioManager::stationsChanged, &app, [&](Band b){
            HSRADIO_LOG_INFO(hsradioManagerTest).noquote() << QStringLiteral("[MGR] stationsChanged: %1 (%2)")
                .arg(bandName(b)).arg(manager.stations(b).size());
        });
        QObject::connect(&manager, &HSRadioManager::favoritesChanged, &app, [&](){
            HSRADIO_LOG_INFO(hsradioManagerTest).noquote() << QStringLiteral("[MGR] favoritesChanged");
        });
        QObject::connect(&manager, &HSRadioManager::errorOccurred, &app, [&](ErrorCode code, const QString &msg){
            HSRADIO_LOG_WARNING(hsradioManagerTest).noquote() << QStringLiteral("[MGR][ERROR] %1: %2").arg(errorCodeName(code), msg);
        });

        const Result initRes = manager.initialize();
        if (!reportResult(QStringLiteral("manager.initialize()"), initRes.code, initRes.message)) {
            anyFailed = true;
        } else {
            auto parseFreqName = [](const QString &text, int *freq, QString *name) -> bool {
                const QStringList parts = text.split(QLatin1Char(':'));
                bool ok = false;
                *freq = parts.value(0).toInt(&ok);
                if (!ok) return false;
                *name = parts.size() > 1 ? parts.value(1) : QString();
                return true;
            };
            if (parser.isSet(managerRestoreOption))
                reportResult(QStringLiteral("manager.restoreLastState()"), manager.restoreLastState().code, QString());
            if (parser.isSet(managerFactoryResetOption))
                reportResult(QStringLiteral("manager.factoryReset()"), manager.factoryReset().code, QString());
            if (parser.isSet(managerSetBandOption)) {
                const QString t = parser.value(managerSetBandOption).trimmed().toUpper();
                if (t == QStringLiteral("FM") || t == QStringLiteral("AM")) {
                    const Band b = t == QStringLiteral("FM") ? Band::FM : Band::AM;
                    runAtom(&probe, manager.setBand(b),
                            QStringLiteral("manager.setBand(%1)").arg(bandName(b)), &anyFailed);
                } else { HSRADIO_LOG_CRITICAL(hsradioManagerTest) << "Invalid --set-band"; anyFailed = true; }
            }
            // 可重复传入：按命令行顺序依次设置，每个频点打印状态与信号。
            for (const QString &text : parser.values(managerSetFrequencyOption)) {
                bool ok = false;
                const int f = text.trimmed().toInt(&ok);
                if (!ok) {
                    HSRADIO_LOG_CRITICAL(hsradioManagerTest) << "Invalid --set-frequency:" << text;
                    anyFailed = true;
                    continue;
                }
                if (!runAtom(&probe, manager.setFrequency(f),
                             QStringLiteral("manager.setFrequency(%1 kHz)").arg(f), &anyFailed))
                    continue;
                HSRADIO_LOG_INFO(hsradioManagerTest).noquote()
                    << QStringLiteral("[MGR] state: band=%1 freq=%2 kHz")
                           .arg(bandName(manager.currentBand()))
                           .arg(manager.currentFrequencyKHz());
                runAtom(&probe, manager.refreshSignal(),
                        QStringLiteral("manager.refreshSignal(%1 kHz)").arg(f), &anyFailed);
            }
            if (parser.isSet(managerStepUpOption))
                runAtom(&probe, manager.stepUp(), QStringLiteral("manager.stepUp()"), &anyFailed);
            if (parser.isSet(managerStepDownOption))
                runAtom(&probe, manager.stepDown(), QStringLiteral("manager.stepDown()"), &anyFailed);
            if (parser.isSet(managerSetMuteOption)) {
                const QString t = parser.value(managerSetMuteOption).trimmed().toLower();
                if (t == QStringLiteral("on") || t == QStringLiteral("off")) {
                    const bool muted = t == QStringLiteral("on");
                    runAtom(&probe, manager.setMute(muted),
                            QStringLiteral("manager.setMute(%1)").arg(muted ? QStringLiteral("on")
                                                                            : QStringLiteral("off")),
                            &anyFailed);
                } else { HSRADIO_LOG_CRITICAL(hsradioManagerTest) << "Invalid --set-mute"; anyFailed = true; }
            }
            if (parser.isSet(managerRefreshOption))
                runAtom(&probe, manager.refreshSignal(), QStringLiteral("manager.refreshSignal()"),
                        &anyFailed);
            for (const QString &t : parser.values(managerAddStationOption)) {
                int f = 0; QString n;
                if (parseFreqName(t, &f, &n)) {
                    Station s; s.band = manager.currentBand(); s.frequencyKHz = f;
                    s.name = n; s.source = QStringLiteral("manual");
                    const Result r = manager.addStation(s);
                    reportResult(QStringLiteral("manager.addStation(%1)").arg(f), r.code, r.message);
                } else { HSRADIO_LOG_CRITICAL(hsradioManagerTest) << "Invalid --add-station:" << t; anyFailed = true; }
            }
            for (const QString &t : parser.values(managerAddFavOption)) {
                int f = 0; QString n;
                if (parseFreqName(t, &f, &n)) {
                    Station s; s.band = manager.currentBand(); s.frequencyKHz = f;
                    s.name = n; s.source = QStringLiteral("manual");
                    const Result r = manager.addFavorite(s);
                    reportResult(QStringLiteral("manager.addFavorite(%1)").arg(f), r.code, r.message);
                } else { HSRADIO_LOG_CRITICAL(hsradioManagerTest) << "Invalid --add-fav:" << t; anyFailed = true; }
            }
            if (parser.isSet(managerRemoveStationOption)) {
                const QString id = parser.value(managerRemoveStationOption);
                const Result r = manager.removeStation(id);
                reportResult(QStringLiteral("manager.removeStation(%1)").arg(id), r.code, r.message);
            }
            if (parser.isSet(managerRemoveFavOption)) {
                const QString id = parser.value(managerRemoveFavOption);
                const Result r = manager.removeFavorite(id);
                reportResult(QStringLiteral("manager.removeFavorite(%1)").arg(id), r.code, r.message);
            }
            if (parser.isSet(managerListStationsOption)) {
                HSRADIO_LOG_INFO(hsradioManagerTest).noquote() << QStringLiteral("[MGR] stations(%1):")
                    .arg(bandName(manager.currentBand()));
                for (const Station &s : manager.stations(manager.currentBand()))
                    HSRADIO_LOG_INFO(hsradioManagerTest).noquote() << QStringLiteral("   - %1 %2 kHz name=%3 source=%4")
                        .arg(s.id).arg(s.frequencyKHz).arg(s.name).arg(s.source);
            }
            if (parser.isSet(managerListFavsOption)) {
                HSRADIO_LOG_INFO(hsradioManagerTest).noquote() << QStringLiteral("[MGR] favorites(%1):")
                    .arg(bandName(manager.currentBand()));
                for (const Station &s : manager.favorites(manager.currentBand()))
                    HSRADIO_LOG_INFO(hsradioManagerTest).noquote() << QStringLiteral("   - %1 %2 kHz name=%3 source=%4")
                        .arg(s.id).arg(s.frequencyKHz).arg(s.name).arg(s.source);
            }
            if (parser.isSet(managerPrintStateOption)) {
                HSRADIO_LOG_INFO(hsradioManagerTest).noquote() << QStringLiteral("[MGR] region=%1 band=%2 freq=%3")
                    .arg(repository.region())
                    .arg(bandName(manager.currentBand()))
                    .arg(manager.currentFrequencyKHz());
                HSRADIO_LOG_INFO(hsradioManagerTest).noquote() << QStringLiteral("[MGR] stations FM/AM=%1/%2 favorites FM/AM=%3/%4")
                    .arg(manager.stations(Band::FM).size())
                    .arg(manager.stations(Band::AM).size())
                    .arg(manager.favorites(Band::FM).size())
                    .arg(manager.favorites(Band::AM).size());
            }
            if (parser.isSet(managerSeekOption)) {
                const bool up = !parser.isSet(managerSeekDownOption);
                const SeekMode mode = (parser.value(managerSeekModeOption).trimmed().toLower()
                    == QStringLiteral("next")) ? SeekMode::NextStation : SeekMode::FullBand;
                QElapsedTimer mgrSeekTimer;
                mgrSeekTimer.start();
                manager.startSeek(up, mode);
                int cancelAfter = 0;
                if (parser.isSet(managerSeekCancelAfterOption))
                    parseIntegerOption(parser, managerSeekCancelAfterOption, &cancelAfter);
                if (cancelAfter > 0)
                    QTimer::singleShot(cancelAfter, &app, [&](){
                        HSRADIO_LOG_INFO(hsradioManagerTest).noquote() << QStringLiteral("[MGR][SEEK] stopSeek() requested");
                        manager.stopSeek();
                    });
                if (waitUntil([&](){ return mgrSeekDone; }, 120000)) {
                    const qint64 elapsedMs = mgrSeekTimer.elapsed();
                    HSRADIO_LOG_INFO(hsradioManagerTest).noquote()
                        << QStringLiteral("[MGR][SEEK] summary: stations=%1 found=%2 stop=%3 elapsed=%4 s (%5 ms)")
                            .arg(manager.stations(manager.currentBand()).size())
                            .arg(mgrSeekFound ? QStringLiteral("true") : QStringLiteral("false"))
                            .arg(mgrSeekFinal)
                            .arg(elapsedMs / 1000.0, 0, 'f', 2)
                            .arg(elapsedMs);
                } else {
                    HSRADIO_LOG_CRITICAL(hsradioManagerTest).noquote() << "[FAIL] manager.startSeek | timed out";
                    anyFailed = true;
                }
            }
        }
        manager.shutdown();
        // 只有文件确实落盘才算保存成功，避免路径不可写时误报。
        const bool stateSaved = QFile::exists(stateFile);
        const bool configSaved = QFile::exists(configFile);
        if (stateSaved && configSaved) {
            HSRADIO_LOG_INFO(hsradioManagerTest).noquote() << QStringLiteral("[MGR] state saved -> %1, config saved -> %2")
                .arg(stateFile).arg(configFile);
        } else {
            HSRADIO_LOG_CRITICAL(hsradioManagerTest).noquote()
                << QStringLiteral("[FAIL] state/config save: state=%1 config=%2")
                    .arg(stateSaved ? QStringLiteral("ok") : QStringLiteral("missing"))
                    .arg(configSaved ? QStringLiteral("ok") : QStringLiteral("missing"));
            anyFailed = true;
        }
    }


    scheduler.shutdown();
    return anyFailed ? 1 : 0;
}
