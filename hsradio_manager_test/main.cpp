// HSRadioManagerTest —— 调度器 / 业务层接口调试工具。
//
// 与 HSRadioHardwareTest 同一形态：所有操作由命令行参数点名触发，
// 每一步打印 [PASS]/[FAIL] 与回读值，不做写死的断言、不自动跑用例。
// 后端是 MockRadioHardware，因此不需要 ALSA、不需要 i.MX9 真机。
//
//   ./hsradio_manager_test --help
//   ./hsradio_manager_test --set-band FM --set-frequency 87500
//   ./hsradio_manager_test --seek --seek-lock-ms 0 --seek-sample-ms 0

#include "hsradio/HSRadioTypes.h"
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
        qInfo().noquote() << "[PASS]" << operation;
        return true;
    }
    qCritical().noquote()
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
    QHash<quint64, HardwareState> states;
    QHash<quint64, SignalInfo> signalInfos;
    QString lastError;

    bool seekDone = false;
    bool seekFound = false;
    int seekFinalFrequency = 0;
    int progressCount = 0;
    int hitCount = 0;
};

bool runAtom(Probe *probe, quint64 id, const QString &label, bool *anyFailed)
{
    if (id == 0) {
        qCritical().noquote() << "[FAIL]" << label << "| scheduler not ready";
        *anyFailed = true;
        return false;
    }
    probe->lastError.clear();
    if (!waitUntil([&] { return probe->codes.contains(id); })) {
        qCritical().noquote() << "[FAIL]" << label << "| timed out waiting for operationFinished";
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
        QStringLiteral("HSRadio scheduler/manager interface debug tool. Default backend is MockRadioHardware; use --saf775x to select the real SAF775X ALSA backend (ALSA build only)."));
    parser.addHelpOption();
    parser.addVersionOption();


    // —— 真实 SAF775X ALSA 后端选项（仅当以 ALSA 方式编译时可用）——
    const QCommandLineOption saf775xOption(
        QStringLiteral("saf775x"),
        QStringLiteral("Use the real SAF775X ALSA backend instead of Mock (requires ALSA build)."));

    // —— 原子命令 ——
    const QCommandLineOption setBandOption(
        QStringLiteral("set-band"),
        QStringLiteral("submitSetBand: set band to FM or AM."),
        QStringLiteral("FM|AM"));
    const QCommandLineOption setFrequencyOption(
        QStringLiteral("set-frequency"),
        QStringLiteral("submitTune: tune to a frequency in kHz."),
        QStringLiteral("kHz"));
    const QCommandLineOption searchOption(
        QStringLiteral("search"),
        QStringLiteral("Use Search (stay-muted) tune mode with --set-frequency (default: Preset)."));
    const QCommandLineOption setMuteOption(
        QStringLiteral("set-mute"),
        QStringLiteral("submitSetMute: on or off."),
        QStringLiteral("on|off"));
    const QCommandLineOption noReadOption(
        QStringLiteral("no-read"),
        QStringLiteral("Skip the automatic submitReadState / submitReadSignal."));

    // —— 搜台 ——
    const QCommandLineOption seekOption(
        QStringLiteral("seek"),
        QStringLiteral("submitSeek: run a station search with the --seek-* parameters."));
    const QCommandLineOption seekBandOption(
        QStringLiteral("seek-band"),
        QStringLiteral("Band for seek (default: FM)."),
        QStringLiteral("FM|AM"),
        QStringLiteral("FM"));
    const QCommandLineOption seekFromOption(
        QStringLiteral("seek-from"),
        QStringLiteral("Seek start frequency in kHz (default: 87500)."),
        QStringLiteral("kHz"),
        QStringLiteral("87500"));
    const QCommandLineOption seekToOption(
        QStringLiteral("seek-to"),
        QStringLiteral("Seek end frequency in kHz (default: 108000)."),
        QStringLiteral("kHz"),
        QStringLiteral("108000"));
    const QCommandLineOption seekStepOption(
        QStringLiteral("seek-step"),
        QStringLiteral("Seek step in kHz (default: 100)."),
        QStringLiteral("kHz"),
        QStringLiteral("100"));
    const QCommandLineOption seekModeOption(
        QStringLiteral("seek-mode"),
        QStringLiteral("full = scan the whole band, next = stop at the next valid station (default: full)."),
        QStringLiteral("full|next"),
        QStringLiteral("full"));
    const QCommandLineOption seekDownOption(
        QStringLiteral("seek-down"),
        QStringLiteral("Scan downwards (default: upwards)."));
    const QCommandLineOption seekNoMuteOption(
        QStringLiteral("seek-no-mute"),
        QStringLiteral("Do not force mute during seek (default: force mute, restore afterwards)."));
    const QCommandLineOption seekNoWrapOption(
        QStringLiteral("seek-no-wrap"),
        QStringLiteral("Stop at the band edge instead of wrapping around."));
    const QCommandLineOption seekMinRssiOption(
        QStringLiteral("seek-min-rssi"),
        QStringLiteral("Minimum RSSI to accept a station (default: 21)."),
        QStringLiteral("value"),
        QStringLiteral("21"));
    const QCommandLineOption seekMaxUsnOption(
        QStringLiteral("seek-max-usn"),
        QStringLiteral("Maximum USN to accept a station (default: 25)."),
        QStringLiteral("value"),
        QStringLiteral("25"));
    const QCommandLineOption seekMaxWamOption(
        QStringLiteral("seek-max-wam"),
        QStringLiteral("Maximum WAM to accept a station (default: 25)."),
        QStringLiteral("value"),
        QStringLiteral("25"));
    const QCommandLineOption seekMaxOffsetOption(
        QStringLiteral("seek-max-offset"),
        QStringLiteral("Maximum OFFSET to accept a station (default: 40)."),
        QStringLiteral("value"),
        QStringLiteral("40"));
    const QCommandLineOption seekLockMsOption(
        QStringLiteral("seek-lock-ms"),
        QStringLiteral("PLL lock delay per step in ms (default: 8; use 0 for fast debugging)."),
        QStringLiteral("ms"),
        QStringLiteral("8"));
    const QCommandLineOption seekSampleMsOption(
        QStringLiteral("seek-sample-ms"),
        QStringLiteral("Delay between samples in ms (default: 2)."),
        QStringLiteral("ms"),
        QStringLiteral("2"));
    const QCommandLineOption seekSamplesOption(
        QStringLiteral("seek-samples"),
        QStringLiteral("Number of signal samples per step (default: 4)."),
        QStringLiteral("count"),
        QStringLiteral("4"));
    const QCommandLineOption seekMaxInvalidOption(
        QStringLiteral("seek-max-invalid"),
        QStringLiteral("Give up after this many invalid samples (default: 3)."),
        QStringLiteral("count"),
        QStringLiteral("3"));
    const QCommandLineOption seekCancelAfterOption(
        QStringLiteral("seek-cancel-after"),
        QStringLiteral("Call cancelSeek() N ms after the seek starts (debug the cancellation path)."),
        QStringLiteral("ms"));
    const QCommandLineOption noProgressOption(
        QStringLiteral("no-progress"),
        QStringLiteral("Do not print per-step seek progress."));

    parser.addOption(saf775xOption);
    parser.addOption(setBandOption);
    parser.addOption(setFrequencyOption);
    parser.addOption(searchOption);
    parser.addOption(setMuteOption);
    parser.addOption(noReadOption);
    parser.addOption(seekOption);
    parser.addOption(seekBandOption);
    parser.addOption(seekFromOption);
    parser.addOption(seekToOption);
    parser.addOption(seekStepOption);
    parser.addOption(seekModeOption);
    parser.addOption(seekDownOption);
    parser.addOption(seekNoMuteOption);
    parser.addOption(seekNoWrapOption);
    parser.addOption(seekMinRssiOption);
    parser.addOption(seekMaxUsnOption);
    parser.addOption(seekMaxWamOption);
    parser.addOption(seekMaxOffsetOption);
    parser.addOption(seekLockMsOption);
    parser.addOption(seekSampleMsOption);
    parser.addOption(seekSamplesOption);
    parser.addOption(seekMaxInvalidOption);
    parser.addOption(seekCancelAfterOption);
    parser.addOption(noProgressOption);
    const QCommandLineOption managerModeOption(
        QStringLiteral("manager"),
        QStringLiteral("启用管理器模式：以 HSRadioManager 为门面执行业务接口（--manager-* 才生效）。"));
    const QCommandLineOption managerStateFileOption(
        QStringLiteral("manager-state-file"),
        QStringLiteral("状态文件路径（默认 radio_state.json，位于当前目录）。"),
        QStringLiteral("path"),
        QStringLiteral("radio_state.json"));
    const QCommandLineOption managerConfigFileOption(
        QStringLiteral("manager-config-file"),
        QStringLiteral("配置文件路径（默认 radio_config.json，位于当前目录）。"),
        QStringLiteral("path"),
        QStringLiteral("radio_config.json"));
    const QCommandLineOption managerSetBandOption(
        QStringLiteral("manager-set-band"),
        QStringLiteral("manager.setBand：FM 或 AM。"),
        QStringLiteral("FM|AM"));
    const QCommandLineOption managerSetFrequencyOption(
        QStringLiteral("manager-set-frequency"),
        QStringLiteral("manager.setFrequency：kHz。"),
        QStringLiteral("kHz"));
    const QCommandLineOption managerStepUpOption(
        QStringLiteral("manager-step-up"),
        QStringLiteral("manager.stepUp()。"));
    const QCommandLineOption managerStepDownOption(
        QStringLiteral("manager-step-down"),
        QStringLiteral("manager.stepDown()。"));
    const QCommandLineOption managerSetMuteOption(
        QStringLiteral("manager-set-mute"),
        QStringLiteral("manager.setMute：on 或 off。"),
        QStringLiteral("on|off"));
    const QCommandLineOption managerRefreshOption(
        QStringLiteral("manager-refresh"),
        QStringLiteral("manager.refreshSignal()。"));
    const QCommandLineOption managerSeekOption(
        QStringLiteral("manager-seek"),
        QStringLiteral("manager.startSeek()：用配置里的频段范围与阈值扫描。"));
    const QCommandLineOption managerSeekDownOption(
        QStringLiteral("manager-seek-down"),
        QStringLiteral("向下搜台（默认向上）。"));
    const QCommandLineOption managerSeekModeOption(
        QStringLiteral("manager-seek-mode"),
        QStringLiteral("full=全频段重建，next=找下一个有效台（默认 full）。"),
        QStringLiteral("full|next"),
        QStringLiteral("full"));
    const QCommandLineOption managerSeekCancelAfterOption(
        QStringLiteral("manager-seek-cancel-after"),
        QStringLiteral("N ms 后调用 stopSeek() 调试取消路径。"),
        QStringLiteral("ms"));
    const QCommandLineOption managerAddStationOption(
        QStringLiteral("manager-add-station"),
        QStringLiteral("添加电台 kHz[:name]（可重复）。"),
        QStringLiteral("spec"));
    const QCommandLineOption managerAddFavOption(
        QStringLiteral("manager-add-fav"),
        QStringLiteral("添加收藏 kHz[:name]（可重复）。"),
        QStringLiteral("spec"));
    const QCommandLineOption managerRemoveStationOption(
        QStringLiteral("manager-remove-station"),
        QStringLiteral("删除电台（按 id）。"),
        QStringLiteral("id"));
    const QCommandLineOption managerRemoveFavOption(
        QStringLiteral("manager-remove-fav"),
        QStringLiteral("删除收藏（按 id）。"),
        QStringLiteral("id"));
    const QCommandLineOption managerListStationsOption(
        QStringLiteral("manager-list-stations"),
        QStringLiteral("列出当前频段电台。"));
    const QCommandLineOption managerListFavsOption(
        QStringLiteral("manager-list-favs"),
        QStringLiteral("列出当前频段收藏。"));
    const QCommandLineOption managerRestoreOption(
        QStringLiteral("manager-restore"),
        QStringLiteral("manager.restoreLastState()。"));
    const QCommandLineOption managerFactoryResetOption(
        QStringLiteral("manager-factory-reset"),
        QStringLiteral("manager.factoryReset()：状态与配置回默认。"));
    const QCommandLineOption managerPrintStateOption(
        QStringLiteral("manager-print-state"),
        QStringLiteral("打印存储与配置状态摘要。"));

    parser.addOption(managerModeOption);
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


    // —— 组装硬件后端（默认 Mock；--saf775x 选用真实 SAF775X ALSA 后端）——
    std::unique_ptr<IRadioHardware> hardware;
    QString backendLabel;

    bool wantSaf775x = false;

#if defined(HSRADIO_ENABLE_ALSA)
    wantSaf775x = parser.isSet(saf775xOption);
#else
    if (parser.isSet(saf775xOption)) {
        qCritical().noquote() << "[FAIL] --saf775x requested but this build has no ALSA backend (HSRADIO_ENABLE_ALSA off).";
        return 2;
    }
#endif

    if (!wantSaf775x) {
        backendLabel = QStringLiteral("initialize(MockRadioHardware)");
        hardware.reset(new MockRadioHardware);
        qInfo().noquote() << QStringLiteral("Backend: MockRadioHardware (default)");
    } else {
#if defined(HSRADIO_ENABLE_ALSA)
        HSRadioSAF775XConfig safConfig;   // 全部走默认值
        hardware.reset(new HSRadioSAF775XBackend(safConfig));
        backendLabel = QStringLiteral("initialize(HSRadioSAF775XBackend)");
        qInfo().noquote()
            << QStringLiteral("Backend: HSRadioSAF775XBackend (ALSA, defaults)");
#else
        qCritical().noquote() << "[FAIL] --saf775x requested but this build has no ALSA backend.";
        return 2;
#endif
    }


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
                         qWarning().noquote() << "[ERROR]" << probe.lastError;
                     });
    QObject::connect(&scheduler, &HSRadioOperationScheduler::stateRead, &app,
                     [&](quint64 id, const HardwareState &state) { probe.states.insert(id, state); });
    QObject::connect(&scheduler, &HSRadioOperationScheduler::signalRead, &app,
                     [&](quint64 id, const SignalInfo &info) { probe.signalInfos.insert(id, info); });
    QObject::connect(&scheduler, &HSRadioOperationScheduler::seekStarted, &app,
                     [&](quint64 id) {
                         qInfo().noquote() << QStringLiteral("[SEEK] started (requestId=%1)").arg(id);
                     });
    QObject::connect(&scheduler, &HSRadioOperationScheduler::seekHit, &app,
                     [&](quint64, int frequencyKHz, const SignalInfo &info) {
                         ++probe.hitCount;
                         qInfo().noquote()
                             << QStringLiteral("[FOUND] %1 kHz | %2")
                                    .arg(frequencyKHz)
                                    .arg(signalText(info));
                     });
    QObject::connect(&scheduler, &HSRadioOperationScheduler::seekProgress, &app,
                     [&](quint64, int frequencyKHz) {
                         ++probe.progressCount;
                         if (!parser.isSet(noProgressOption))
                             qInfo().noquote() << QStringLiteral("[SEEK] scanning %1 kHz").arg(frequencyKHz);
                     });
    QObject::connect(&scheduler, &HSRadioOperationScheduler::seekFinished, &app,
                     [&](quint64 id, int frequencyKHz, bool found) {
                         probe.seekDone = true;
                         probe.seekFound = found;
                         probe.seekFinalFrequency = frequencyKHz;
                         qInfo().noquote()
                             << QStringLiteral("[SEEK] finished (requestId=%1): stop at %2 kHz, found=%3")
                                    .arg(id)
                                    .arg(frequencyKHz)
                                    .arg(found ? QStringLiteral("true") : QStringLiteral("false"));
                     });

    const Result initResult = scheduler.initialize(std::move(hardware));
    if (!initResult.ok()) {
        qCritical().noquote() << "[FAIL] initialize |" << initResult.message;
        return 2;
    }
    if (!waitUntil([&] { return initialized; })) {
        qCritical().noquote() << "[FAIL] initialize | timed out waiting for initialized signal";
        return 1;
    }
    if (!reportResult(backendLabel, initCode, QString())) {
        scheduler.shutdown();
        return 1;
    }

    bool anyFailed = false;

    // —— 原子命令：按参数点名触发 ——
    if (parser.isSet(setBandOption)) {
        const QString text = parser.value(setBandOption).trimmed().toUpper();
        if (text == QStringLiteral("FM") || text == QStringLiteral("AM")) {
            const Band band = text == QStringLiteral("FM") ? Band::FM : Band::AM;
            runAtom(&probe, scheduler.submitSetBand(band),
                    QStringLiteral("submitSetBand(%1)").arg(bandName(band)), &anyFailed);
        } else {
            qCritical() << "Invalid --set-band value; use FM or AM.";
            anyFailed = true;
        }
    }

    if (parser.isSet(setFrequencyOption)) {
        int frequencyKHz = 0;
        if (!parseIntegerOption(parser, setFrequencyOption, &frequencyKHz)) {
            qCritical() << "Invalid --set-frequency value.";
            anyFailed = true;
        } else {
            const bool inSearchMode = parser.isSet(searchOption);
            const IRadioHardware::TuneMode mode = inSearchMode
                ? IRadioHardware::TuneMode::Search
                : IRadioHardware::TuneMode::Preset;
            runAtom(&probe, scheduler.submitTune(frequencyKHz, mode),
                    QStringLiteral("submitTune(%1 kHz, %2)")
                        .arg(frequencyKHz)
                        .arg(inSearchMode ? QStringLiteral("Search")
                                          : QStringLiteral("Preset")),
                    &anyFailed);
        }
    }

    if (parser.isSet(setMuteOption)) {
        const QString text = parser.value(setMuteOption).trimmed().toLower();
        if (text == QStringLiteral("on") || text == QStringLiteral("off")) {
            const bool muted = text == QStringLiteral("on");
            runAtom(&probe, scheduler.submitSetMute(muted),
                    QStringLiteral("submitSetMute(%1)").arg(muted ? QStringLiteral("on")
                                                                  : QStringLiteral("off")),
                    &anyFailed);
        } else {
            qCritical() << "Invalid --set-mute value; use on or off.";
            anyFailed = true;
        }
    }

    // —— 回读：默认读一次状态与信号（读到的是上面所有命令之后的结果）——
    if (!parser.isSet(noReadOption)) {
        const quint64 stateId = scheduler.submitReadState();
        if (runAtom(&probe, stateId, QStringLiteral("submitReadState"), &anyFailed)) {
            const HardwareState state = probe.states.value(stateId);
            qInfo().noquote()
                << QStringLiteral("  state: band=%1, frequency=%2 kHz, mute=%3")
                       .arg(bandName(state.band))
                       .arg(state.frequencyKHz)
                       .arg(state.muted ? QStringLiteral("on") : QStringLiteral("off"));
        }

        const quint64 signalId = scheduler.submitReadSignal();
        if (runAtom(&probe, signalId, QStringLiteral("submitReadSignal"), &anyFailed))
            qInfo().noquote()
                << QStringLiteral("  signal: %1").arg(signalText(probe.signalInfos.value(signalId)));
    }

    // —— 搜台 ——
    if (parser.isSet(seekOption)) {
        SeekRequest request;
        request.muteDuringSeek = !parser.isSet(seekNoMuteOption);
        request.upward = !parser.isSet(seekDownOption);
        request.wrapAround = !parser.isSet(seekNoWrapOption);

        const QString bandText = parser.value(seekBandOption).trimmed().toUpper();
        if (bandText == QStringLiteral("AM"))
            request.band = Band::AM;
        else if (bandText == QStringLiteral("FM"))
            request.band = Band::FM;
        else {
            qCritical() << "Invalid --seek-band value; use FM or AM.";
            scheduler.shutdown();
            return 2;
        }

        const QString modeText = parser.value(seekModeOption).trimmed().toLower();
        if (modeText == QStringLiteral("next"))
            request.mode = SeekMode::NextStation;
        else if (modeText == QStringLiteral("full"))
            request.mode = SeekMode::FullBand;
        else {
            qCritical() << "Invalid --seek-mode value; use full or next.";
            scheduler.shutdown();
            return 2;
        }

        int seekToKHz = 0;
        if (!parseIntegerOption(parser, seekFromOption, &request.startKHz)
            || !parseIntegerOption(parser, seekToOption, &seekToKHz)
            || !parseIntegerOption(parser, seekStepOption, &request.stepKHz)
            || !parseIntegerOption(parser, seekMinRssiOption, &request.threshold.minRssi)
            || !parseIntegerOption(parser, seekMaxUsnOption, &request.threshold.maxUsn)
            || !parseIntegerOption(parser, seekMaxWamOption, &request.threshold.maxWam)
            || !parseIntegerOption(parser, seekMaxOffsetOption, &request.threshold.maxOffset)
            || !parseIntegerOption(parser, seekLockMsOption, &request.lockMs)
            || !parseIntegerOption(parser, seekSampleMsOption, &request.sampleMs)
            || !parseIntegerOption(parser, seekSamplesOption, &request.sampleCount)
            || !parseIntegerOption(parser, seekMaxInvalidOption, &request.maxInvalid)) {
            qCritical() << "Invalid numeric --seek-* value.";
            scheduler.shutdown();
            return 2;
        }

        // 调试工具没有独立的频段配置，直接用 from/to 推出扫描边界。
        request.minKHz = qMin(request.startKHz, seekToKHz);
        request.maxKHz = qMax(request.startKHz, seekToKHz);

        int cancelAfterMs = 0;
        if (parser.isSet(seekCancelAfterOption)
            && !parseIntegerOption(parser, seekCancelAfterOption, &cancelAfterMs)) {
            qCritical() << "Invalid --seek-cancel-after value.";
            scheduler.shutdown();
            return 2;
        }

        qInfo().noquote()
            << QStringLiteral("[SEEK] band=%1 mode=%2 dir=%3 range=[%4,%5] step=%6")
                   .arg(bandName(request.band))
                   .arg(request.mode == SeekMode::FullBand ? QStringLiteral("full")
                                                           : QStringLiteral("next"))
                   .arg(request.upward ? QStringLiteral("up") : QStringLiteral("down"))
                   .arg(request.minKHz).arg(request.maxKHz).arg(request.stepKHz);
        qInfo().noquote()
            << QStringLiteral("[SEEK] thresholds rssi>=%1 usn<=%2 wam<=%3 offset<=%4 | "
                              "timing lock=%5ms sample=%6ms x%7 maxInvalid=%8")
                   .arg(request.threshold.minRssi).arg(request.threshold.maxUsn)
                   .arg(request.threshold.maxWam).arg(request.threshold.maxOffset)
                   .arg(request.lockMs).arg(request.sampleMs)
                   .arg(request.sampleCount).arg(request.maxInvalid);
        qInfo().noquote()
            << QStringLiteral("[SEEK] muteDuringSeek=%1 wrapAround=%2 cancelAfter=%3ms")
                   .arg(request.muteDuringSeek ? QStringLiteral("on") : QStringLiteral("off"))
                   .arg(request.wrapAround ? QStringLiteral("on") : QStringLiteral("off"))
                   .arg(cancelAfterMs);

        QElapsedTimer timer;
        timer.start();
        const quint64 seekId = scheduler.submitSeek(request);
        if (seekId == 0) {
            qCritical().noquote() << "[FAIL] submitSeek | scheduler not ready";
            anyFailed = true;
        } else {
            if (cancelAfterMs > 0) {
                QTimer::singleShot(cancelAfterMs, &app, [&]() {
                    qInfo().noquote() << "[SEEK] cancelSeek() requested";
                    scheduler.cancelSeek();
                });
            }
            if (!waitUntil([&] { return probe.seekDone; }, 120000)) {
                qCritical().noquote() << "[FAIL] submitSeek | timed out waiting for seekFinished";
                anyFailed = true;
            } else {
                waitUntil([&] { return probe.codes.contains(seekId); }, 3000);
                const ErrorCode code = probe.codes.value(seekId, ErrorCode::None);
                qInfo().noquote()
                    << QStringLiteral("[SEEK] summary: %1 station(s), stop=%2 kHz, found=%3, "
                                      "progress=%4, result=%5, elapsed=%6 s")
                           .arg(probe.hitCount)
                           .arg(probe.seekFinalFrequency)
                           .arg(probe.seekFound ? QStringLiteral("true") : QStringLiteral("false"))
                           .arg(probe.progressCount)
                           .arg(errorCodeName(code))
                           .arg(timer.elapsed() / 1000.0, 0, 'f', 2);
                if (code != ErrorCode::None)
                    anyFailed = true;
            }
        }
    }

    // —— 管理器模式：以 HSRadioManager 为门面执行业务接口 ——
    if (parser.isSet(managerModeOption)
        || parser.isSet(managerSetBandOption) || parser.isSet(managerSetFrequencyOption)
        || parser.isSet(managerStepUpOption) || parser.isSet(managerStepDownOption)
        || parser.isSet(managerSetMuteOption) || parser.isSet(managerRefreshOption)
        || parser.isSet(managerSeekOption) || parser.isSet(managerAddStationOption)
        || parser.isSet(managerAddFavOption) || parser.isSet(managerRemoveStationOption)
        || parser.isSet(managerRemoveFavOption) || parser.isSet(managerListStationsOption)
        || parser.isSet(managerListFavsOption) || parser.isSet(managerRestoreOption)
        || parser.isSet(managerFactoryResetOption) || parser.isSet(managerPrintStateOption)) {
        const QString stateFile = parser.isSet(managerStateFileOption)
            ? parser.value(managerStateFileOption) : QStringLiteral("radio_state.json");
        const QString configFile = parser.isSet(managerConfigFileOption)
            ? parser.value(managerConfigFileOption) : QStringLiteral("radio_config.json");
        HSRadioStationStore store(stateFile);
        HSRadioRepository repository(configFile);
        HSRadioManager manager(&scheduler, &store, &repository);

        QObject::connect(&manager, &HSRadioManager::initialized, &app, [&](ErrorCode code){
            qInfo().noquote() << QStringLiteral("[MGR] initialized: %1").arg(errorCodeName(code));
        });
        QObject::connect(&manager, &HSRadioManager::bandChanged, &app, [&](Band b){
            qInfo().noquote() << QStringLiteral("[MGR] bandChanged: %1").arg(bandName(b));
        });
        QObject::connect(&manager, &HSRadioManager::frequencyChanged, &app, [&](int f){
            qInfo().noquote() << QStringLiteral("[MGR] frequencyChanged: %1 kHz").arg(f);
        });
        QObject::connect(&manager, &HSRadioManager::signalInfoChanged, &app, [&](const SignalInfo &info){
            qInfo().noquote() << QStringLiteral("[MGR] signalInfoChanged: %1").arg(signalText(info));
        });
        QObject::connect(&manager, &HSRadioManager::stateChanged, &app, [&](const HardwareState &s){
            qInfo().noquote() << QStringLiteral("[MGR] stateChanged: band=%1 freq=%2 mute=%3")
                .arg(bandName(s.band)).arg(s.frequencyKHz)
                .arg(s.muted ? QStringLiteral("on") : QStringLiteral("off"));
        });
        bool mgrSeekDone = false; bool mgrSeekFound = false; int mgrSeekFinal = 0;
        QObject::connect(&manager, &HSRadioManager::seekStarted, &app, [&](quint64 id, bool up){
            qInfo().noquote() << QStringLiteral("[MGR][SEEK] started id=%1 dir=%2")
                .arg(id).arg(up ? QStringLiteral("up") : QStringLiteral("down"));
        });
        QObject::connect(&manager, &HSRadioManager::seekProgress, &app, [&](quint64, int f){
            if (!parser.isSet(noProgressOption))
                qInfo().noquote() << QStringLiteral("[MGR][SEEK] scanning %1 kHz").arg(f);
        });
        QObject::connect(&manager, &HSRadioManager::seekFinished, &app, [&](quint64 id, int f, bool found){
            mgrSeekDone = true; mgrSeekFound = found; mgrSeekFinal = f;
            qInfo().noquote() << QStringLiteral("[MGR][SEEK] finished id=%1 stop=%2 found=%3")
                .arg(id).arg(f).arg(found ? QStringLiteral("true") : QStringLiteral("false"));
        });
        QObject::connect(&manager, &HSRadioManager::stationsChanged, &app, [&](Band b){
            qInfo().noquote() << QStringLiteral("[MGR] stationsChanged: %1 (%2)")
                .arg(bandName(b)).arg(manager.stations(b).size());
        });
        QObject::connect(&manager, &HSRadioManager::favoritesChanged, &app, [&](){
            qInfo().noquote() << QStringLiteral("[MGR] favoritesChanged");
        });
        QObject::connect(&manager, &HSRadioManager::errorOccurred, &app, [&](ErrorCode code, const QString &msg){
            qWarning().noquote() << QStringLiteral("[MGR][ERROR] %1: %2").arg(errorCodeName(code), msg);
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
                if (t == QStringLiteral("FM") || t == QStringLiteral("AM"))
                    manager.setBand(t == QStringLiteral("FM") ? Band::FM : Band::AM);
                else { qCritical() << "Invalid --manager-set-band"; anyFailed = true; }
            }
            if (parser.isSet(managerSetFrequencyOption)) {
                int f = 0;
                if (parseIntegerOption(parser, managerSetFrequencyOption, &f))
                    manager.setFrequency(f);
                else { qCritical() << "Invalid --manager-set-frequency"; anyFailed = true; }
            }
            if (parser.isSet(managerStepUpOption)) manager.stepUp();
            if (parser.isSet(managerStepDownOption)) manager.stepDown();
            if (parser.isSet(managerSetMuteOption)) {
                const QString t = parser.value(managerSetMuteOption).trimmed().toLower();
                if (t == QStringLiteral("on") || t == QStringLiteral("off"))
                    manager.setMute(t == QStringLiteral("on"));
                else { qCritical() << "Invalid --manager-set-mute"; anyFailed = true; }
            }
            if (parser.isSet(managerRefreshOption)) manager.refreshSignal();
            for (const QString &t : parser.values(managerAddStationOption)) {
                int f = 0; QString n;
                if (parseFreqName(t, &f, &n)) {
                    Station s; s.band = manager.currentBand(); s.frequencyKHz = f;
                    s.name = n; s.source = QStringLiteral("manual");
                    const Result r = manager.addStation(s);
                    reportResult(QStringLiteral("manager.addStation(%1)").arg(f), r.code, r.message);
                } else { qCritical() << "Invalid --manager-add-station:" << t; anyFailed = true; }
            }
            for (const QString &t : parser.values(managerAddFavOption)) {
                int f = 0; QString n;
                if (parseFreqName(t, &f, &n)) {
                    Station s; s.band = manager.currentBand(); s.frequencyKHz = f;
                    s.name = n; s.source = QStringLiteral("manual");
                    const Result r = manager.addFavorite(s);
                    reportResult(QStringLiteral("manager.addFavorite(%1)").arg(f), r.code, r.message);
                } else { qCritical() << "Invalid --manager-add-fav:" << t; anyFailed = true; }
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
                qInfo().noquote() << QStringLiteral("[MGR] stations(%1):")
                    .arg(bandName(manager.currentBand()));
                for (const Station &s : manager.stations(manager.currentBand()))
                    qInfo().noquote() << QStringLiteral("   - %1 %2 kHz name=%3 source=%4")
                        .arg(s.id).arg(s.frequencyKHz).arg(s.name).arg(s.source);
            }
            if (parser.isSet(managerListFavsOption)) {
                qInfo().noquote() << QStringLiteral("[MGR] favorites(%1):")
                    .arg(bandName(manager.currentBand()));
                for (const Station &s : manager.favorites(manager.currentBand()))
                    qInfo().noquote() << QStringLiteral("   - %1 %2 kHz name=%3 source=%4")
                        .arg(s.id).arg(s.frequencyKHz).arg(s.name).arg(s.source);
            }
            if (parser.isSet(managerPrintStateOption)) {
                qInfo().noquote() << QStringLiteral("[MGR] region=%1 band=%2 freq=%3")
                    .arg(repository.region())
                    .arg(bandName(manager.currentBand()))
                    .arg(manager.currentFrequencyKHz());
                qInfo().noquote() << QStringLiteral("[MGR] stations FM/AM=%1/%2 favorites FM/AM=%3/%4")
                    .arg(manager.stations(Band::FM).size())
                    .arg(manager.stations(Band::AM).size())
                    .arg(manager.favorites(Band::FM).size())
                    .arg(manager.favorites(Band::AM).size());
            }
            if (parser.isSet(managerSeekOption)) {
                const bool up = !parser.isSet(managerSeekDownOption);
                const SeekMode mode = (parser.value(managerSeekModeOption).trimmed().toLower()
                    == QStringLiteral("next")) ? SeekMode::NextStation : SeekMode::FullBand;
                manager.startSeek(up, mode);
                int cancelAfter = 0;
                if (parser.isSet(managerSeekCancelAfterOption))
                    parseIntegerOption(parser, managerSeekCancelAfterOption, &cancelAfter);
                if (cancelAfter > 0)
                    QTimer::singleShot(cancelAfter, &app, [&](){
                        qInfo().noquote() << QStringLiteral("[MGR][SEEK] cancelSeek() requested");
                        scheduler.cancelSeek();
                    });
                if (waitUntil([&](){ return mgrSeekDone; }, 120000)) {
                    qInfo().noquote() << QStringLiteral("[MGR][SEEK] summary: found=%1 stop=%2")
                        .arg(mgrSeekFound ? QStringLiteral("true") : QStringLiteral("false"))
                        .arg(mgrSeekFinal);
                } else {
                    qCritical().noquote() << "[FAIL] manager.startSeek | timed out";
                    anyFailed = true;
                }
            }
        }
        manager.shutdown();
        qInfo().noquote() << QStringLiteral("[MGR] state saved -> %1, config saved -> %2")
            .arg(stateFile).arg(configFile);
    }


    scheduler.shutdown();
    return anyFailed ? 1 : 0;
}
