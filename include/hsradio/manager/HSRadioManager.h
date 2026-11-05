#pragma once

#include "hsradio/HSRadioTypes.h"

#include <QObject>
#include <QSet>

class QTimer;

namespace HSRadio {

class HSRadioOperationScheduler;
class HSRadioStationStore;
class HSRadioRepository;

// 业务门面：运行在主线程，对外提供收音机全部业务能力。
// 约定：
//   - 碰硬件的操作一律异步：提交给调度器，立即返回 RequestId，
//     完成后通过信号通知（operationFinished / seekFinished 等）。
//   - 纯数据操作（电台/收藏增删改查）同步返回 Result，仅访问存储层。
class HSRadioManager : public QObject {
    Q_OBJECT
public:
    HSRadioManager(HSRadioOperationScheduler *scheduler,
                   HSRadioStationStore *store,
                   HSRadioRepository *repository,
                   QObject *parent = nullptr);
    ~HSRadioManager() override;

    // —— 生命周期 ——
    Result initialize();     // 同步：加载配置/状态 + 信号接线；硬件初始化由上层 scheduler.initialize 完成
    void   shutdown();       // 停定时器、保存状态、释放资源
    // 异步恢复：返 RequestId，完成由 restoreFinished 通知；
    // 内部按 SetBand → Tune → ReadState 下发，只下发频点、不解除静音，
    // 是否恢复播放由 HMI 音源管理决定。
    RequestId restoreLastState();
    Result factoryReset();   // 状态回默认 + 配置回出厂模板

    // —— 查询（同步，无硬件 I/O）——
    bool       isInitialized() const;
    bool       isSearching() const;
    // 记忆频率是否已真正下发到硬件（调谐命令完成）；false 时下列值只是磁盘记忆值。
    bool       isHardwareTuned() const;
    Band       currentBand() const;
    // 未下发调谐前为记忆/目标频率，不代表射频前端实际频点。
    int        currentFrequencyKHz() const;
    SignalInfo lastSignalInfo() const;

    // —— 硬件操作（异步，返回 RequestId）——
    RequestId setBand(Band band);
    RequestId setFrequency(int frequencyKHz);
    RequestId stepUp();          // 频率 + 步进，越界回绕
    RequestId stepDown();
    RequestId startSeek(bool upward, SeekMode mode = SeekMode::FullBand);
    RequestId stopSeek();        // 立即停止并返回当前频点
    RequestId setMute(bool muted);
    RequestId refreshSignal();

    // —— 电台列表（同步，纯数据）——
    QVector<Station> stations(Band band) const;
    Result addStation(const Station &station);
    Result removeStation(const QString &id);
    Result renameStation(const QString &id, const QString &name);
    Result clearStations(Band band);

    // —— 收藏（同步；收藏自包含，可挂不在列表中的手动频率）——
    QVector<Station> favorites(Band band) const;
    Result addFavorite(const Station &station);
    Result removeFavorite(const QString &id);
    bool   isFavorite(const QString &id) const;

// 信号形参一律写全限定名：moc 按书写形式记录元类型名，
// 写短名会让跨线程队列连接按 "ErrorCode" 等无命名空间名查找而静默失败。
signals:
    void initialized(HSRadio::ErrorCode code);
    void stateChanged(const HSRadio::HardwareState &state);
    void bandChanged(HSRadio::Band band);
    void frequencyChanged(int frequencyKHz);
    void signalInfoChanged(const HSRadio::SignalInfo &info);
    void restoreFinished(quint64 id, HSRadio::ErrorCode code,
                         HSRadio::Band band, int frequencyKHz);
    void seekStarted(quint64 id, bool upward);
    void seekProgress(quint64 id, int frequencyKHz);
    void seekFinished(quint64 id, int frequencyKHz, bool found);
    void stationsChanged(HSRadio::Band band);
    void favoritesChanged();
    void operationFinished(quint64 id, HSRadio::ErrorCode code);
    void errorOccurred(HSRadio::ErrorCode code, const QString &message);

private:
    void connectScheduler();
    void enqueueReadState();
    // 把磁盘记忆频率对齐到当前配置的频段：越界时回退到该频段 defaultKHz。
    // 只读判断，不写 store；是否落盘由调用方决定。
    int normalizedFrequency(Band band, int storedKHz) const;

    HSRadioOperationScheduler *m_scheduler = nullptr;
    HSRadioStationStore      *m_store = nullptr;
    HSRadioRepository        *m_repository = nullptr;

    bool      m_initialized = false;
    bool      m_searching = false;
    // 缓存频率是否已真正下发到硬件；未下发时 stateChanged 原样上报后端自报状态。
    bool      m_hardwareTuned = false;
    Band      m_currentBand = Band::FM;
    int       m_currentFrequencyKHz = 0;
    bool      m_muted = false;
    SignalInfo m_lastSignalInfo;
    bool      m_currentSeekUpward = true;
    quint64   m_lastSeekId = 0;
    QTimer   *m_refreshTimer = nullptr;

    // 已提交、尚未执行完成的调谐类命令；只有其 operationFinished 到达才置 hardwareTuned。
    QSet<RequestId> m_pendingTuneIds;
    // 恢复序列的三条命令，聚合错误码，最后一条完成才发 restoreFinished。
    QSet<RequestId> m_restoreIds;
    RequestId m_restoreId = 0;
    ErrorCode m_restoreCode = ErrorCode::None;
};

} // namespace HSRadio
