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

// 请求标识：每次异步硬件操作返回的唯一编号。
using RequestId = quint64;

// 搜台模式。
enum class SeekMode : quint8 {
    NextStation = 0,   // 从当前频率起找到下一个有效台即停
    FullBand = 1       // 扫描整个频段，结果直接写入该频段电台列表
};

// 电台（电台列表与收藏共用；收藏自包含，可挂不在列表中的手动调谐频率）。
struct Station {
    QString id;                  // 唯一 ID（程序生成，如 "fm-87500"）
    Band    band = Band::FM;     // 所属频段
    int     frequencyKHz = 0;    // 频率，kHz
    QString name;                // 名称，可为空
    QString source;              // 来源："seek" | "manual"
};

// 搜台信号质量阈值。
struct SeekThreshold {
    int minRssi = 21;
    int maxUsn = 25;
    int maxWam = 25;
    int maxOffset = 40;
};

// 一次搜台请求的完整参数（由 Manager 依配置构造，交给调度器执行）。
struct SeekRequest {
    Band          band = Band::FM;
    bool          upward = true;
    SeekMode      mode = SeekMode::FullBand;
    int           startKHz = 0;
    int           minKHz = 0;
    int           maxKHz = 0;
    int           stepKHz = 0;
    SeekThreshold threshold;
    int           lockMs = 8;            // PLL 锁定时间
    int           sampleMs = 2;          // 单次抽样间隔
    int           sampleCount = 4;       // 抽样次数
    int           maxInvalid = 3;        // 连续无效则放弃
    bool          wrapAround = true;     // 到边界是否回绕
    bool          muteDuringSeek = true; // 搜台期间强制静音，结束恢复
};

} // namespace HSRadio

Q_DECLARE_METATYPE(HSRadio::Band)
Q_DECLARE_METATYPE(HSRadio::ErrorCode)
Q_DECLARE_METATYPE(HSRadio::Result)
Q_DECLARE_METATYPE(HSRadio::SignalInfo)
Q_DECLARE_METATYPE(HSRadio::HardwareState)
Q_DECLARE_METATYPE(HSRadio::Station)
Q_DECLARE_METATYPE(HSRadio::SeekMode)
Q_DECLARE_METATYPE(HSRadio::SeekThreshold)
Q_DECLARE_METATYPE(HSRadio::SeekRequest)
