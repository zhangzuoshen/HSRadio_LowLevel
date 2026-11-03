#pragma once

#include "hsradio/HSRadioTypes.h"

namespace HSRadio {

// 所有方法均为同步调用，且必须从硬件对象的
// 所属线程发起。管理器/调度器应使用队列调用，以确保
// ALSA I/O 不会阻塞服务主线程。
class IRadioHardware {
public:
    // 调谐模式选择驱动的调谐行为。
    //   - Preset：调谐到一个新节目并短静音（正常收听）。
    //   - Search：调谐并保持静音（用于扫描搜台）。
    enum class TuneMode : quint8 { Preset, Search };

    virtual ~IRadioHardware() = default;
    virtual Result initialize() = 0;
    virtual Result setBand(Band band) = 0;
    virtual Result tune(int frequencyKHz, TuneMode mode = TuneMode::Preset) = 0;
    virtual Result setMute(bool muted) = 0;
    virtual Result readState(HardwareState &state) = 0;
    virtual Result readSignalInfo(SignalInfo &info) = 0;
    virtual void shutdown() = 0;
};

} // HSRadio 命名空间
