#pragma once

#include "hsradio/HSRadioTypes.h"

namespace HSRadio {

// All methods are synchronous and must be called from the hardware object's
// owning thread. The manager/scheduler should use queued invocations so that
// ALSA I/O never blocks the service main thread.
class IRadioHardware {
public:
    virtual ~IRadioHardware() = default;

    virtual Result initialize() = 0;
    virtual Result setBand(Band band) = 0;
    // Tune mode selects the driver tune behaviour. The numeric values are
    // hardware-agnostic and MUST NOT encode any ALSA/device protocol bytes;
    // the concrete backend translates them to driver-specific control values
    // (e.g. the SAF775D uses 0x10/0x20). This keeps the interface decoupled
    // from any particular hardware.
    //   - Preset : tune to a new program with a short mute (normal listening).
    //   - Search : tune and stay muted (used while scanning for stations).
    enum class TuneMode : quint8 { Preset, Search };

    virtual Result tune(int frequencyKHz, TuneMode mode = TuneMode::Preset) = 0;
    virtual Result setMute(bool muted) = 0;
    virtual Result readState(HardwareState &state) = 0;
    virtual Result readSignalInfo(SignalInfo &info) = 0;
    virtual void shutdown() = 0;
};

} // namespace HSRadio
