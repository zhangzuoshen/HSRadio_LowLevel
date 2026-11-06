#pragma once

#include "hsradio/HSRadioTypes.h"

#include <QString>

namespace HSRadio {

// 配置仓库：负责 radio_config.json 的读写与内存镜像，
// 涵盖地区(region)、各频段范围/步进/默认频率、搜台阈值与时序。
// 恢复出厂 = 用 default 模板（或内置默认）覆盖 config 并落盘。
class HSRadioRepository {
public:
    struct BandConfig {
        int minKHz = 87500;
        int maxKHz = 108000;
        int stepKHz = 100;
        int defaultKHz = 87500;
    };

    // defaultConfigFilePath 为空时，按 configFilePath + ".default" 推导。
    explicit HSRadioRepository(const QString &configFilePath,
                              const QString &defaultConfigFilePath = QString());

    Result load();
    // 写回磁盘；policy 决定 .bak 的生成/保留/清除（默认生成）。
    Result save(JsonBackupPolicy policy = JsonBackupPolicy::Create);
    Result resetToDefault();      // 用 default 模板（或内置默认）覆盖并落盘

    QString region() const;
    void setRegion(const QString &region);

    BandConfig bandConfig(Band band) const;
    void setBandConfig(Band band, const BandConfig &cfg);

    SeekThreshold seekThreshold() const;
    void setSeekThreshold(const SeekThreshold &th);

    bool seekUpward() const;
    void setSeekUpward(bool upward);

    bool seekWrapAround() const;
    void setSeekWrapAround(bool wrap);

    int seekLockMs() const;
    void setSeekLockMs(int ms);

    int seekSampleMs() const;
    void setSeekSampleMs(int ms);

    int seekSampleCount() const;
    void setSeekSampleCount(int n);

    int seekMaxInvalid() const;
    void setSeekMaxInvalid(int n);

    // 依据当前配置构造一次搜台请求（填充频段边界与时序，muteDuringSeek 默认 true）。
    SeekRequest makeSeekRequest(Band band, bool upward, SeekMode mode,
                               int startKHz) const;

private:
    void setBuiltInDefaults();
    void parseFromJson(const QJsonObject &root);

    QString m_configFilePath;
    QString m_defaultConfigFilePath;

    QString      m_region = QStringLiteral("CN");
    BandConfig   m_bandFM{87500, 108000, 100, 87500};
    BandConfig   m_bandAM{531, 1602, 9, 531};
    SeekThreshold m_threshold;    // 默认 21/25/25/40（见 HSRadioTypes）
    bool m_seekUpward = true;
    bool m_seekWrapAround = true;
    int  m_seekLockMs = 8;
    int  m_seekSampleMs = 2;
    int  m_seekSampleCount = 4;
    int  m_seekMaxInvalid = 3;
};

} // namespace HSRadio
