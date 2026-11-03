#pragma once

#include "hsradio/hardware/IRadioHardware.h"

#include <QObject>
#include <QString>

struct _snd_ctl;
typedef struct _snd_ctl snd_ctl_t;

namespace HSRadio {

// SAF775X 的 ALSA 驱动暴露三个 mixer 控件。这些名称
// 与板级/驱动相关，由平台集成层提供。
//   - tuneControl：SND_CTL_ELEM_TYPE_BYTES。承载调谐请求结构体
//                   （频段 + 频率 + 模式），并在读回时返回信号信息
//                   读回请求之后。
//   - muteControl：SND_CTL_ELEM_TYPE_INTEGER/BOOLEAN。0 = 取消静音，1 = 静音。
//   - regControl：SND_CTL_ELEM_TYPE_BYTES（可选；Phase 1 不在范围内）。
//                   寄存器读写（reg + data）。
//
// 这里故意不采用理想化的“每个参数一个整型控件”模型
// ：SAF775X 将频段、频率与信号状态打包进单个
// BYTES 二进制块中，因此后端直接讲驱动的二进制协议。
struct HSRadioSAF775XConfig {
    QString device = QStringLiteral("default");

    QString tuneControl = QStringLiteral("Radiocontrol");
    QString muteControl = QStringLiteral("Mute_radio");
    QString regControl; // 可选，例如 "Radioregvalue"

    long muteOffValue = 0;
    long muteOnValue = 1;
};

// SAF775X 调谐请求/响应的二进制布局。字节布局必须
// 与内核驱动的期望完全一致。自然对齐（不使用
// #pragma pack）在 x86-64 与 aarch64 上均为 28 字节；.cpp 中的静态断言
// 用于防止与目标驱动不一致。
struct HSRadioSAF775XTune {
    unsigned char mode = 0x10;  // 参见下方 SAF775X 模式常量
    unsigned char band = 0;     // 0 = FM，1 = AM
    unsigned int freq = 0;     // 10 kHz 单位（8700 = 87.00 MHz）；原样打包进 ALSA
    unsigned int usn = 0;
    unsigned char wam = 0;
    unsigned int offset = 0;
    unsigned int level = 0;     // 接收信号强度（RSSI）
    unsigned char modulation = 0;
    unsigned char stereo = 0;
};

// SAF775X 寄存器特征（供可选 regControl 使用）。#pragma pack(2)
// 与驱动布局一致；2 字节。
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

} // HSRadio 命名空间
