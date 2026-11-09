#pragma once

#include <QMetaType>
#include <QString>

namespace HSRadio {

// JSON 状态文件写入时的备份策略。
enum class JsonBackupPolicy : quint8 {
    Create = 0,   // 普通写入：commit 前把上一版主文件复制为 .bak
    KeepExisting, // 跳过备份生成、保留已有 .bak（从 .bak 恢复后重建主文件）
    Purge         // 跳过备份生成、commit 成功后删除 .bak（恢复出厂）
};

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
    int modulation = 0;     // 解调质量（真机/SAF775X 驱动填充）
    bool stereo = false;

    bool hasRssi = false;
    bool hasUsn = false;
    bool hasWam = false;
    bool hasOffset = false;
    bool hasModulation = false;
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

// 搜台信号质量阈值：FM / AM 分两组，判定口径不同。
//   FM：rssi >= minRssi 且 usn <= maxUsn 且 wam <= maxWam 且 offset <= maxOffset
//   AM：rssi >= minRssi 且 offset <= maxOffset 且 modulation <= maxModulation
// 注：AM 的 usn/wam 不参与判定（与参考驱动一致）。
// 默认值对齐参考驱动 RadioControl::setFmThreshold / setAmThreshold。
struct FmSeekThreshold {
    int minRssi = 25;
    int maxUsn = 25;
    int maxWam = 25;
    int maxOffset = 40;
};

struct AmSeekThreshold {
    int minRssi = 30;
    int maxOffset = 20;
    int maxModulation = 50;
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
    int           lockMs = 8;            // PLL 锁定时间
    int           sampleMs = 2;          // 单次抽样间隔
    int           sampleCount = 4;       // 抽样次数
    int           maxInvalid = 3;        // 连续无效则放弃
    bool          wrapAround = true;     // 到边界是否回绕
    bool          muteDuringSeek = true; // 搜台期间强制静音，结束恢复

    // 频段专属阈值：判定时按 band 选用对应一组。
    FmSeekThreshold fmThreshold;
    AmSeekThreshold amThreshold;
};

} // namespace HSRadio

Q_DECLARE_METATYPE(HSRadio::Band)
Q_DECLARE_METATYPE(HSRadio::ErrorCode)
Q_DECLARE_METATYPE(HSRadio::Result)
Q_DECLARE_METATYPE(HSRadio::SignalInfo)
Q_DECLARE_METATYPE(HSRadio::HardwareState)
Q_DECLARE_METATYPE(HSRadio::Station)
Q_DECLARE_METATYPE(HSRadio::SeekMode)
Q_DECLARE_METATYPE(HSRadio::FmSeekThreshold)
Q_DECLARE_METATYPE(HSRadio::AmSeekThreshold)
Q_DECLARE_METATYPE(HSRadio::SeekRequest)

// 运行期元类型注册：跨线程（调度器工作线程 -> 主线程）信号携带自定义
// 类型时，Qt 队列连接要求目标类型已通过 qRegisterMetaType 注册，
// 仅 Q_DECLARE_METATYPE 不足以支持排队连接。
// 信号形参统一写全限定名（HSRadio::X），moc 元数据也记录全名，
// 与 qRegisterMetaType<HSRadio::X>() 注册的名字一致。
// 下面额外注册的无命名空间别名仅为兼容旧式 SIGNAL/SLOT 字符串连接
// 与第三方按短名查找元类型的场景，按项目约定保留。
inline void registerHSRadioMetaTypes()
{
    qRegisterMetaType<HSRadio::Band>();
    qRegisterMetaType<HSRadio::ErrorCode>();
    qRegisterMetaType<HSRadio::Result>();
    qRegisterMetaType<HSRadio::SignalInfo>();
    qRegisterMetaType<HSRadio::HardwareState>();
    qRegisterMetaType<HSRadio::Station>();
    qRegisterMetaType<HSRadio::SeekMode>();
    qRegisterMetaType<HSRadio::FmSeekThreshold>();
    qRegisterMetaType<HSRadio::AmSeekThreshold>();
    qRegisterMetaType<HSRadio::SeekRequest>();
    qRegisterMetaType<HSRadio::Band>("Band");
    qRegisterMetaType<HSRadio::ErrorCode>("ErrorCode");
    qRegisterMetaType<HSRadio::Result>("Result");
    qRegisterMetaType<HSRadio::SignalInfo>("SignalInfo");
    qRegisterMetaType<HSRadio::HardwareState>("HardwareState");
    qRegisterMetaType<HSRadio::Station>("Station");
    qRegisterMetaType<HSRadio::SeekMode>("SeekMode");
    qRegisterMetaType<HSRadio::FmSeekThreshold>("FmSeekThreshold");
    qRegisterMetaType<HSRadio::AmSeekThreshold>("AmSeekThreshold");
    qRegisterMetaType<HSRadio::SeekRequest>("SeekRequest");
}
