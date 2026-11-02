#pragma once

#include <QMetaType>
#include <QString>

namespace HSRadio {

enum class Band : quint8 {
    FM = 0,
    AM = 1
};

enum class ErrorCode : quint16 {
    None = 0,
    InvalidParameter,
    HardwareNotReady,
    AlsaOpenFailed,
    AlsaControlNotFound,
    AlsaControlTypeUnsupported,
    AlsaReadFailed,
    AlsaWriteFailed,
    HardwareOperationFailed
};

struct Result {
    ErrorCode code = ErrorCode::None;
    QString message;

    bool ok() const noexcept { return code == ErrorCode::None; }

    static Result success() { return {}; }
    static Result failure(ErrorCode c, const QString &m)
    {
        return {c, m};
    }
};

struct SignalInfo {
    int rssi = 0;
    int usn = 0;
    int wam = 0;
    int offset = 0;
    bool stereo = false;

    bool hasRssi = false;
    bool hasUsn = false;
    bool hasWam = false;
    bool hasOffset = false;
    bool hasStereo = false;
};

struct HardwareState {
    Band band = Band::FM;
    int frequencyKHz = 87000;
    bool muted = false;
};

} // namespace HSRadio

Q_DECLARE_METATYPE(HSRadio::Band)
Q_DECLARE_METATYPE(HSRadio::ErrorCode)
Q_DECLARE_METATYPE(HSRadio::Result)
Q_DECLARE_METATYPE(HSRadio::SignalInfo)
Q_DECLARE_METATYPE(HSRadio::HardwareState)
