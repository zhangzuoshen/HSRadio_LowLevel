#include "hsradio/hardware/AlsaRadioBackend.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDebug>
#include <QStringList>

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

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("HSRadioHardwareTest"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("HSRadio ALSA hardware backend test tool (SAF775D BYTES protocol)"));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption deviceOption(
        QStringList() << QStringLiteral("d") << QStringLiteral("device"),
        QStringLiteral("ALSA control device (default: default)."),
        QStringLiteral("device"),
        QStringLiteral("default"));

    const QCommandLineOption tuneControlOption(
        QStringLiteral("tune-control"),
        QStringLiteral("ALSA BYTES control carrying the SAF775D tune struct (band + frequency + signal)."),
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
    parser.process(app);

    if (!parser.isSet(tuneControlOption) || !parser.isSet(muteControlOption)) {
        qCritical().noquote()
            << "Missing required ALSA control names (--tune-control, --mute-control). Use --help for usage.";
        return 2;
    }

    AlsaRadioConfig config;
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

    // This standalone test runs the backend on the main thread.
    // Production code must move the backend to its dedicated hardware thread
    // and invoke all methods in that object's owning thread.
    AlsaRadioBackend hardware(config);

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
            const TuneMode mode = search ? TuneMode::Search : TuneMode::Preset;
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

    hardware.shutdown();
    reportResult(QStringLiteral("shutdown"), Result::success());

    return operationFailed ? 1 : 0;
}
