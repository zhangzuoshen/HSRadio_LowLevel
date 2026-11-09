#include "hsradio/hardware/HSRadioSAF775XBackend.h"
#include "hsradio/HSRadioLogging.h"

#include <QThread>

#include <alsa/asoundlib.h>

#include <cstring>
#include <memory>
#include <utility>

namespace HSRadio {
namespace {

// HSRadioSAF775XTune 的 mode 字段。
constexpr unsigned char kModeStandby  = 0x00;
constexpr unsigned char kModePreset   = 0x10; // 调谐到新节目，短静音
constexpr unsigned char kModeSearch   = 0x20; // 调谐到新节目，保持静音
// 读回信号参数不需要下发 0x70 请求：对齐参考驱动 readBytesCtl()，
// 驱动 get 回调在 snd_ctl_elem_read() 时直接返回当前调谐状态与信号参数。
// 若先 write 一次调谐请求，会重新触发 put 回调、废掉锁定等待并读到未稳定值。

// 将硬件无关的 TuneMode 翻译为 SAF775X 的 ALSA
// 控制字节。这里是唯一知晓设备值的地方。
unsigned char alsaTuneMode(IRadioHardware::TuneMode mode) {
    switch (mode) {
    case IRadioHardware::TuneMode::Search: return kModeSearch;
    case IRadioHardware::TuneMode::Preset:
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

// 编译期保护：调谐结构体必须与内核驱动布局一致。
static_assert(sizeof(HSRadioSAF775XTune) == 28, "HSRadioSAF775XTune layout mismatch");
static_assert(sizeof(HSRadioSAF775XRegTrait) == 2, "HSRadioSAF775XRegTrait layout mismatch");

// 对齐参考驱动 RadioControl::writeBytesCtl / readBytesCtl：
//   - snd_ctl_elem_value_alloca 会把整个 elem（含 512 字节 BYTES 区）清零；
//     用 malloc 时尾部是未初始化堆垃圾，驱动按整块解析时行为异常。
//   - 用 set_interface + set_name 定位控件，整块 set_bytes / get_bytes 拷贝。
static void initElemForControl(snd_ctl_elem_value_t *v, const QByteArray &nameBytes) {
    snd_ctl_elem_value_set_interface(v, SND_CTL_ELEM_IFACE_MIXER);
    snd_ctl_elem_value_set_name(v, nameBytes.constData());
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

} // 命名空间

HSRadioSAF775XBackend::HSRadioSAF775XBackend(HSRadioSAF775XConfig config, QObject *parent)
    : QObject(parent), m_config(std::move(config))
{
}

HSRadioSAF775XBackend::~HSRadioSAF775XBackend()
{
    // QObject 实例必须在其所属线程中销毁。
    shutdown();
}

Result HSRadioSAF775XBackend::checkThread() const
{
    if (QThread::currentThread() != thread()) {
        return Result::failure(
            ErrorCode::HardwareOperationFailed,
            QStringLiteral("HSRadioSAF775XBackend called outside its owning thread"));
    }
    return Result::success();
}

Result HSRadioSAF775XBackend::alsaFailure(ErrorCode code, const QString &operation,
                                     int alsaError)
{
    return Result::failure(code,
                           QStringLiteral("%1: %2")
                               .arg(operation, QString::fromLocal8Bit(snd_strerror(alsaError))));
}

Result HSRadioSAF775XBackend::openControl()
{
    if (m_config.device.trimmed().isEmpty())
        return Result::failure(ErrorCode::InvalidParameter,
                               QStringLiteral("ALSA control device is empty"));

    const QByteArray deviceName = m_config.device.toLocal8Bit();
    const int rc = snd_ctl_open(&m_ctl, deviceName.constData(), 0);
    if (rc < 0) {
        m_ctl = nullptr;
        HSRADIO_LOG_WARNING(hsradioHardwareSaf775x).noquote() << "snd_ctl_open failed for device" << m_config.device;
        return alsaFailure(ErrorCode::AlsaOpenFailed,
                           QStringLiteral("snd_ctl_open('%1')").arg(m_config.device), rc);
    }
    return Result::success();
}

Result HSRadioSAF775XBackend::initialize()
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

    // tuneControl 必须是足够容纳 HSRadioSAF775XTune 的 BYTES 控件。
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
        if (snd_ctl_elem_info_get_count(info.info.get()) < sizeof(HSRadioSAF775XTune)) {
            result = Result::failure(ErrorCode::AlsaControlTypeUnsupported,
                QStringLiteral("Tune control '%1' byte size %2 < required %3")
                    .arg(m_config.tuneControl)
                    .arg(snd_ctl_elem_info_get_count(info.info.get()))
                    .arg(sizeof(HSRadioSAF775XTune)));
            shutdown();
            return result;
        }
    }

    // muteControl 必须是 INTEGER/INTEGER64/BOOLEAN。
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

    // regControl 为可选；若提供，必须是 BYTES 控件。
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
    HSRADIO_LOG_INFO(hsradioHardwareSaf775x).noquote() << "initialized; device=" << m_config.device;
    return Result::success();
}

Result HSRadioSAF775XBackend::applyTune()
{
    if (!m_initialized)
        return Result::failure(ErrorCode::HardwareNotReady,
                               QStringLiteral("Hardware not initialized"));

    HSRadioSAF775XTune tune{};
    tune.mode = alsaTuneMode(m_currentTuneMode);
    tune.band = (m_currentBand == Band::AM) ? 1 : 0;
    tune.freq = static_cast<unsigned int>(m_currentFreqKHz / 10);  // kHz → 10 kHz 单位

    // 类型/大小校验在 initialize() 已完成，这里直接按参考 writeBytesCtl 整块写。
    const QByteArray nameBytes = m_config.tuneControl.toLocal8Bit();
    snd_ctl_elem_value_t *elemVal = nullptr;
    snd_ctl_elem_value_alloca(&elemVal);   // 清零整个 elem，避免 BYTES 区尾部堆垃圾
    initElemForControl(elemVal, nameBytes);
    snd_ctl_elem_set_bytes(elemVal, &tune, sizeof(tune));

    HSRADIO_LOG_DEBUG(hsradioHardwareSaf775x).noquote()
        << QStringLiteral("[TUNE-WRITE] mode=0x%1 band=%2 freq=%3 (kHz=%4) bytes=%5")
               .arg(static_cast<unsigned int>(tune.mode), 0, 16)
               .arg(static_cast<unsigned int>(tune.band))
               .arg(tune.freq).arg(m_currentFreqKHz)
               .arg(sizeof(tune));

    const int rc = snd_ctl_elem_write(m_ctl, elemVal);
    if (rc < 0) {
        HSRADIO_LOG_WARNING(hsradioHardwareSaf775x) << "ALSA snd_ctl_elem I/O failed, rc" << rc;
        return alsaFailure(ErrorCode::AlsaWriteFailed,
                           QStringLiteral("snd_ctl_elem_write('%1')").arg(m_config.tuneControl), rc);
    }
    return Result::success();
}

Result HSRadioSAF775XBackend::readTune(HSRadioSAF775XTune &out)
{
    if (!m_initialized)
        return Result::failure(ErrorCode::HardwareNotReady,
                               QStringLiteral("Hardware not initialized"));

    // 对齐参考 RadioControl::readBytesCtl()：alloca 清零整个 elem，
    // set_interface + set_name 定位，纯 snd_ctl_elem_read，整块 memcpy 读回。
    const QByteArray nameBytes = m_config.tuneControl.toLocal8Bit();
    snd_ctl_elem_value_t *elemVal = nullptr;
    snd_ctl_elem_value_alloca(&elemVal);
    initElemForControl(elemVal, nameBytes);

    const int rc = snd_ctl_elem_read(m_ctl, elemVal);
    if (rc < 0) {
        HSRADIO_LOG_WARNING(hsradioHardwareSaf775x) << "ALSA snd_ctl_elem I/O failed, rc" << rc;
        return alsaFailure(ErrorCode::AlsaReadFailed,
                           QStringLiteral("snd_ctl_elem_read('%1')").arg(m_config.tuneControl), rc);
    }

    const void *rawPtr = snd_ctl_elem_value_get_bytes(elemVal);
    if (!rawPtr)
        return Result::failure(ErrorCode::AlsaReadFailed,
                               QStringLiteral("snd_ctl_elem_value_get_bytes('%1') returned null")
                                   .arg(m_config.tuneControl));

    std::memcpy(&out, rawPtr, sizeof(HSRadioSAF775XTune));
    return Result::success();
}

Result HSRadioSAF775XBackend::writeMute(bool muted)
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
    if (rc < 0) {
        HSRADIO_LOG_WARNING(hsradioHardwareSaf775x) << "ALSA snd_ctl_elem I/O failed, rc" << rc;
        return alsaFailure(ErrorCode::AlsaWriteFailed,
                           QStringLiteral("snd_ctl_elem_write('%1')").arg(m_config.muteControl), rc);
    }
    return Result::success();
}

Result HSRadioSAF775XBackend::readMute(bool &muted) const
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
    if (rc < 0) {
        HSRADIO_LOG_WARNING(hsradioHardwareSaf775x) << "ALSA snd_ctl_elem I/O failed, rc" << rc;
        return alsaFailure(ErrorCode::AlsaReadFailed,
                           QStringLiteral("snd_ctl_elem_read('%1')").arg(m_config.muteControl), rc);
    }

