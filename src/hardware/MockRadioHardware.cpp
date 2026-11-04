#include "hsradio/hardware/MockRadioHardware.h"
#include "hsradio/HSRadioLogging.h"

namespace HSRadio {

namespace {

// 未预置频点的默认"弱信号"：明显低于常规阈值
// （rssi >= 21、usn <= 25、wam <= 25、offset <= 40），判为无台。
SignalInfo weakSignal()
{
    SignalInfo info;
    info.rssi = 0;
    info.usn = 60;
    info.wam = 60;
    info.offset = 80;
    info.stereo = false;
    info.hasRssi = true;
    info.hasUsn = true;
    info.hasWam = true;
    info.hasOffset = true;
    info.hasStereo = true;
    return info;
}

} // namespace

MockRadioHardware::MockRadioHardware() = default;

Result MockRadioHardware::initialize()
{
    m_initialized = true;
    HSRADIO_LOG_INFO(hsradioHardwareMock).noquote() << "mock backend initialized";
    return {};
}

Result MockRadioHardware::setBand(Band band)
{
    if (!m_initialized)
        return Result::failure(ErrorCode::HardwareNotReady,
                               QStringLiteral("mock not initialized"));
    HSRADIO_LOG_DEBUG(hsradioHardwareMock).noquote() << "setBand" << (band == Band::AM ? "AM" : "FM");
    m_band = band;
    return {};
}

Result MockRadioHardware::tune(int frequencyKHz, TuneMode mode)
{
    if (!m_initialized)
        return Result::failure(ErrorCode::HardwareNotReady,
                               QStringLiteral("mock not initialized"));
    if (m_tuneFailure != ErrorCode::None)
        return Result::failure(m_tuneFailure, QStringLiteral("mock tune failure"));
    m_frequencyKHz = frequencyKHz;
    m_lastTuneMode = mode;
    HSRADIO_LOG_DEBUG(hsradioHardwareMock).noquote() << "tune" << frequencyKHz << "kHz mode="
                                          << (mode == TuneMode::Search ? "search" : "preset");
    return {};
}

Result MockRadioHardware::setMute(bool muted)
{
    if (!m_initialized)
        return Result::failure(ErrorCode::HardwareNotReady,
                               QStringLiteral("mock not initialized"));
    ++m_muteCallCount;
    m_muted = muted;
    HSRADIO_LOG_DEBUG(hsradioHardwareMock).noquote() << "setMute" << muted << "callCount=" << m_muteCallCount;
    return {};
}

Result MockRadioHardware::readState(HardwareState &state)
{
    if (!m_initialized)
        return Result::failure(ErrorCode::HardwareNotReady,
                               QStringLiteral("mock not initialized"));
    state.band = m_band;
    state.frequencyKHz = m_frequencyKHz;
    state.muted = m_muted;
    return {};
}

Result MockRadioHardware::readSignalInfo(SignalInfo &info)
{
    if (!m_initialized)
        return Result::failure(ErrorCode::HardwareNotReady,
                               QStringLiteral("mock not initialized"));
    const auto it = m_stations.constFind(m_frequencyKHz);
    info = (it != m_stations.constEnd()) ? it.value() : weakSignal();
    return {};
}

void MockRadioHardware::shutdown()
{
    m_initialized = false;
}

void MockRadioHardware::setStationSignal(int frequencyKHz, const SignalInfo &info)
{
    m_stations.insert(frequencyKHz, info);
}

void MockRadioHardware::clearStations()
{
    m_stations.clear();
}

void MockRadioHardware::setTuneFailure(ErrorCode code)
{
    m_tuneFailure = code;
    HSRADIO_LOG_WARNING(hsradioHardwareMock).noquote() << "tune failure injected:" << int(code);
}

void MockRadioHardware::resetObservations()
{
    m_lastTuneMode = TuneMode::Preset;
    m_muteCallCount = 0;
}

} // namespace HSRadio
