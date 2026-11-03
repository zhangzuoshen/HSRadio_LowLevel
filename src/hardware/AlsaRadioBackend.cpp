#include "hsradio/hardware/AlsaRadioBackend.h"

#include <QThread>

#include <alsa/asoundlib.h>

#include <cstring>
#include <limits>
#include <memory>
#include <utility>

namespace HSRadio {
namespace {

// Mode field of StRadioTune.
constexpr unsigned char kModeStandby  = 0x00;
constexpr unsigned char kModePreset   = 0x10; // tune to new program, short mute
constexpr unsigned char kModeSearch   = 0x20; // tune to new program, stay muted
constexpr unsigned char kModeReadBack = 0x70; // request signal parameters

// Translate the hardware-agnostic TuneMode into the SAF775D ALSA
// control byte. This is the ONLY place that knows the device values.
unsigned char alsaTuneMode(TuneMode mode) {
    switch (mode) {
    case TuneMode::Search: return kModeSearch;
    case TuneMode::Preset:
    default:              return kModePreset;
    }
}

struct ElemIdDeleter {
    void operator()(snd_ctl_elem_id_t *p) const noexcept { snd_ctl_elem_id_free(p); }
};
struct ElemInfoDeleter {
    void operator()(snd_ctl_elem_info_t *p) const noexcept { snd_ctl_elem_info_free(p); }
};
struct ElemValueDeleter {
    void operator()(snd_ctl_elem_value_t *p) const noexcept { snd_ctl_elem_value_free(p); }
};

using ElemIdPtr = std::unique_ptr<snd_ctl_elem_id_t, ElemIdDeleter>;
using ElemInfoPtr = std::unique_ptr<snd_ctl_elem_info_t, ElemInfoDeleter>;
using ElemValuePtr = std::unique_ptr<snd_ctl_elem_value_t, ElemValueDeleter>;

struct ControlInfo {
    ElemIdPtr id;
    ElemInfoPtr info;
};

// Compile-time guard: the tune struct must match the kernel driver layout.
static_assert(sizeof(StRadioTune) == 28, "StRadioTune layout mismatch");
static_assert(sizeof(RegTrait) == 2, "RegTrait layout mismatch");

// Minimal, version-agnostic BYTES helpers. Some embedded/cross ALSA
// toolchains do not declare snd_ctl_elem_value_set_bytes / get_bytes, so we
// use the always-available per-byte primitives instead.
static void setElemBytes(snd_ctl_elem_value_t *v, const void *data, size_t size) {
    const unsigned char *p = static_cast<const unsigned char *>(data);
    for (size_t i = 0; i < size; ++i)
        snd_ctl_elem_value_set_byte(v, static_cast<unsigned int>(i), p[i]);
}

static void getElemBytes(snd_ctl_elem_value_t *v, void *data, size_t size) {
    unsigned char *p = static_cast<unsigned char *>(data);
    for (size_t i = 0; i < size; ++i)
        p[i] = snd_ctl_elem_value_get_byte(v, static_cast<unsigned int>(i));
}

Result lookupElement(snd_ctl_t *ctl, const QString &name, unsigned int index,
                     ControlInfo &out)
{
    if (!ctl)
        return Result::failure(ErrorCode::HardwareNotReady,
                               QStringLiteral("ALSA control device is not open"));
    if (name.trimmed().isEmpty())
        return Result::failure(ErrorCode::InvalidParameter,
                               QStringLiteral("ALSA control name is empty"));

    snd_ctl_elem_id_t *rawId = nullptr;
    int rc = snd_ctl_elem_id_malloc(&rawId);
    if (rc < 0)
        return Result::failure(ErrorCode::HardwareOperationFailed,
                               QStringLiteral("snd_ctl_elem_id_malloc: %1")
                                   .arg(QString::fromLocal8Bit(snd_strerror(rc))));
    out.id.reset(rawId);

    snd_ctl_elem_info_t *rawInfo = nullptr;
    rc = snd_ctl_elem_info_malloc(&rawInfo);
    if (rc < 0)
        return Result::failure(ErrorCode::HardwareOperationFailed,
                               QStringLiteral("snd_ctl_elem_info_malloc: %1")
                                   .arg(QString::fromLocal8Bit(snd_strerror(rc))));
    out.info.reset(rawInfo);

    const QByteArray nameBytes = name.toLocal8Bit();
    snd_ctl_elem_id_set_interface(out.id.get(), SND_CTL_ELEM_IFACE_MIXER);
    snd_ctl_elem_id_set_name(out.id.get(), nameBytes.constData());
    snd_ctl_elem_id_set_index(out.id.get(), index);
    snd_ctl_elem_info_set_id(out.info.get(), out.id.get());

    rc = snd_ctl_elem_info(ctl, out.info.get());
    if (rc < 0) {
        return Result::failure(
            ErrorCode::AlsaControlNotFound,
            QStringLiteral("ALSA control '%1' (index %2): %3")
                .arg(name).arg(index)
                .arg(QString::fromLocal8Bit(snd_strerror(rc))));
    }
    if (snd_ctl_elem_info_get_count(out.info.get()) < 1) {
        return Result::failure(ErrorCode::AlsaControlTypeUnsupported,
                               QStringLiteral("ALSA control '%1' has no values")
                                   .arg(name));
    }
    return Result::success();
}

} // namespace

AlsaRadioBackend::AlsaRadioBackend(AlsaRadioConfig config, QObject *parent)
    : QObject(parent), m_config(std::move(config))
{
}

AlsaRadioBackend::~AlsaRadioBackend()
{
    // QObject instances must be destroyed in their owning thread.
    shutdown();
}

Result AlsaRadioBackend::checkThread() const
{
    if (QThread::currentThread() != thread()) {
        return Result::failure(
            ErrorCode::HardwareOperationFailed,
            QStringLiteral("AlsaRadioBackend called outside its owning thread"));
    }
    return Result::success();
}

Result AlsaRadioBackend::alsaFailure(ErrorCode code, const QString &operation,
                                     int alsaError)
{
    return Result::failure(code,
                           QStringLiteral("%1: %2")
                               .arg(operation, QString::fromLocal8Bit(snd_strerror(alsaError))));
}

Result AlsaRadioBackend::openControl()
{
    if (m_config.device.trimmed().isEmpty())
        return Result::failure(ErrorCode::InvalidParameter,
                               QStringLiteral("ALSA control device is empty"));

    const QByteArray deviceName = m_config.device.toLocal8Bit();
    const int rc = snd_ctl_open(&m_ctl, deviceName.constData(), 0);
    if (rc < 0) {
        m_ctl = nullptr;
        return alsaFailure(ErrorCode::AlsaOpenFailed,
                           QStringLiteral("snd_ctl_open('%1')").arg(m_config.device), rc);
    }
    return Result::success();
}

Result AlsaRadioBackend::initialize()
{
    const Result threadResult = checkThread();
    if (!threadResult.ok())
        return threadResult;
    if (m_initialized)
        return Result::success();

    if (m_ctl) {
        snd_ctl_close(m_ctl);
        m_ctl = nullptr;
    }

    Result result = openControl();
    if (!result.ok())
        return result;

    // tuneControl must be a BYTES control large enough for StRadioTune.
    {
        ControlInfo info;
        result = lookupElement(m_ctl, m_config.tuneControl, 0, info);
        if (!result.ok()) { shutdown(); return result; }
        const auto tuneType = snd_ctl_elem_info_get_type(info.info.get());
        if (tuneType != SND_CTL_ELEM_TYPE_BYTES) {
            result = Result::failure(ErrorCode::AlsaControlTypeUnsupported,
                QStringLiteral("Tune control '%1' must be a BYTES control, got type %2")
                    .arg(m_config.tuneControl).arg(static_cast<int>(tuneType)));
            shutdown();
            return result;
        }
        if (snd_ctl_elem_info_get_count(info.info.get()) < sizeof(StRadioTune)) {
            result = Result::failure(ErrorCode::AlsaControlTypeUnsupported,
                QStringLiteral("Tune control '%1' byte size %2 < required %3")
                    .arg(m_config.tuneControl)
                    .arg(snd_ctl_elem_info_get_count(info.info.get()))
                    .arg(sizeof(StRadioTune)));
            shutdown();
            return result;
        }
    }

    // muteControl must be INTEGER/INTEGER64/BOOLEAN.
    {
        ControlInfo info;
        result = lookupElement(m_ctl, m_config.muteControl, 0, info);
        if (!result.ok()) { shutdown(); return result; }
        const auto muteType = snd_ctl_elem_info_get_type(info.info.get());
        if (muteType != SND_CTL_ELEM_TYPE_INTEGER &&
            muteType != SND_CTL_ELEM_TYPE_INTEGER64 &&
            muteType != SND_CTL_ELEM_TYPE_BOOLEAN) {
            result = Result::failure(ErrorCode::AlsaControlTypeUnsupported,
                QStringLiteral("Mute control '%1' must be INTEGER/BOOLEAN, got type %2")
                    .arg(m_config.muteControl).arg(static_cast<int>(muteType)));
            shutdown();
            return result;
        }
    }

    // regControl is optional; if supplied it must be a BYTES control.
    if (!m_config.regControl.trimmed().isEmpty()) {
        ControlInfo info;
        result = lookupElement(m_ctl, m_config.regControl, 0, info);
        if (!result.ok()) { shutdown(); return result; }
        const auto regType = snd_ctl_elem_info_get_type(info.info.get());
        if (regType != SND_CTL_ELEM_TYPE_BYTES) {
            result = Result::failure(ErrorCode::AlsaControlTypeUnsupported,
                QStringLiteral("Reg control '%1' must be a BYTES control, got type %2")
                    .arg(m_config.regControl).arg(static_cast<int>(regType)));
            shutdown();
            return result;
        }
    }

    m_initialized = true;
    return Result::success();
}

Result AlsaRadioBackend::applyTune()
{
    if (!m_initialized)
        return Result::failure(ErrorCode::HardwareNotReady,
                               QStringLiteral("Hardware not initialized"));

    StRadioTune tune{};
    tune.mode = alsaTuneMode(m_currentTuneMode);
    tune.band = (m_currentBand == Band::AM) ? 1 : 0;
    tune.freq = static_cast<unsigned int>(m_currentFreqKHz / 10);  // kHz -> 10 kHz units

    ControlInfo info;
    Result result = lookupElement(m_ctl, m_config.tuneControl, 0, info);
    if (!result.ok()) return result;

    const auto type = snd_ctl_elem_info_get_type(info.info.get());
    if (type != SND_CTL_ELEM_TYPE_BYTES)
        return Result::failure(ErrorCode::AlsaControlTypeUnsupported,
            QStringLiteral("Tune control '%1' is not a BYTES control").arg(m_config.tuneControl));

    snd_ctl_elem_value_t *rawValue = nullptr;
    int allocRc = snd_ctl_elem_value_malloc(&rawValue);
    if (allocRc < 0)
        return alsaFailure(ErrorCode::HardwareOperationFailed,
                           QStringLiteral("snd_ctl_elem_value_malloc"), allocRc);
    ElemValuePtr elem(rawValue);
    snd_ctl_elem_value_set_id(elem.get(), info.id.get());
    setElemBytes(elem.get(), &tune, sizeof(tune));

    const int rc = snd_ctl_elem_write(m_ctl, elem.get());
    if (rc < 0)
        return alsaFailure(ErrorCode::AlsaWriteFailed,
                           QStringLiteral("snd_ctl_elem_write('%1')").arg(m_config.tuneControl), rc);
    return Result::success();
}

Result AlsaRadioBackend::readTune(StRadioTune &out)
{
    if (!m_initialized)
        return Result::failure(ErrorCode::HardwareNotReady,
                               QStringLiteral("Hardware not initialized"));

    StRadioTune request{};
    request.mode = kModeReadBack;
    request.band = (m_currentBand == Band::AM) ? 1 : 0;
    request.freq = static_cast<unsigned int>(m_currentFreqKHz / 10);  // kHz -> 10 kHz units

    ControlInfo info;
    Result result = lookupElement(m_ctl, m_config.tuneControl, 0, info);
    if (!result.ok()) return result;

    const auto type = snd_ctl_elem_info_get_type(info.info.get());
    if (type != SND_CTL_ELEM_TYPE_BYTES)
        return Result::failure(ErrorCode::AlsaControlTypeUnsupported,
            QStringLiteral("Tune control '%1' is not a BYTES control").arg(m_config.tuneControl));

    snd_ctl_elem_value_t *rawValue = nullptr;
    int allocRc = snd_ctl_elem_value_malloc(&rawValue);
    if (allocRc < 0)
        return alsaFailure(ErrorCode::HardwareOperationFailed,
                           QStringLiteral("snd_ctl_elem_value_malloc"), allocRc);
    ElemValuePtr elem(rawValue);
    snd_ctl_elem_value_set_id(elem.get(), info.id.get());
    setElemBytes(elem.get(), &request, sizeof(request));

    // Write the read-back request, then read the filled struct back.
    int rc = snd_ctl_elem_write(m_ctl, elem.get());
    if (rc < 0)
        return alsaFailure(ErrorCode::AlsaWriteFailed,
                           QStringLiteral("snd_ctl_elem_write('%1')").arg(m_config.tuneControl), rc);

    rc = snd_ctl_elem_read(m_ctl, elem.get());
    if (rc < 0)
        return alsaFailure(ErrorCode::AlsaReadFailed,
                           QStringLiteral("snd_ctl_elem_read('%1')").arg(m_config.tuneControl), rc);

    getElemBytes(elem.get(), &out, sizeof(StRadioTune));
    return Result::success();
}

Result AlsaRadioBackend::writeMute(bool muted)
{
    if (!m_initialized)
        return Result::failure(ErrorCode::HardwareNotReady,
                               QStringLiteral("Hardware not initialized"));
    const long value = muted ? m_config.muteOnValue : m_config.muteOffValue;

    ControlInfo info;
    Result result = lookupElement(m_ctl, m_config.muteControl, 0, info);
    if (!result.ok()) return result;

    const auto type = snd_ctl_elem_info_get_type(info.info.get());
    if (type == SND_CTL_ELEM_TYPE_BOOLEAN) {
        if (value != 0 && value != 1)
            return Result::failure(ErrorCode::InvalidParameter,
                QStringLiteral("Boolean mute control '%1' accepts only 0 or 1").arg(m_config.muteControl));
    } else if (type == SND_CTL_ELEM_TYPE_INTEGER) {
        const long minV = snd_ctl_elem_info_get_min(info.info.get());
        const long maxV = snd_ctl_elem_info_get_max(info.info.get());
        if (value < minV || value > maxV)
            return Result::failure(ErrorCode::InvalidParameter,
                QStringLiteral("Value %1 outside '%2' range [%3, %4]")
                    .arg(value).arg(m_config.muteControl).arg(minV).arg(maxV));
    }

    snd_ctl_elem_value_t *rawValue = nullptr;
    int allocRc = snd_ctl_elem_value_malloc(&rawValue);
    if (allocRc < 0)
        return alsaFailure(ErrorCode::HardwareOperationFailed,
                           QStringLiteral("snd_ctl_elem_value_malloc"), allocRc);
    ElemValuePtr elem(rawValue);
    snd_ctl_elem_value_set_id(elem.get(), info.id.get());
    snd_ctl_elem_value_set_integer(elem.get(), 0, value);

    const int rc = snd_ctl_elem_write(m_ctl, elem.get());
    if (rc < 0)
        return alsaFailure(ErrorCode::AlsaWriteFailed,
                           QStringLiteral("snd_ctl_elem_write('%1')").arg(m_config.muteControl), rc);
    return Result::success();
}

Result AlsaRadioBackend::readMute(bool &muted) const
{
    if (!m_initialized)
        return Result::failure(ErrorCode::HardwareNotReady,
                               QStringLiteral("Hardware not initialized"));
    ControlInfo info;
    Result result = lookupElement(m_ctl, m_config.muteControl, 0, info);
    if (!result.ok()) return result;

    snd_ctl_elem_value_t *rawValue = nullptr;
    int allocRc = snd_ctl_elem_value_malloc(&rawValue);
    if (allocRc < 0)
        return alsaFailure(ErrorCode::HardwareOperationFailed,
                           QStringLiteral("snd_ctl_elem_value_malloc"), allocRc);
    ElemValuePtr elem(rawValue);
    snd_ctl_elem_value_set_id(elem.get(), info.id.get());

    const int rc = snd_ctl_elem_read(m_ctl, elem.get());
    if (rc < 0)
        return alsaFailure(ErrorCode::AlsaReadFailed,
                           QStringLiteral("snd_ctl_elem_read('%1')").arg(m_config.muteControl), rc);

    const long value = snd_ctl_elem_value_get_integer(elem.get(), 0);
    muted = (value == m_config.muteOnValue);
    return Result::success();
}

Result AlsaRadioBackend::setBand(Band band)
{
    const Result threadResult = checkThread();
    if (!threadResult.ok()) return threadResult;
    if (band != Band::FM && band != Band::AM)
        return Result::failure(ErrorCode::InvalidParameter, QStringLiteral("Invalid radio band"));
    m_currentBand = band;
    return applyTune();
}

Result AlsaRadioBackend::tune(int frequencyKHz, TuneMode mode)
{
    const Result threadResult = checkThread();
    if (!threadResult.ok()) return threadResult;
    if (frequencyKHz <= 0)
        return Result::failure(ErrorCode::InvalidParameter, QStringLiteral("Frequency must be positive kHz"));
    m_currentFreqKHz = frequencyKHz;
    m_currentTuneMode = mode;
    return applyTune();
}

Result AlsaRadioBackend::setMute(bool muted)
{
    const Result threadResult = checkThread();
    if (!threadResult.ok()) return threadResult;
    Result result = writeMute(muted);
    if (result.ok()) m_currentMuted = muted;
    return result;
}

Result AlsaRadioBackend::readState(HardwareState &state)
{
    const Result threadResult = checkThread();
    if (!threadResult.ok()) return threadResult;
    if (!m_initialized)
        return Result::failure(ErrorCode::HardwareNotReady, QStringLiteral("Hardware not initialized"));

    // Prefer hardware values; fall back to the last commanded state if the
    // read-back is unavailable or returns implausible data.
    StRadioTune tune{};
    const bool tuned = readTune(tune).ok();

    if (tuned && tune.band == 0)
        state.band = Band::FM;
    else if (tuned && tune.band == 1)
        state.band = Band::AM;
    else
        state.band = m_currentBand;

    if (tuned && tune.freq > 0 &&
        tune.freq <= static_cast<unsigned int>(std::numeric_limits<int>::max()))
        state.frequencyKHz = static_cast<int>(tune.freq) * 10;  // 10 kHz units -> kHz
    else
        state.frequencyKHz = m_currentFreqKHz;

    bool muted = m_currentMuted;
    if (readMute(muted).ok())
        state.muted = muted;
    else
        state.muted = m_currentMuted;

    return Result::success();
}

Result AlsaRadioBackend::readSignalInfo(SignalInfo &info)
{
    const Result threadResult = checkThread();
    if (!threadResult.ok()) return threadResult;
    if (!m_initialized)
        return Result::failure(ErrorCode::HardwareNotReady, QStringLiteral("Hardware not initialized"));

    StRadioTune tune{};
    const Result readResult = readTune(tune);
    if (!readResult.ok()) return readResult;

    info = SignalInfo{};
    info.rssi = static_cast<int>(tune.level);   // level == received signal strength
    info.usn = static_cast<int>(tune.usn);
    info.wam = static_cast<int>(tune.wam);
    info.offset = static_cast<int>(tune.offset);
    info.stereo = (tune.stereo != 0);
    info.hasRssi = true;
    info.hasUsn = true;
    info.hasWam = true;
    info.hasOffset = true;
    info.hasStereo = true;
    return Result::success();
}

void AlsaRadioBackend::shutdown()
{
    if (QThread::currentThread() != thread())
        return;
    m_initialized = false;
    if (m_ctl) {
        snd_ctl_close(m_ctl);
        m_ctl = nullptr;
    }
}

} // namespace HSRadio
