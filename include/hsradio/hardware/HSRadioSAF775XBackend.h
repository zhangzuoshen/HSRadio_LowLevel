#pragma once

#include "hsradio/hardware/IRadioHardware.h"

#include <QObject>
#include <QString>

struct _snd_ctl;
typedef struct _snd_ctl snd_ctl_t;

namespace HSRadio {

// The SAF775X ALSA driver exposes three mixer controls. These names are
// board/driver-specific and supplied by the platform integration layer.
//   - tuneControl : SND_CTL_ELEM_TYPE_BYTES. Carries the tune request struct
//                   (band + frequency + mode) and returns signal information
//                   when read back after a read-back request.
//   - muteControl : SND_CTL_ELEM_TYPE_INTEGER/BOOLEAN. 0 = unmute, 1 = mute.
//   - regControl  : SND_CTL_ELEM_TYPE_BYTES (optional; Phase 1 out of scope).
//                   Register read/write (reg + data).
//
// This is intentionally NOT the idealized "one integer control per parameter"
// model: the SAF775X packs band, frequency and signal status into a single
// BYTES blob, so the backend speaks the driver's binary protocol directly.
struct HSRadioSAF775XConfig {
    QString device = QStringLiteral("default");

    QString tuneControl = QStringLiteral("Radiocontrol");
    QString muteControl = QStringLiteral("Mute_radio");
    QString regControl; // optional, e.g. "Radioregvalue"

    long muteOffValue = 0;
    long muteOnValue = 1;
};

// Binary layout of the SAF775X tune request/response. The byte layout MUST
// match the kernel driver's expectation exactly. Natural alignment (no
// #pragma pack) yields 28 bytes on both x86-64 and aarch64; the static_assert
// in the .cpp guards against a mismatch with the target driver.
struct HSRadioSAF775XTune {
    unsigned char mode = 0x10;  // see SAF775X mode constants below
    unsigned char band = 0;     // 0 = FM, 1 = AM
    unsigned int freq = 0;     // 10 kHz units (8700 = 87.00 MHz); packed into ALSA as-is
    unsigned int usn = 0;
    unsigned char wam = 0;
    unsigned int offset = 0;
    unsigned int level = 0;     // received signal level (RSSI)
    unsigned char modulation = 0;
    unsigned char stereo = 0;
};

// SAF775X register trait (used by the optional regControl). #pragma pack(2)
// matches the driver layout; 2 bytes.
#pragma pack(push, 2)
struct HSRadioSAF775XRegTrait {
    unsigned char reg = 0;
    unsigned char data = 0;
};
#pragma pack(pop)

class HSRadioSAF775XBackend final : public QObject, public IRadioHardware {
public:
    explicit HSRadioSAF775XBackend(HSRadioSAF775XConfig config, QObject *parent = nullptr);
    ~HSRadioSAF775XBackend() override;

    Result initialize() override;
    Result setBand(Band band) override;
    Result tune(int frequencyKHz, TuneMode mode = TuneMode::Preset) override;
    Result setMute(bool muted) override;
    Result readState(HardwareState &state) override;
    Result readSignalInfo(SignalInfo &info) override;
    void shutdown() override;

private:
    Result openControl();
    Result applyTune();                  // write current band + frequency
    Result readTune(HSRadioSAF775XTune &out);   // mode=0x70 write, then read back
    Result writeMute(bool muted);
    Result readMute(bool &muted) const;
    Result checkThread() const;
    static Result alsaFailure(ErrorCode code, const QString &operation,
                              int alsaError);

    HSRadioSAF775XConfig m_config;
    snd_ctl_t *m_ctl = nullptr;
    bool m_initialized = false;

    Band m_currentBand = Band::FM;
    int m_currentFreqKHz = 87000;
    bool m_currentMuted = false;
    TuneMode m_currentTuneMode = TuneMode::Preset;
};

} // namespace HSRadio
