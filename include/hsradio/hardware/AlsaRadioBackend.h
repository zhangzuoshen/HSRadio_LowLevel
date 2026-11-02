#pragma once

#include "hsradio/hardware/IRadioHardware.h"

#include <QObject>
#include <QString>

struct _snd_ctl;
typedef struct _snd_ctl snd_ctl_t;

namespace HSRadio {

struct AlsaIntegerControl {
    QString name;
    unsigned int index = 0;
};

// Control names are intentionally supplied by the platform integration layer.
// Do not assume names from another SAF775D board/driver are identical.
struct AlsaRadioConfig {
    QString device = QStringLiteral("default");

    AlsaIntegerControl band;       // 0=FM, 1=AM unless values below are changed
    long fmBandValue = 0;
    long amBandValue = 1;

    AlsaIntegerControl frequencyKHz;
    AlsaIntegerControl mute;
    long muteOffValue = 0;
    long muteOnValue = 1;

    AlsaIntegerControl rssi;
    AlsaIntegerControl usn;
    AlsaIntegerControl wam;
    AlsaIntegerControl offset;
    AlsaIntegerControl stereo;
};

class AlsaRadioBackend final : public QObject, public IRadioHardware {
public:
    explicit AlsaRadioBackend(AlsaRadioConfig config, QObject *parent = nullptr);
    ~AlsaRadioBackend() override;

    Result initialize() override;
    Result setBand(Band band) override;
    Result tune(int frequencyKHz) override;
    Result setMute(bool muted) override;
    Result readState(HardwareState &state) override;
    Result readSignalInfo(SignalInfo &info) override;
    void shutdown() override;

private:
    Result openControl();
    Result readInteger(const AlsaIntegerControl &control, long long &value) const;
    Result writeInteger(const AlsaIntegerControl &control, long long value) const;
    Result validateControl(const AlsaIntegerControl &control) const;
    bool validateOptionalControl(const AlsaIntegerControl &control) const;
    Result checkThread() const;
    static Result alsaFailure(ErrorCode code, const QString &operation,
                              int alsaError);

    AlsaRadioConfig m_config;
    snd_ctl_t *m_ctl = nullptr;
    bool m_initialized = false;
    bool m_hasRssi = false;
    bool m_hasUsn = false;
    bool m_hasWam = false;
    bool m_hasOffset = false;
    bool m_hasStereo = false;
};

} // namespace HSRadio