    const long value = snd_ctl_elem_value_get_integer(elem.get(), 0);
    muted = (value == m_config.muteOnValue);
    return Result::success();
}

Result HSRadioSAF775XBackend::setBand(Band band)
{
    const Result threadResult = checkThread();
    if (!threadResult.ok()) return threadResult;
    if (band != Band::FM && band != Band::AM)
        return Result::failure(ErrorCode::InvalidParameter, QStringLiteral("Invalid radio band"));
    HSRADIO_LOG_INFO(hsradioHardwareSaf775x).noquote() << "setBand" << (band == Band::AM ? "AM" : "FM");
    m_currentBand = band;
    return applyTune();
}

Result HSRadioSAF775XBackend::tune(int frequencyKHz, TuneMode mode)
{
    const Result threadResult = checkThread();
    if (!threadResult.ok()) return threadResult;
    if (frequencyKHz <= 0)
        return Result::failure(ErrorCode::InvalidParameter, QStringLiteral("Frequency must be positive kHz"));
    m_currentFreqKHz = frequencyKHz;
    m_currentTuneMode = mode;
    HSRADIO_LOG_INFO(hsradioHardwareSaf775x).noquote() << "tune" << frequencyKHz << "kHz mode="
                                             << (mode == IRadioHardware::TuneMode::Search ? "search" : "preset");
    return applyTune();
}

