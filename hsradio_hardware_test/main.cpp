#include "hsradio/hardware/HSRadioSAF775XBackend.h"
#include "hsradio/hardware/IRadioHardware.h"
#include "hsradio/HSRadioSeekPolicy.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDebug>
#include <QList>
#include <QThread>
#include <QStringList>
#include <QTime>

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

bool reportResult(const QString &operation, const Result &result)
{
    if (result.ok()) {
        qInfo().noquote() << "[PASS]" << operation;
        return true;
    }

    qCritical().noquote()
        << "[FAIL]" << operation
        << "| code =" << errorCodeName(result.code)
        << "| message =" << result.message;
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

// 参考驱动时序：调谐后需等 PLL 锁定（FM 8ms / AM 60ms），再多次抽样(2ms 间隔)。
// N 次抽样中累计 3 次无效即放弃；至少 1 次有效则判为有效电台。
// 判定按 band 用传入的那一组阈值，复用库内 isStationValid。
bool sampleStationValid(IRadioHardware &hw,
                        Band band,
                        const SeekThreshold &th,
                        int samples = 4,
                        int sampleDelayMs = 2,
                        int lockDelayMs = -1)
{
    const int lockMs = lockDelayMs >= 0 ? lockDelayMs : th.lockMs;
    QThread::msleep(static_cast<unsigned long>(lockMs));
    int validCount = 0;
    int invalidCount = 0;
    for (int i = 0; i < samples; ++i) {
        SignalInfo info;
        if (!hw.readSignalInfo(info).ok())
            return false;

        const bool ok = isStationValid(info, band, th);

        if (ok) ++validCount; else ++invalidCount;
        if (invalidCount >= 3) break;
        QThread::msleep(static_cast<unsigned long>(sampleDelayMs));
    }
    return validCount >= 1;
}

// 自动搜台：在 [fromKHz, toKHz] 按 stepKHz 步进，以 Search 模式静音调谐，
// 命中阈值则记录。返回是否找到至少一个电台。
// 注：本工具只负责"找台"并记录，找到后不自动切回 Preset（避免测试时出声/爆音）。
bool runStationSearch(IRadioHardware &hw,
                      Band band,
                      int fromKHz,
                      int toKHz,
                      int stepKHz,
                      const SeekThreshold &th)
{
    if (stepKHz <= 0) {
        qCritical() << "Seek step must be positive.";
        return false;
    }

    const QString bandName = band == Band::FM ? QStringLiteral("FM")
                                            : QStringLiteral("AM");
    if (!reportResult(QStringLiteral("setBand(%1)").arg(bandName),
                      hw.setBand(band))) {
        return false;
    }

    QTime time;
    time.start();// 启动计时器

    QList<int> found;
    for (int f = fromKHz; f <= toKHz; f += stepKHz) {
        if (!hw.tune(f, IRadioHardware::TuneMode::Search).ok()) {
            qWarning().noquote() << "tune skipped at" << f << "kHz";
            continue;
        }
        if (sampleStationValid(hw, band, th)) {
            found.append(f);
            qInfo().noquote() << "[FOUND] station at" << f << "kHz";
        }
    }

    qInfo().noquote()
        << QStringLiteral("Seek complete: %1 station(s) found(%2 s)").arg(found.size()).arg(time.elapsed()/1000.0);
    for (int f : found)
        qInfo().noquote() << "  -" << f << "kHz";

    return !found.isEmpty();
}

} // 命名空间

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("HSRadioHardwareTest"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0"));

    qSetMessagePattern("[%{time yyyy-MM-dd h:mm:ss.zzz}] [%{if-debug}D%{endif}%{if-info}I%{endif}%{if-warning}W%{endif}%{if-critical}C%{endif}%{if-fatal}F%{endif}] [%{pid}:%{threadid}] [%{category}] - %{message}");

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("HSRadio ALSA hardware backend test tool (SAF775X BYTES protocol)"));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption deviceOption(
        QStringList() << QStringLiteral("d") << QStringLiteral("device"),
        QStringLiteral("ALSA control device (default: default)."),
        QStringLiteral("device"),
        QStringLiteral("default"));

    const QCommandLineOption tuneControlOption(
        QStringLiteral("tune-control"),
        QStringLiteral("ALSA BYTES control carrying the SAF775X tune struct (band + frequency + signal)."),
        QStringLiteral("name"),
        QStringLiteral("Radiocontrol"));

    const QCommandLineOption muteControlOption(
        QStringLiteral("mute-control"),
        QStringLiteral("ALSA INTEGER/BOOLEAN control for mute."),
        QStringLiteral("name"),
        QStringLiteral("Mute_radio"));

    const QCommandLineOption regControlOption(
        QStringLiteral("reg-control"),
        QStringLiteral("Optional ALSA BYTES control for register access (Phase 1 out of scope)."),
        QStringLiteral("name"));

    const QCommandLineOption muteOffValueOption(
        QStringLiteral("mute-off-value"),
        QStringLiteral("ALSA integer value representing unmuted (default: 0)."),
        QStringLiteral("value"),
        QStringLiteral("0"));

    const QCommandLineOption muteOnValueOption(
        QStringLiteral("mute-on-value"),
        QStringLiteral("ALSA integer value representing muted (default: 1)."),
        QStringLiteral("value"),
        QStringLiteral("1"));

    const QCommandLineOption setBandOption(
        QStringLiteral("set-band"),
        QStringLiteral("Optional write test: set band to FM or AM."),
        QStringLiteral("FM|AM"));

    const QCommandLineOption setFrequencyOption(
        QStringLiteral("set-frequency"),
        QStringLiteral("Optional write test: set frequency in kHz."),
        QStringLiteral("kHz"));

    const QCommandLineOption setMuteOption(
        QStringLiteral("set-mute"),
        QStringLiteral("Optional write test: set mute to on or off."),
        QStringLiteral("on|off"));

    const QCommandLineOption searchOption(
        QStringLiteral("search"),
        QStringLiteral("Use Search (stay-muted) tune mode instead of Preset when tuning."));

    const QCommandLineOption seekOption(
        QStringLiteral("seek"),
        QStringLiteral("Run automatic station search over [--seek-from, --seek-to] with --seek-step."));

    const QCommandLineOption seekBandOption(
        QStringLiteral("seek-band"),
        QStringLiteral("Band for seek (FM|AM, default: FM)."),
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

    const QCommandLineOption seekMinRssiOption(
        QStringLiteral("seek-min-rssi"),
        QStringLiteral("Optional: minimum RSSI(level) to accept a station (FM, default: 25)."),
        QStringLiteral("value"));

    const QCommandLineOption seekMaxUsnOption(
        QStringLiteral("seek-max-usn"),
        QStringLiteral("Optional: maximum USN to accept a station (default: 25)."),
        QStringLiteral("value"));

    const QCommandLineOption seekMaxWamOption(
        QStringLiteral("seek-max-wam"),
        QStringLiteral("Optional: maximum WAM to accept a station (default: 25)."),
        QStringLiteral("value"));

    const QCommandLineOption seekMaxOffsetOption(
        QStringLiteral("seek-max-offset"),
        QStringLiteral("Optional: maximum offset to accept a station (default: 40)."),
        QStringLiteral("value"));

    parser.addOption(deviceOption);
    parser.addOption(tuneControlOption);
    parser.addOption(muteControlOption);
    parser.addOption(regControlOption);
    parser.addOption(muteOffValueOption);
    parser.addOption(muteOnValueOption);
    parser.addOption(setBandOption);
    parser.addOption(setFrequencyOption);
    parser.addOption(setMuteOption);
    parser.addOption(searchOption);
    parser.addOption(seekOption);
    parser.addOption(seekBandOption);
    parser.addOption(seekFromOption);
    parser.addOption(seekToOption);
    parser.addOption(seekStepOption);
    parser.addOption(seekMinRssiOption);
    parser.addOption(seekMaxUsnOption);
    parser.addOption(seekMaxWamOption);
    parser.addOption(seekMaxOffsetOption);
    parser.process(app);

    if (!parser.isSet(tuneControlOption) || !parser.isSet(muteControlOption)) {
        qCritical().noquote()
            << "Missing required ALSA control names (--tune-control, --mute-control). Use --help for usage.";
        return 2;
    }

    HSRadioSAF775XConfig config;
    config.device = parser.value(deviceOption);
    config.tuneControl = parser.value(tuneControlOption);
    config.muteControl = parser.value(muteControlOption);
    config.regControl = parser.value(regControlOption);

    bool numericOk = false;
    config.muteOffValue = parser.value(muteOffValueOption).toLong(&numericOk);
    if (!numericOk) {
        qCritical() << "Invalid --mute-off-value";
        return 2;
    }
    config.muteOnValue = parser.value(muteOnValueOption).toLong(&numericOk);
    if (!numericOk) {
        qCritical() << "Invalid --mute-on-value";
        return 2;
    }

    qInfo().noquote() << "ALSA device:" << config.device;
    qInfo().noquote() << "Tune control:" << config.tuneControl;
    qInfo().noquote() << "Mute control:" << config.muteControl;
    qInfo().noquote() << "Reg control:"
                      << (config.regControl.isEmpty() ? QStringLiteral("(none)")
                                                      : config.regControl);

    // 该独立测试在主线程上运行后端。
    // 生产代码必须将后端移到其专用的硬件线程
    // 并在该对象的所属线程中调用所有方法。
    HSRadioSAF775XBackend hardware(config);

    if (!reportResult(QStringLiteral("initialize"), hardware.initialize())) {
        hardware.shutdown();
        return 1;
    }

    bool operationFailed = false;

    if (parser.isSet(setBandOption)) {
        const QString bandText = parser.value(setBandOption).trimmed().toUpper();
        if (bandText == QStringLiteral("FM")) {
            operationFailed |= !reportResult(
                QStringLiteral("setBand(FM)"), hardware.setBand(Band::FM));
        } else if (bandText == QStringLiteral("AM")) {
            operationFailed |= !reportResult(
                QStringLiteral("setBand(AM)"), hardware.setBand(Band::AM));
        } else {
            qCritical() << "Invalid --set-band value; use FM or AM.";
            operationFailed = true;
        }
    }

    if (parser.isSet(setFrequencyOption)) {
        int frequencyKHz = 0;
        if (!parseIntegerOption(parser, setFrequencyOption, &frequencyKHz)) {
            qCritical() << "Invalid --set-frequency value.";
            operationFailed = true;
        } else {
            const bool search = parser.isSet(searchOption);
            const IRadioHardware::TuneMode mode = search ? IRadioHardware::TuneMode::Search : IRadioHardware::TuneMode::Preset;
            operationFailed |= !reportResult(
                QStringLiteral("tune(%1 kHz, %2)")
                    .arg(frequencyKHz)
                    .arg(search ? QStringLiteral("search") : QStringLiteral("preset")),
                hardware.tune(frequencyKHz, mode));
        }
    }

    if (parser.isSet(setMuteOption)) {
        const QString muteText = parser.value(setMuteOption).trimmed().toLower();
        if (muteText == QStringLiteral("on")) {
            operationFailed |= !reportResult(
                QStringLiteral("setMute(true)"), hardware.setMute(true));
        } else if (muteText == QStringLiteral("off")) {
            operationFailed |= !reportResult(
                QStringLiteral("setMute(false)"), hardware.setMute(false));
        } else {
            qCritical() << "Invalid --set-mute value; use on or off.";
            operationFailed = true;
        }
    }

    HardwareState state;
    if (reportResult(QStringLiteral("readState"), hardware.readState(state))) {
        const QString bandName = state.band == Band::FM
            ? QStringLiteral("FM") : QStringLiteral("AM");
        qInfo().noquote()
            << QStringLiteral("Current state: band=%1, frequency=%2 kHz, mute=%3")
                   .arg(bandName)
                   .arg(state.frequencyKHz)
                   .arg(state.muted ? QStringLiteral("on") : QStringLiteral("off"));
    } else {
        operationFailed = true;
    }

    SignalInfo signalInfo;
    if (reportResult(QStringLiteral("readSignalInfo"),
                     hardware.readSignalInfo(signalInfo))) {
        QStringList fields;
        if (signalInfo.hasRssi)
            fields << QStringLiteral("RSSI=%1").arg(signalInfo.rssi);
        if (signalInfo.hasUsn)
            fields << QStringLiteral("USN=%1").arg(signalInfo.usn);
        if (signalInfo.hasWam)
            fields << QStringLiteral("WAM=%1").arg(signalInfo.wam);
        if (signalInfo.hasOffset)
            fields << QStringLiteral("OFFSET=%1").arg(signalInfo.offset);
        if (signalInfo.hasModulation)
            fields << QStringLiteral("MOD=%1").arg(signalInfo.modulation);
        if (signalInfo.hasStereo)
            fields << QStringLiteral("Stereo=%1")
                          .arg(signalInfo.stereo ? QStringLiteral("yes")
                                                 : QStringLiteral("no"));

        qInfo().noquote() << "Signal info:"
                          << (fields.isEmpty()
                                  ? QStringLiteral("no signal data")
                                  : fields.join(QStringLiteral(", ")));
    } else {
        operationFailed = true;
    }

    if (parser.isSet(seekOption)) {
        int seekFrom = 87500, seekTo = 108000, seekStep = 100;
        if (!parseIntegerOption(parser, seekFromOption, &seekFrom)
            || !parseIntegerOption(parser, seekToOption, &seekTo)
            || !parseIntegerOption(parser, seekStepOption, &seekStep)) {
            qCritical() << "Invalid --seek-from / --seek-to / --seek-step value.";
            operationFailed = true;
        } else {
            const QString seekBandText = parser.value(seekBandOption).trimmed().toUpper();
            Band seekBand = Band::FM;
            if (seekBandText == QStringLiteral("AM"))
                seekBand = Band::AM;
            else if (seekBandText != QStringLiteral("FM")) {
                qCritical() << "Invalid --seek-band; use FM or AM.";
                operationFailed = true;
            }

            // 按本次搜台频段取一组阈值；CLI 的 --seek-* 选项只作用于 FM。
            SeekThreshold th = (seekBand == Band::AM)
                                   ? kDefaultAmSeekThreshold
                                   : kDefaultFmSeekThreshold;
            if (seekBand == Band::FM) {
                if (parser.isSet(seekMinRssiOption)
                    && !parseIntegerOption(parser, seekMinRssiOption, &th.minRssi))
                    operationFailed = true;
                if (parser.isSet(seekMaxUsnOption)
                    && !parseIntegerOption(parser, seekMaxUsnOption, &th.maxUsn))
                    operationFailed = true;
                if (parser.isSet(seekMaxWamOption)
                    && !parseIntegerOption(parser, seekMaxWamOption, &th.maxWam))
                    operationFailed = true;
                if (parser.isSet(seekMaxOffsetOption)
                    && !parseIntegerOption(parser, seekMaxOffsetOption, &th.maxOffset))
                    operationFailed = true;
            }

            if (!operationFailed) {
                const bool found = runStationSearch(hardware, seekBand,
                                                   seekFrom, seekTo, seekStep,
                                                   th);
                operationFailed |= !reportResult(
                    QStringLiteral("seek(%1, %2..%3 @%4)")
                        .arg(seekBand == Band::FM ? QStringLiteral("FM")
                                                   : QStringLiteral("AM"))
                        .arg(seekFrom).arg(seekTo).arg(seekStep),
                    found ? Result::success()
                          : Result::failure(ErrorCode::HardwareOperationFailed,
                                            QStringLiteral("no station found")));
            }
        }
    }

    hardware.shutdown();
    reportResult(QStringLiteral("shutdown"), Result::success());

    return operationFailed ? 1 : 0;
}
