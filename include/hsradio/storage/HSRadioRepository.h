#pragma once

#include "hsradio/HSRadioTypes.h"

#include <QString>

namespace HSRadio {

// 配置仓库：负责 radio_config.json 的读写与内存镜像，
// 涵盖地区(region)、各频段范围/步进/默认频率、搜台阈值与时序。
// 默认值全部由代码内置（setBuiltInDefaults），不依赖外部模板文件。
class HSRadioRepository {
public:
    struct BandConfig {
        int minKHz = 87500;
        int maxKHz = 108000;
        int stepKHz = 100;
        int defaultKHz = 87500;
    };

    explicit HSRadioRepository(const QString &configFilePath);

    Result load();
    // 写回磁盘；policy 决定 .bak 的生成/保留/清除（默认生成）。
    Result save(JsonBackupPolicy policy = JsonBackupPolicy::Create);
    Result resetToDefault();      // 用内置默认覆盖并落盘

    QString region() const;
    void setRegion(const QString &region);

    BandConfig bandConfig(Band band) const;
    void setBandConfig(Band band, const BandConfig &cfg);

    SeekThreshold fmSeekThreshold() const;
    void setFmSeekThreshold(const SeekThreshold &th);

    SeekThreshold amSeekThreshold() const;
    void setAmSeekThreshold(const SeekThreshold &th);

    bool seekUpward() const;
    void setSeekUpward(bool upward);

    bool seekWrapAround() const;
    void setSeekWrapAround(bool wrap);

    // 搜台锁定等待按频段区分：FM 8ms / AM 60ms。
    int seekLockMs(Band band) const;
    void setSeekLockMs(Band band, int ms);

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

    QString      m_region = QStringLiteral("CN");
    BandConfig   m_bandFM{87500, 108000, 100, 87500};
    BandConfig   m_bandAM{531, 1602, 9, 531};
    // 出厂默认值直接在此定义，改 HSRadioTypes.h 的两个常量即可全局调整。
    SeekThreshold m_fmThreshold = kDefaultFmSeekThreshold;   // 25/25/25/40, mod=0, lock=8ms
    SeekThreshold m_amThreshold = kDefaultAmSeekThreshold;   // 30/0/0/20, mod=50, lock=60ms
    bool m_seekUpward = true;
    bool m_seekWrapAround = true;
    int  m_seekSampleMs = 2;
    int  m_seekSampleCount = 4;
    int  m_seekMaxInvalid = 3;
};

} // namespace HSRadio
