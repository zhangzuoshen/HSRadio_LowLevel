#include "hsradio/manager/HSRadioManager.h"

#include "hsradio/hardware/IRadioHardware.h"
#include "hsradio/scheduler/HSRadioOperationScheduler.h"
#include "hsradio/storage/HSRadioStationStore.h"
#include "hsradio/storage/HSRadioRepository.h"

#include <QTimer>
#include <QtGlobal>
#include <QDebug>
#include "hsradio/HSRadioLogging.h"

namespace HSRadio {

HSRadioManager::HSRadioManager(HSRadioOperationScheduler *scheduler,
                               HSRadioStationStore *store,
                               HSRadioRepository *repository,
                               QObject *parent)
    : QObject(parent)
    , m_scheduler(scheduler)
    , m_store(store)
    , m_repository(repository)
{
}

HSRadioManager::~HSRadioManager()
{
    if (m_refreshTimer)
        m_refreshTimer->stop();
}

int HSRadioManager::normalizedFrequency(Band band, int storedKHz) const
{
    if (!m_repository)
        return storedKHz;
    const auto bc = m_repository->bandConfig(band);
    if (bc.minKHz > bc.maxKHz)   // 配置异常：不动用户状态，避免把坏配置固化进 state
        return storedKHz;
    if (storedKHz >= bc.minKHz && storedKHz <= bc.maxKHz)
        return storedKHz;

    const int fallback = qBound(bc.minKHz, bc.defaultKHz, bc.maxKHz);
    HSRADIO_LOG_WARNING(hsradioManager).noquote()
        << QStringLiteral("remembered frequency %1 kHz out of [%2,%3], fallback to default %4 kHz")
               .arg(storedKHz).arg(bc.minKHz).arg(bc.maxKHz).arg(fallback);
    return fallback;
}

Result HSRadioManager::initialize()
{
    if (!m_store || !m_repository)
        return Result::failure(ErrorCode::InvalidParameter,
                               QStringLiteral("存储层未注入"));

    const Result storeRes = m_store->load();
    const Result repoRes = m_repository->load();
    if (!storeRes.ok())
        return storeRes;
    if (!repoRes.ok())
        return repoRes;

    m_currentBand = m_store->currentBand();
    // 只把磁盘记忆值作为 UI 显示与后续操作的目标频率；开机不向硬件下发调谐，
    // 是否恢复收听由 HMI 音源管理决定。硬件此时已由调度器强制静音。
    // 越界记忆值回退到该频段 defaultKHz（仅改内存，不为此写盘）。
    m_currentFrequencyKHz = normalizedFrequency(m_currentBand,
                                                m_store->lastFrequency(m_currentBand));
    m_hardwareTuned = false;

    connectScheduler();

    // 800ms 周期刷新信号质量（搜台期间暂停；尚未下发调谐时不读，
    // 避免把驱动上电频点的信号当成记忆频点的信号广播）。
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(800);
    QObject::connect(m_refreshTimer, &QTimer::timeout, this, [this] {
        if (m_hardwareTuned && !m_searching && m_scheduler)
            m_scheduler->submitReadSignal();
    });
    m_refreshTimer->start();

    // 广播一次初始硬件状态（含开机强制静音），让 HMI 知道当前是静音的。
    enqueueReadState();

    m_initialized = true;
    HSRADIO_LOG_INFO(hsradioManager).noquote() << "initialized; band=" << (m_currentBand == Band::AM ? "AM" : "FM")
                                      << "remembered freq=" << m_currentFrequencyKHz << "kHz"
                                      << "hardwareTuned=false";
    return Result::success();
}

void HSRadioManager::connectScheduler()
{
    if (!m_scheduler)
        return;

    QObject::connect(m_scheduler, &HSRadioOperationScheduler::initialized,
                     this, [this](ErrorCode code) { emit initialized(code); });

    QObject::connect(m_scheduler, &HSRadioOperationScheduler::operationFinished,
                     this, [this](quint64 id, ErrorCode code) {
                         emit operationFinished(id, code);

                         // 只有调谐类命令真正执行完成，缓存频率才算已下发到硬件；
                         // 被 shutdown 丢弃的命令会收到 HardwareNotReady，不置位。
                         if (m_pendingTuneIds.remove(id) && code == ErrorCode::None)
                             m_hardwareTuned = true;

                         // 聚合恢复序列中每条命令的错误码，最后一条完成才通知上层。
                         if (m_restoreIds.remove(id) && code != ErrorCode::None
                             && m_restoreCode == ErrorCode::None) {
                             m_restoreCode = code;
                         }
                         if (m_restoreId && id == m_restoreId) {
                             const RequestId done = m_restoreId;
                             const ErrorCode finalCode = m_restoreCode;
                             m_restoreId = 0;
                             m_restoreCode = ErrorCode::None;
                             m_restoreIds.clear();
                             emit restoreFinished(done, finalCode, m_currentBand,
                                                  m_currentFrequencyKHz);
                         }
                     });

    QObject::connect(m_scheduler, &HSRadioOperationScheduler::stateRead,
                     this, [this](quint64, const HardwareState &state) {
                         // 读回只用于同步静音状态。已下发过调谐时以 Manager 设置值为准
                         // （保留批3决策）；尚未下发时原样上报后端自报状态，
                         // 不把记忆频率伪装成硬件当前频点。
                         m_muted = state.muted;
                         HardwareState current = state;
                         if (m_hardwareTuned) {
                             current.band = m_currentBand;
                             current.frequencyKHz = m_currentFrequencyKHz;
                         }
                         emit stateChanged(current);
                     });

    QObject::connect(m_scheduler, &HSRadioOperationScheduler::signalRead,
                     this, [this](quint64, const SignalInfo &info) {
                         m_lastSignalInfo = info;
                         emit signalInfoChanged(info);
                     });

    QObject::connect(m_scheduler, &HSRadioOperationScheduler::errorOccurred,
                     this, &HSRadioManager::errorOccurred);

    QObject::connect(m_scheduler, &HSRadioOperationScheduler::seekStarted,
                     this, [this](quint64 id) {
                         m_searching = true;
                         emit seekStarted(id, m_currentSeekUpward);
                     });

    QObject::connect(m_scheduler, &HSRadioOperationScheduler::seekProgress,
                     this, &HSRadioManager::seekProgress);

    QObject::connect(m_scheduler, &HSRadioOperationScheduler::seekHit,
                     this, [this](quint64, int frequencyKHz, const SignalInfo &info) {
                         Q_UNUSED(info);
                         Station s;
                         s.band = m_currentBand;
                         s.frequencyKHz = frequencyKHz;
                         s.source = QStringLiteral("seek");
                         s.id = HSRadioStationStore::makeStationId(s.band, frequencyKHz);
                         if (!s.id.isEmpty()) {
                             m_store->addStation(s);   // upsert
                             emit stationsChanged(s.band);
                         }
                     });

    QObject::connect(m_scheduler, &HSRadioOperationScheduler::seekFinished,
                     this, [this](quint64 id, int frequencyKHz, bool found) {
                         m_searching = false;
                         m_currentFrequencyKHz = frequencyKHz;
                         m_store->setLastFrequency(m_currentBand, frequencyKHz);
                         m_store->save();
                         emit seekFinished(id, frequencyKHz, found);
                     });
}

void HSRadioManager::enqueueReadState()
{
    if (m_scheduler)
        m_scheduler->submitReadState();
}

void HSRadioManager::shutdown()
{
    if (m_refreshTimer)
        m_refreshTimer->stop();
    // 保存失败必须显式告警，避免上层误以为状态已经落盘。
    if (m_store) {
        const Result result = m_store->save();
        if (!result.ok())
            HSRADIO_LOG_WARNING(hsradioManager).noquote()
                << "state save failed:" << result.message;
    }
    if (m_repository) {
        const Result result = m_repository->save();
        if (!result.ok())
            HSRADIO_LOG_WARNING(hsradioManager).noquote()
                << "config save failed:" << result.message;
    }
}

RequestId HSRadioManager::restoreLastState()
{
    if (!m_scheduler || !m_store || !m_repository)
        return 0;

    const Band band = m_store->currentBand();
    const int stored = m_store->lastFrequency(band);
    const int freq = normalizedFrequency(band, stored);

    m_currentBand = band;
    m_currentFrequencyKHz = freq;

    // 只在越界修正时才写回：正常开机恢复不产生落盘与 .bak 覆盖。
    // 保存失败记入恢复序列的错误码，让 restoreFinished 对上层可见。
    ErrorCode initialCode = ErrorCode::None;
    if (freq != stored) {
        m_store->setLastFrequency(band, freq);
        const Result saved = m_store->save();
        if (!saved.ok()) {
            initialCode = saved.code;
            HSRADIO_LOG_WARNING(hsradioManager).noquote()
                << "state save failed after frequency fallback:" << saved.message;
        }
    }
    HSRADIO_LOG_INFO(hsradioManager).noquote()
        << "restoreLastState; band=" << (band == Band::AM ? "AM" : "FM")
        << "freq=" << freq << "kHz";
    emit bandChanged(band);
    emit frequencyChanged(freq);

    // 显式三步：SetBand → Tune → ReadState。读回放最后，
    // 既作为「完成」标志，也避免读到调谐前的旧状态。
    // 只下发频点，不解除静音；是否恢复播放由 HMI 音源管理决定。
    const RequestId bandId = m_scheduler->submitSetBand(band);
    const RequestId tuneId = m_scheduler->submitTune(freq, IRadioHardware::TuneMode::Preset);
    const RequestId stateId = m_scheduler->submitReadState();
    if (!bandId || !tuneId || !stateId)
        return 0;

    m_restoreIds.clear();
    m_restoreIds << bandId << tuneId << stateId;
    m_restoreCode = initialCode;
    m_restoreId = stateId;
    m_pendingTuneIds.insert(tuneId);
    return stateId;
}

Result HSRadioManager::factoryReset()
{
    HSRADIO_LOG_INFO(hsradioManager).noquote() << "factoryReset";
    if (!m_store || !m_repository)
        return Result::failure(ErrorCode::InvalidParameter,
                               QStringLiteral("存储层未注入"));
    m_store->resetToDefault();
    m_repository->resetToDefault();
    m_currentBand = m_store->currentBand();
    const int stored = m_store->lastFrequency(m_currentBand);
    const int freq = normalizedFrequency(m_currentBand, stored);
    if (freq != stored)
        m_store->setLastFrequency(m_currentBand, freq);
    m_store->save();
    m_repository->save();
    m_currentFrequencyKHz = freq;
    m_hardwareTuned = false;   // 恢复出厂只改数据，硬件仍在原频点
    m_pendingTuneIds.clear();
    emit bandChanged(m_currentBand);
    emit frequencyChanged(m_currentFrequencyKHz);
    emit stationsChanged(Band::FM);
    emit stationsChanged(Band::AM);
    emit favoritesChanged();
    return Result::success();
}

bool HSRadioManager::isInitialized() const { return m_initialized; }
bool HSRadioManager::isSearching() const { return m_searching; }
bool HSRadioManager::isHardwareTuned() const { return m_hardwareTuned; }
Band HSRadioManager::currentBand() const { return m_currentBand; }
int  HSRadioManager::currentFrequencyKHz() const { return m_currentFrequencyKHz; }
SignalInfo HSRadioManager::lastSignalInfo() const { return m_lastSignalInfo; }

RequestId HSRadioManager::setBand(Band band)
{
    if (!m_scheduler || !m_store)
        return 0;
    HSRADIO_LOG_INFO(hsradioManager).noquote() << "setBand" << (band == Band::AM ? "AM" : "FM");
    m_store->setCurrentBand(band);
    const int stored = m_store->lastFrequency(band);
    const int freq = normalizedFrequency(band, stored);
    if (freq != stored)
        m_store->setLastFrequency(band, freq);
    m_store->save();
    m_currentBand = band;
    emit bandChanged(band);
    m_currentFrequencyKHz = freq;
    emit frequencyChanged(freq);
    const RequestId id = m_scheduler->submitSetBand(band);
    // 只下发频段，后端 applyTune 用的是自身缓存频率，缓存频率仍未下发，不置位。
    enqueueReadState();
    return id;
}

RequestId HSRadioManager::setFrequency(int frequencyKHz)
{
    if (!m_scheduler || !m_store || !m_repository)
        return 0;
    HSRADIO_LOG_INFO(hsradioManager).noquote() << "setFrequency" << frequencyKHz << "kHz";
    const auto bc = m_repository->bandConfig(m_currentBand);
    int target = frequencyKHz;
    if (target < bc.minKHz || target > bc.maxKHz) {
        target = qBound(bc.minKHz, target, bc.maxKHz);
        HSRADIO_LOG_WARNING(hsradioManager).noquote()
            << QStringLiteral("frequency %1 clamped to [%2,%3]")
                   .arg(frequencyKHz).arg(bc.minKHz).arg(bc.maxKHz);
    }
    m_store->setLastFrequency(m_currentBand, target);
    m_store->save();
    m_currentFrequencyKHz = target;
    emit frequencyChanged(target);
    const RequestId id = m_scheduler->submitTune(target, IRadioHardware::TuneMode::Preset);
    if (id)
        m_pendingTuneIds.insert(id);   // 等 operationFinished 到达才置 hardwareTuned
    enqueueReadState();
    return id;
}

RequestId HSRadioManager::stepUp()
{
    if (!m_repository)
        return 0;
    const auto bc = m_repository->bandConfig(m_currentBand);
    int next = m_currentFrequencyKHz + bc.stepKHz;
    if (next > bc.maxKHz)
        next = bc.minKHz;
    return setFrequency(next);
}

RequestId HSRadioManager::stepDown()
{
    if (!m_repository)
        return 0;
    const auto bc = m_repository->bandConfig(m_currentBand);
    int prev = m_currentFrequencyKHz - bc.stepKHz;
    if (prev < bc.minKHz)
        prev = bc.maxKHz;
    return setFrequency(prev);
}

RequestId HSRadioManager::startSeek(bool upward, SeekMode mode)
{
    if (!m_scheduler || !m_store || !m_repository)
        return 0;
    HSRADIO_LOG_INFO(hsradioManager).noquote() << "startSeek mode=" << (mode == SeekMode::FullBand ? "full" : "next")
                                     << "upward=" << upward;
    const Band band = m_currentBand;
    const int startKHz = m_currentFrequencyKHz;
    if (mode == SeekMode::FullBand)
        m_store->clearStations(band);   // 全频段扫描：先清空再边扫边写
    m_currentSeekUpward = upward;
    const SeekRequest request =
        m_repository->makeSeekRequest(band, upward, mode, startKHz);
    m_lastSeekId = m_scheduler->submitSeek(request);
    if (m_lastSeekId)
        m_pendingTuneIds.insert(m_lastSeekId);   // 搜台会 setBand 并逐频点 tune
    return m_lastSeekId;
}

RequestId HSRadioManager::stopSeek()
{
    HSRADIO_LOG_INFO(hsradioManager).noquote() << "stopSeek";
    if (m_scheduler)
        m_scheduler->cancelSeek();
    return m_lastSeekId;   // 取消结果经 seekFinished 返回，无独立 RequestId 语义
}

RequestId HSRadioManager::setMute(bool muted)
{
    if (!m_scheduler)
        return 0;
    HSRADIO_LOG_INFO(hsradioManager).noquote() << "setMute" << muted;
    m_muted = muted;
    const RequestId id = m_scheduler->submitSetMute(muted);
    enqueueReadState();
    return id;
}

RequestId HSRadioManager::refreshSignal()
{
    if (!m_scheduler)
        return 0;
    return m_scheduler->submitReadSignal();
}

QVector<Station> HSRadioManager::stations(Band band) const
{
    return m_store ? m_store->stations(band) : QVector<Station>();
}

Result HSRadioManager::addStation(const Station &station)
{
    if (!m_store)
        return Result::failure(ErrorCode::InvalidParameter,
                               QStringLiteral("存储层未注入"));
    Station s = station;
    if (s.id.isEmpty())
        s.id = HSRadioStationStore::makeStationId(s.band, s.frequencyKHz);
    const Result r = m_store->addStation(s);
    if (r.ok()) {
        m_store->save();
        emit stationsChanged(s.band);
    }
    return r;
}

Result HSRadioManager::removeStation(const QString &id)
{
    if (!m_store)
        return Result::failure(ErrorCode::InvalidParameter,
                               QStringLiteral("存储层未注入"));
    Band band = Band::FM;
    for (Band b : {Band::FM, Band::AM})
        for (const Station &s : m_store->stations(b))
            if (s.id == id) { band = b; break; }
    const Result r = m_store->removeStation(id);
    if (r.ok()) {
        m_store->save();
        emit stationsChanged(band);
    }
    return r;
}

Result HSRadioManager::renameStation(const QString &id, const QString &name)
{
    if (!m_store)
        return Result::failure(ErrorCode::InvalidParameter,
                               QStringLiteral("存储层未注入"));
    Band band = Band::FM;
    for (Band b : {Band::FM, Band::AM})
        for (const Station &s : m_store->stations(b))
            if (s.id == id) { band = b; break; }
    const Result r = m_store->renameStation(id, name);
    if (r.ok()) {
        m_store->save();
        emit stationsChanged(band);
    }
    return r;
}

Result HSRadioManager::clearStations(Band band)
{
    if (!m_store)
        return Result::failure(ErrorCode::InvalidParameter,
                               QStringLiteral("存储层未注入"));
    const Result r = m_store->clearStations(band);
    if (r.ok()) {
        m_store->save();
        emit stationsChanged(band);
    }
    return r;
}

QVector<Station> HSRadioManager::favorites(Band band) const
{
    return m_store ? m_store->favorites(band) : QVector<Station>();
}

Result HSRadioManager::addFavorite(const Station &station)
{
    if (!m_store)
        return Result::failure(ErrorCode::InvalidParameter,
                               QStringLiteral("存储层未注入"));
    Station s = station;
    if (s.id.isEmpty())
        s.id = HSRadioStationStore::makeStationId(s.band, s.frequencyKHz);
    const Result r = m_store->addFavorite(s);
    if (r.ok()) {
        m_store->save();
        emit favoritesChanged();
    }
    return r;
}

Result HSRadioManager::removeFavorite(const QString &id)
{
    if (!m_store)
        return Result::failure(ErrorCode::InvalidParameter,
                               QStringLiteral("存储层未注入"));
    const Result r = m_store->removeFavorite(id);
    if (r.ok()) {
        m_store->save();
        emit favoritesChanged();
    }
    return r;
}

bool HSRadioManager::isFavorite(const QString &id) const
{
    return m_store ? m_store->isFavorite(id) : false;
}

} // namespace HSRadio
