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
        QStringLiteral("HSRadio ALSA hardware backend test tool"));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption deviceOption(
        QStringList() << QStringLiteral("d") << QStringLiteral("device"),
        QStringLiteral("ALSA control device (default: default)."),
        QStringLiteral("device"),
        QStringLiteral("default"));

    const QCommandLineOption bandControlOption(
        QStringLiteral("band-control"),
        QStringLiteral("ALSA mixer control name for band selection."),
        QStringLiteral("name"));

    const QCommandLineOption frequencyControlOption(
        QStringLiteral("frequency-control"),
        QStringLiteral("ALSA mixer control name for frequency in kHz."),
        QStringLiteral("name"));

    const QCommandLineOption muteControlOption(
        QStringLiteral("mute-control"),
        QStringLiteral("ALSA mixer control name for mute."),
        QStringLiteral("name"));

    const QCommandLineOption rssiControlOption(
        QStringLiteral("rssi-control"),
        QStringLiteral("Optional ALSA mixer control name for RSSI."),
        QStringLiteral("name"));

    const QCommandLineOption usnControlOption(
        QStringLiteral("usn-control"),
        QStringLiteral("Optional ALSA mixer control name for USN."),
        QStringLiteral("name"));

    const QCommandLineOption wamControlOption(
        QStringLiteral("wam-control"),
        QStringLiteral("Optional ALSA mixer control name for WAM."),
        QStringLiteral("name"));

    const QCommandLineOption offsetControlOption(
        QStringLiteral("offset-control"),
        QStringLiteral("Optional ALSA mixer control name for OFFSET."),
        QStringLiteral("name"));

    const QCommandLineOption stereoControlOption(
        QStringLiteral("stereo-control"),
        QStringLiteral("Optional ALSA mixer control name for stereo status."),
        QStringLiteral("name"));

    const QCommandLineOption fmValueOption(
        QStringLiteral("fm-value"),
        QStringLiteral("ALSA integer value representing FM (default: 0)."),
        QStringLiteral("value"),
        QStringLiteral("0"));

    const QCommandLineOption amValueOption(
        QStringLiteral("am-value"),
        QStringLiteral("ALSA integer value representing AM (default: 1)."),
        QStringLiteral("value"),
        QStringLiteral("1"));

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

    parser.addOption(deviceOption);
    parser.addOption(bandControlOption);
    parser.addOption(frequencyControlOption);
    parser.addOption(muteControlOption);
    parser.addOption(rssiControlOption);
    parser.addOption(usnControlOption);
    parser.addOption(wamControlOption);
    parser.addOption(offsetControlOption);
    parser.addOption(stereoControlOption);
    parser.addOption(fmValueOption);
    parser.addOption(amValueOption);
    parser.addOption(muteOffValueOption);
    parser.addOption(muteOnValueOption);
    parser.addOption(setBandOption);
    parser.addOption(setFrequencyOption);
    parser.addOption(setMuteOption);
    parser.process(app);

    if (!parser.isSet(bandControlOption)
        || !parser.isSet(frequencyControlOption)
        || !parser.isSet(muteControlOption)) {
        qCritical().noquote()
            << "Missing required ALSA control names. Use --help for usage.";
        return 2;
    }

    AlsaRadioConfig config;
    config.device = parser.value(deviceOption);
    config.band.name = parser.value(bandControlOption);
    config.frequencyKHz.name = parser.value(frequencyControlOption);
    config.mute.name = parser.value(muteControlOption);

    config.rssi.name = parser.value(rssiControlOption);
    config.usn.name = parser.value(usnControlOption);
    config.wam.name = parser.value(wamControlOption);
    config.offset.name = parser.value(offsetControlOption);
    config.stereo.name = parser.value(stereoControlOption);

    bool numericOk = false;
    config.fmBandValue = parser.value(fmValueOption).toLong(&numericOk);
    if (!numericOk) {
        qCritical() << "Invalid --fm-value";
        return 2;
    }
    config.amBandValue = parser.value(amValueOption).toLong(&numericOk);
    if (!numericOk) {
        qCritical() << "Invalid --am-value";
        return 2;
    }
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
    qInfo().noquote() << "Band control:" << config.band.name;
    qInfo().noquote() << "Frequency control:" << config.frequencyKHz.name;
    qInfo().noquote() << "Mute control:" << config.mute.name;
    qInfo() << "Optional signal controls:"
            << "RSSI =" << config.rssi.name
            << "USN =" << config.usn.name
            << "WAM =" << config.wam.name
            << "OFFSET =" << config.offset.name
            << "Stereo =" << config.stereo.name;

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
            operationFailed |= !reportResult(
                QStringLiteral("tune(%1 kHz)").arg(frequencyKHz),
                hardware.tune(frequencyKHz));
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
                                  ? QStringLiteral("no optional signal controls configured")
                                  : fields.join(QStringLiteral(", ")));
    } else {
        operationFailed = true;
    }

    hardware.shutdown();
    reportResult(QStringLiteral("shutdown"), Result::success());

    return operationFailed ? 1 : 0;
}
