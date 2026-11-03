// HSRadioManagerTest —— 调度器 / 业务层接口调试工具。
//
// 与 HSRadioHardwareTest 同一形态：所有操作由命令行参数点名触发，
// 每一步打印 [PASS]/[FAIL] 与回读值，不做写死的断言、不自动跑用例。
// 后端是 MockRadioHardware，因此不需要 ALSA、不需要 i.MX9 真机。
//
//   ./hsradio_manager_test --help
//   ./hsradio_manager_test --mock-demo --set-band FM --set-frequency 87500
//   ./hsradio_manager_test --mock-demo --seek --seek-lock-ms 0 --seek-sample-ms 0

#include "hsradio/HSRadioTypes.h"
#include "hsradio/hardware/MockRadioHardware.h"
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

bool parseErrorCode(const QString &text, ErrorCode *code)
{
    const QString name = text.trimmed();
    const QStringList names = {
        QStringLiteral("None"), QStringLiteral("InvalidParameter"),
        QStringLiteral("HardwareNotReady"), QStringLiteral("AlsaOpenFailed"),
        QStringLiteral("AlsaControlNotFound"), QStringLiteral("AlsaControlTypeUnsupported"),
        QStringLiteral("AlsaReadFailed"), QStringLiteral("AlsaWriteFailed"),
        QStringLiteral("HardwareOperationFailed")
    };
    const QVector<ErrorCode> codes = {
        ErrorCode::None, ErrorCode::InvalidParameter,
        ErrorCode::HardwareNotReady, ErrorCode::AlsaOpenFailed,
        ErrorCode::AlsaControlNotFound, ErrorCode::AlsaControlTypeUnsupported,
        ErrorCode::AlsaReadFailed, ErrorCode::AlsaWriteFailed,
        ErrorCode::HardwareOperationFailed
    };
    for (int i = 0; i < names.size(); ++i) {
        if (name.compare(names.at(i), Qt::CaseInsensitive) == 0) {
            *code = codes.at(i);
            return true;
        }
    }
    return false;
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

SignalInfo defaultStationSignal()
{
    SignalInfo info;
    info.rssi = 40;
    info.usn = 5;
    info.wam = 5;
    info.offset = 10;
    info.stereo = true;
    info.hasRssi = true;
    info.hasUsn = true;
    info.hasWam = true;
    info.hasOffset = true;
    info.hasStereo = true;
    return info;
}

// --mock-station 87500[:rssi:usn:wam:offset]
bool parseMockStation(const QString &text, int *frequencyKHz, SignalInfo *info)
{
    const QStringList parts = text.split(QLatin1Char(':'));
    if (parts.isEmpty())
        return false;

    bool ok = false;
    *frequencyKHz = parts.at(0).toInt(&ok);
    if (!ok)
        return false;

    *info = defaultStationSignal();
    int *targets[] = { &info->rssi, &info->usn, &info->wam, &info->offset };
    for (int i = 0; i < 4; ++i) {
        if (parts.size() <= i + 1)
            break;
        const int value = parts.at(i + 1).toInt(&ok);
        if (!ok)
            return false;
        *targets[i] = value;
    }
    return true;
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
    QHash<quint64, SignalInfo> signals;
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
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("HSRadioManagerTest"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("HSRadio scheduler/manager interface debug tool (MockRadioHardware, no ALSA / no target board)."));
    parser.addHelpOption();
    parser.addVersionOption();

    // —— Mock 配置 ——
    const QCommandLineOption mockStationOption(
        QStringLiteral("mock-station"),
        QStringLiteral("Preset a valid station: kHz[:rssi:usn:wam:offset]. Repeatable."),
        QStringLiteral("spec"));
    const QCommandLineOption mockDemoOption(
        QStringLiteral("mock-demo"),
        QStringLiteral("Preset three demo stations (87500, 98000, 104800 kHz)."));
    const QCommandLineOption mockFailureOption(
        QStringLiteral("mock-failure"),
        QStringLiteral("Inject a tune failure, e.g. AlsaWriteFailed / HardwareNotReady (default: None)."),
        QStringLiteral("ErrorCode"),
        QStringLiteral("None"));

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

    parser.addOption(mockStationOption);
    parser.addOption(mockDemoOption);
    parser.addOption(mockFailureOption);
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
    parser.process(app);

    // —— 组装 Mock 硬件 ——
    auto hardware = std::unique_ptr<MockRadioHardware>(new MockRadioHardware);
    MockRadioHardware *mock = hardware.get();

    int presetCount = 0;
    if (parser.isSet(mockDemoOption)) {
        mock->setStationSignal(87500, defaultStationSignal());
        mock->setStationSignal(98000, defaultStationSignal());
        mock->setStationSignal(104800, defaultStationSignal());
        presetCount += 3;
    }
    for (const QString &text : parser.values(mockStationOption)) {
        int frequencyKHz = 0;
        SignalInfo info;
        if (!parseMockStation(text, &frequencyKHz, &info)) {
            qCritical().noquote() << "Invalid --mock-station value:" << text;
            return 2;
        }
        mock->setStationSignal(frequencyKHz, info);
        ++presetCount;
    }

    ErrorCode injectedFailure = ErrorCode::None;
    if (!parseErrorCode(parser.value(mockFailureOption), &injectedFailure)) {
        qCritical().noquote() << "Invalid --mock-failure value:" << parser.value(mockFailureOption);
        return 2;
    }
    mock->setTuneFailure(injectedFailure);

    qInfo().noquote()
        << QStringLiteral("Backend: MockRadioHardware | preset stations: %1 (--mock-demo / --mock-station)")
               .arg(presetCount);
    qInfo().noquote()
        << QStringLiteral("Injected tune failure: %1").arg(errorCodeName(injectedFailure));

    // —— 启动调度器 ——
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
                     [&](quint64 id, const SignalInfo &info) { probe.signals.insert(id, info); });
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
    if (!reportResult(QStringLiteral("initialize(MockRadioHardware)"), initCode, QString())) {
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
                << QStringLiteral("  signal: %1").arg(signalText(probe.signals.value(signalId)));
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

    // —— Mock 观测点：验证调谐模式与"搜台强制静音/恢复"——
    qInfo().noquote()
        << QStringLiteral("[MOCK] lastTuneMode=%1 muteCallCount=%2")
               .arg(mock->lastTuneMode() == IRadioHardware::TuneMode::Search
                        ? QStringLiteral("Search")
                        : QStringLiteral("Preset"))
               .arg(mock->muteCallCount());
    HardwareState finalState;
    mock->readState(finalState);
    qInfo().noquote()
        << QStringLiteral("[MOCK] final: band=%1 frequency=%2 kHz mute=%3")
               .arg(bandName(finalState.band))
               .arg(finalState.frequencyKHz)
               .arg(finalState.muted ? QStringLiteral("on") : QStringLiteral("off"));

    scheduler.shutdown();
    return anyFailed ? 1 : 0;
}