Result HSRadioSAF775XBackend::setMute(bool muted)
{
    const Result threadResult = checkThread();
    if (!threadResult.ok()) return threadResult;
    HSRADIO_LOG_INFO(hsradioHardwareSaf775x).noquote() << "setMute" << muted;
    Result result = writeMute(muted);
    if (result.ok()) m_currentMuted = muted;
    return result;
}

Result HSRadioSAF775XBackend::readState(HardwareState &state)
{
    const Result threadResult = checkThread();
    if (!threadResult.ok()) return threadResult;
    if (!m_initialized)
        return Result::failure(ErrorCode::HardwareNotReady, QStringLiteral("Hardware not initialized"));

    // 驱动在 read-back 模式下不返回频率/频段（freq 固定为 0xFFFF），
    // 这两项是写入侧参数，直接以后端最近一次下发值为准。
    // readTune() 只供 readSignalInfo() 读取立体声、信号强度等信号字段。
    state.band = m_currentBand;
    state.frequencyKHz = m_currentFreqKHz;

    bool muted = m_currentMuted;
    if (readMute(muted).ok())
        state.muted = muted;
    else
        state.muted = m_currentMuted;

    return Result::success();
}

Result HSRadioSAF775XBackend::readSignalInfo(SignalInfo &info)
{
    const Result threadResult = checkThread();
    if (!threadResult.ok()) return threadResult;
    if (!m_initialized)
        return Result::failure(ErrorCode::HardwareNotReady, QStringLiteral("Hardware not initialized"));

    HSRadioSAF775XTune tune{};
    const Result readResult = readTune(tune);
    if (!readResult.ok()) return readResult;

    info = SignalInfo{};
    info.rssi = static_cast<int>(tune.level);   // level 即接收信号强度
    info.usn = static_cast<int>(tune.usn);
    info.wam = static_cast<int>(tune.wam);
    info.offset = static_cast<int>(tune.offset);
    info.modulation = static_cast<int>(tune.modulation);   // 解调质量（AM 判定用）
    info.stereo = (tune.stereo != 0);
    info.hasRssi = true;
    info.hasUsn = true;
    info.hasWam = true;
    info.hasOffset = true;
    info.hasModulation = true;
    info.hasStereo = true;

    HSRADIO_LOG_DEBUG(hsradioHardwareSaf775x).noquote()
        << QStringLiteral("[SIGNAL] freq=%1 kHz band=%2 mode=%3 | rssi=%4 usn=%5 wam=%6 offset=%7 mod=%8 stereo=%9")
               .arg(m_currentFreqKHz)
               .arg(m_currentBand == Band::AM ? QStringLiteral("AM") : QStringLiteral("FM"))
               .arg(static_cast<unsigned int>(alsaTuneMode(m_currentTuneMode)), 0, 16)
               .arg(info.rssi).arg(info.usn).arg(info.wam)
               .arg(info.offset).arg(info.modulation)
               .arg(info.stereo ? QStringLiteral("yes") : QStringLiteral("no"));
    return Result::success();
}

void HSRadioSAF775XBackend::shutdown()
{
    if (QThread::currentThread() != thread())
        return;
    m_initialized = false;
    if (m_ctl) {
        snd_ctl_close(m_ctl);
        m_ctl = nullptr;
    }
}

} // HSRadio 命名空间
