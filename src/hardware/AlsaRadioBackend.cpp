#include "hsradio/hardware/AlsaRadioBackend.h"

#include <QThread>

#include <alsa/asoundlib.h>

#include <limits>
#include <memory>
#include <utility>

namespace HSRadio {
namespace {

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

Result lookupElement(snd_ctl_t *ctl, const AlsaIntegerControl &control,
                     ControlInfo &out)
{
    if (!ctl)
        return Result::failure(ErrorCode::HardwareNotReady,
                               QStringLiteral("ALSA control device is not open"));
    if (control.name.trimmed().isEmpty())
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

    const QByteArray name = control.name.toLocal8Bit();
    snd_ctl_elem_id_set_interface(out.id.get(), SND_CTL_ELEM_IFACE_MIXER);
    snd_ctl_elem_id_set_name(out.id.get(), name.constData());
    snd_ctl_elem_id_set_index(out.id.get(), control.index);
    snd_ctl_elem_info_set_id(out.info.get(), out.id.get());

    rc = snd_ctl_elem_info(ctl, out.info.get());
    if (rc < 0) {
        return Result::failure(
            ErrorCode::AlsaControlNotFound,
            QStringLiteral("ALSA control '%1' (index %2): %3")
                .arg(control.name).arg(control.index)
                .arg(QString::fromLocal8Bit(snd_strerror(rc))));
    }

    const auto type = snd_ctl_elem_info_get_type(out.info.get());
    if (type != SND_CTL_ELEM_TYPE_INTEGER &&
        type != SND_CTL_ELEM_TYPE_INTEGER64 &&
        type != SND_CTL_ELEM_TYPE_BOOLEAN) {
        return Result::failure(
            ErrorCode::AlsaControlTypeUnsupported,
            QStringLiteral("ALSA control '%1' has unsupported type %2")
                .arg(control.name).arg(static_cast<int>(type)));
    }

    if (snd_ctl_elem_info_get_count(out.info.get()) < 1) {
        return Result::failure(ErrorCode::AlsaControlTypeUnsupported,
                               QStringLiteral("ALSA control '%1' has no values")
                                   .arg(control.name));
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

Result AlsaRadioBackend::validateControl(const AlsaIntegerControl &control) const
{
    ControlInfo handle;
    return lookupElement(m_ctl, control, handle);
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

    if (m_config.fmBandValue == m_config.amBandValue) {
        return Result::failure(ErrorCode::InvalidParameter,
                               QStringLiteral("FM and AM ALSA band values must be different"));
    }
    if (m_config.muteOnValue == m_config.muteOffValue) {
        return Result::failure(ErrorCode::InvalidParameter,
                               QStringLiteral("Mute-on and mute-off ALSA values must be different"));
    }

    Result result = openControl();
    if (!result.ok())
        return result;

    const AlsaIntegerControl *requiredControls[] = {
        &m_config.band, &m_config.frequencyKHz, &m_config.mute
    };
    for (const auto *control : requiredControls) {
        result = validateControl(*control);
        if (!result.ok()) {
            shutdown();
            return result;
        }
    }

    // Signal controls are optional. A missing optional control does not make
    // basic tuning unavailable; readSignalInfo() reports only available data.
    m_hasRssi = validateOptionalControl(m_config.rssi);
    m_hasUsn = validateOptionalControl(m_config.usn);
    m_hasWam = validateOptionalControl(m_config.wam);
    m_hasOffset = validateOptionalControl(m_config.offset);
    m_hasStereo = validateOptionalControl(m_config.stereo);

    m_initialized = true;
    return Result::success();
}

bool AlsaRadioBackend::validateOptionalControl(const AlsaIntegerControl &control) const
{
    if (control.name.trimmed().isEmpty())
        return false;
    return validateControl(control).ok();
}

Result AlsaRadioBackend::readInteger(const AlsaIntegerControl &control,
                                     long long &value) const
{
    ControlInfo handle;
    Result result = lookupElement(m_ctl, control, handle);
    if (!result.ok())
        return result;

    snd_ctl_elem_value_t *rawValue = nullptr;
    const int allocRc = snd_ctl_elem_value_malloc(&rawValue);
    if (allocRc < 0)
        return alsaFailure(ErrorCode::HardwareOperationFailed,
                           QStringLiteral("snd_ctl_elem_value_malloc"), allocRc);
    ElemValuePtr elem(rawValue);
    snd_ctl_elem_value_set_id(elem.get(), handle.id.get());

    const int rc = snd_ctl_elem_read(m_ctl, elem.get());
    if (rc < 0)
        return alsaFailure(ErrorCode::AlsaReadFailed,
                           QStringLiteral("snd_ctl_elem_read('%1')").arg(control.name), rc);

    const auto type = snd_ctl_elem_info_get_type(handle.info.get());
    if (type == SND_CTL_ELEM_TYPE_INTEGER64)
        value = snd_ctl_elem_value_get_integer64(elem.get(), 0);
    else
        value = snd_ctl_elem_value_get_integer(elem.get(), 0);
    return Result::success();
}

Result AlsaRadioBackend::writeInteger(const AlsaIntegerControl &control,
                                      long long value) const
{
    ControlInfo handle;
    Result result = lookupElement(m_ctl, control, handle);
    if (!result.ok())
        return result;

    const auto type = snd_ctl_elem_info_get_type(handle.info.get());
    if (type == SND_CTL_ELEM_TYPE_BOOLEAN) {
        if (value != 0 && value != 1)
            return Result::failure(ErrorCode::InvalidParameter,
                                   QStringLiteral("Boolean ALSA control '%1' accepts only 0 or 1")
                                       .arg(control.name));
    } else if (type == SND_CTL_ELEM_TYPE_INTEGER) {
        const long minValue = snd_ctl_elem_info_get_min(handle.info.get());
        const long maxValue = snd_ctl_elem_info_get_max(handle.info.get());
        if (value < minValue || value > maxValue)
            return Result::failure(ErrorCode::InvalidParameter,
                                   QStringLiteral("Value %1 outside '%2' range [%3, %4]")
                                       .arg(value).arg(control.name).arg(minValue).arg(maxValue));
    } else if (type == SND_CTL_ELEM_TYPE_INTEGER64) {
        const long long minValue = snd_ctl_elem_info_get_min64(handle.info.get());
        const long long maxValue = snd_ctl_elem_info_get_max64(handle.info.get());
        if (value < minValue || value > maxValue)
            return Result::failure(ErrorCode::InvalidParameter,
                                   QStringLiteral("Value %1 outside '%2' range [%3, %4]")
                                       .arg(value).arg(control.name).arg(minValue).arg(maxValue));
    } else {
        return Result::failure(ErrorCode::AlsaControlTypeUnsupported,
                               QStringLiteral("Unsupported ALSA control type: %1")
                                   .arg(control.name));
    }

    snd_ctl_elem_value_t *rawValue = nullptr;
    const int allocRc = snd_ctl_elem_value_malloc(&rawValue);
    if (allocRc < 0)
        return alsaFailure(ErrorCode::HardwareOperationFailed,
                           QStringLiteral("snd_ctl_elem_value_malloc"), allocRc);
    ElemValuePtr elem(rawValue);
    snd_ctl_elem_value_set_id(elem.get(), handle.id.get());
    if (type == SND_CTL_ELEM_TYPE_INTEGER64)
        snd_ctl_elem_value_set_integer64(elem.get(), 0, value);
    else
        snd_ctl_elem_value_set_integer(elem.get(), 0, static_cast<long>(value));

    const int rc = snd_ctl_elem_write(m_ctl, elem.get());
    if (rc < 0)
        return alsaFailure(ErrorCode::AlsaWriteFailed,
                           QStringLiteral("snd_ctl_elem_write('%1')").arg(control.name), rc);
    return Result::success();
}

Result AlsaRadioBackend::setBand(Band band)
{
    const Result threadResult = checkThread();
    if (!threadResult.ok()) return threadResult;
    if (!m_initialized)
        return Result::failure(ErrorCode::HardwareNotReady, QStringLiteral("Hardware not initialized"));

    if (band != Band::FM && band != Band::AM)
        return Result::failure(ErrorCode::InvalidParameter, QStringLiteral("Invalid radio band"));
    return writeInteger(m_config.band, band == Band::FM ? m_config.fmBandValue : m_config.amBandValue);
}

Result AlsaRadioBackend::tune(int frequencyKHz)
{
    const Result threadResult = checkThread();
    if (!threadResult.ok()) return threadResult;
    if (!m_initialized)
        return Result::failure(ErrorCode::HardwareNotReady, QStringLiteral("Hardware not initialized"));
    if (frequencyKHz <= 0)
        return Result::failure(ErrorCode::InvalidParameter, QStringLiteral("Frequency must be positive kHz"));
    return writeInteger(m_config.frequencyKHz, frequencyKHz);
}

Result AlsaRadioBackend::setMute(bool muted)
{
    const Result threadResult = checkThread();
    if (!threadResult.ok()) return threadResult;
    if (!m_initialized)
        return Result::failure(ErrorCode::HardwareNotReady, QStringLiteral("Hardware not initialized"));
    return writeInteger(m_config.mute, muted ? m_config.muteOnValue : m_config.muteOffValue);
}

Result AlsaRadioBackend::readState(HardwareState &state)
{
    const Result threadResult = checkThread();
    if (!threadResult.ok()) return threadResult;
    if (!m_initialized)
        return Result::failure(ErrorCode::HardwareNotReady, QStringLiteral("Hardware not initialized"));

    long long bandValue = 0, frequency = 0, muteValue = 0;
    Result result = readInteger(m_config.band, bandValue);
    if (!result.ok()) return result;
    result = readInteger(m_config.frequencyKHz, frequency);
    if (!result.ok()) return result;
    result = readInteger(m_config.mute, muteValue);
    if (!result.ok()) return result;

    if (bandValue == m_config.fmBandValue)
        state.band = Band::FM;
    else if (bandValue == m_config.amBandValue)
        state.band = Band::AM;
    else
        return Result::failure(ErrorCode::HardwareOperationFailed,
                               QStringLiteral("Unknown band value read from ALSA: %1").arg(bandValue));

    if (frequency <= 0 || frequency > std::numeric_limits<int>::max())
        return Result::failure(ErrorCode::HardwareOperationFailed,
                               QStringLiteral("Invalid frequency read from ALSA: %1").arg(frequency));
    state.frequencyKHz = static_cast<int>(frequency);
    state.muted = (muteValue == m_config.muteOnValue);
    return Result::success();
}

Result AlsaRadioBackend::readSignalInfo(SignalInfo &info)
{
    const Result threadResult = checkThread();
    if (!threadResult.ok()) return threadResult;
    if (!m_initialized)
        return Result::failure(ErrorCode::HardwareNotReady, QStringLiteral("Hardware not initialized"));

    info = SignalInfo{};
    struct Field { const AlsaIntegerControl *control; bool available; int *target; bool *present; };
    const Field fields[] = {
        {&m_config.rssi, m_hasRssi, &info.rssi, &info.hasRssi},
        {&m_config.usn, m_hasUsn, &info.usn, &info.hasUsn},
        {&m_config.wam, m_hasWam, &info.wam, &info.hasWam},
        {&m_config.offset, m_hasOffset, &info.offset, &info.hasOffset}
    };
    for (const Field &field : fields) {
        if (!field.available) continue;
        long long value = 0;
        const Result result = readInteger(*field.control, value);
        if (!result.ok()) return result;
        if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
            return Result::failure(ErrorCode::HardwareOperationFailed,
                                   QStringLiteral("Signal value out of int range: %1").arg(value));
        *field.target = static_cast<int>(value);
        *field.present = true;
    }
    if (m_hasStereo) {
        long long value = 0;
        const Result result = readInteger(m_config.stereo, value);
        if (!result.ok()) return result;
        info.stereo = (value != 0);
        info.hasStereo = true;
    }
    return Result::success();
}

void AlsaRadioBackend::shutdown()
{
    if (QThread::currentThread() != thread())
        return;
    m_initialized = false;
    m_hasRssi = m_hasUsn = m_hasWam = m_hasOffset = m_hasStereo = false;
    if (m_ctl) {
        snd_ctl_close(m_ctl);
        m_ctl = nullptr;
    }
}

} // namespace HSRadio
