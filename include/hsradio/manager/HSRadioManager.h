#pragma once

#include "hsradio/HSRadioTypes.h"

#include <QObject>
#include <QVector>

namespace HSRadio {

class HSRadioOperationScheduler;
class HSRadioStationStore;
class HSRadioBandConfig;

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
                   HSRadioBandConfig *config,
                   QObject *parent = nullptr);
    ~HSRadioManager() override;

    // —— 生命周期 ——
    Result initialize();     // 同步：加载配置/状态；硬件初始化入队，就绪发 initialized
    void   shutdown();       // 停止任务、保存状态、释放资源
    Result restoreLastState();
    Result factoryReset();   // 状态回默认 + 配置回出厂模板

    // —— 查询（同步，无硬件 I/O）——
    bool       isInitialized() const;
    bool       isSearching() const;
    Band       currentBand() const;
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

signals:
    void initialized(ErrorCode code);
    void stateChanged(const HardwareState &state);
    void bandChanged(Band band);
    void frequencyChanged(int frequencyKHz);
    void signalInfoChanged(const SignalInfo &info);
    void seekStarted(quint64 id, bool upward);
    void seekProgress(quint64 id, int frequencyKHz);
    void seekFinished(quint64 id, int frequencyKHz, bool found);
    void stationsChanged(Band band);
    void favoritesChanged();
    void operationFinished(quint64 id, ErrorCode code);
    void errorOccurred(ErrorCode code, const QString &message);

private:
    HSRadioOperationScheduler *m_scheduler = nullptr;
    HSRadioStationStore      *m_store = nullptr;
    HSRadioBandConfig        *m_config = nullptr;
};

} // namespace HSRadio
