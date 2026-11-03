#pragma once

#include "hsradio/hardware/IRadioHardware.h"

#include <QMap>

namespace HSRadio {

// MockRadioHardware：IRadioHardware 的纯内存假实现。
// 不依赖 ALSA、不访问任何设备，用于在没有真机（例如 Ubuntu 开发机）时
// 编译与调试业务层（Manager / Scheduler / 存储）。
//
// 行为：
//   - tune() 仅记录当前频率与调谐模式；
//   - readSignalInfo() 依据预置的"频点 -> 信号"表返回，
//     未预置的频点返回明显不达标的弱信号；
//   - 可注入 tune 故障，便于测试错误处理。
class MockRadioHardware : public IRadioHardware {
public:
    MockRadioHardware();
    ~MockRadioHardware() override = default;

    // IRadioHardware 实现
    Result initialize() override;
    Result setBand(Band band) override;
    Result tune(int frequencyKHz, TuneMode mode) override;
    Result setMute(bool muted) override;
    Result readState(HardwareState &state) override;
    Result readSignalInfo(SignalInfo &info) override;
    void   shutdown() override;

    // —— 测试辅助 ——
    // 预置某频点为有效电台及其信号质量。
    void setStationSignal(int frequencyKHz, const SignalInfo &info);
    void clearStations();
    // 注入 tune 故障：非 None 时此后所有 tune 返回该错误；传 None 解除。
    void setTuneFailure(ErrorCode code);
    // 观测点：最近一次调谐模式、静音调用次数（用于验证搜台强制静音/恢复）。
    TuneMode lastTuneMode() const { return m_lastTuneMode; }
    int      muteCallCount() const { return m_muteCallCount; }
    void     resetObservations();

private:
    bool m_initialized = false;
    Band m_band = Band::FM;
    int  m_frequencyKHz = 87000;
    bool m_muted = false;

    QMap<int, SignalInfo> m_stations;   // 频点 -> 信号质量
    ErrorCode m_tuneFailure = ErrorCode::None;

    TuneMode m_lastTuneMode = TuneMode::Preset;
    int      m_muteCallCount = 0;
};

} // namespace HSRadio
