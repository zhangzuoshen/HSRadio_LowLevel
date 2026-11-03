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
    // Tune mode selects the driver tune behaviour:
    //   - Preset : tune to a new program with a short mute (normal listening).
    //   - Search : tune and stay muted (used while scanning for stations).
    enum class TuneMode : quint8 { Preset = 0x10, Search = 0x20 };

    virtual Result tune(int frequencyKHz, TuneMode mode = TuneMode::Preset) = 0;
    virtual Result setMute(bool muted) = 0;
    virtual Result readState(HardwareState &state) = 0;
    virtual Result readSignalInfo(SignalInfo &info) = 0;
    virtual void shutdown() = 0;
};

} // namespace HSRadio
