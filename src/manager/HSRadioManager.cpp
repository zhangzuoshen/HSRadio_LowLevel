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
    m_currentFrequencyKHz = m_store->lastFrequency(m_currentBand);

    connectScheduler();

    // 800ms 周期刷新信号质量（搜台期间暂停，避免干扰扫描读数）。
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(800);
    QObject::connect(m_refreshTimer, &QTimer::timeout, this, [this] {
        if (!m_searching && m_scheduler)
            m_scheduler->submitReadSignal();
    });
    m_refreshTimer->start();

    m_initialized = true;
    qCInfo(hsradioManager).noquote() << "initialized; band=" << (m_currentBand == Band::AM ? "AM" : "FM")
                                      << "freq=" << m_currentFrequencyKHz << "kHz";
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
                     });

    QObject::connect(m_scheduler, &HSRadioOperationScheduler::stateRead,
                     this, [this](quint64, const HardwareState &state) {
                         m_currentBand = state.band;
                         m_currentFrequencyKHz = state.frequencyKHz;
                         m_muted = state.muted;
                         emit stateChanged(state);
                         emit bandChanged(state.band);
                         emit frequencyChanged(state.frequencyKHz);
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
    if (m_store)
        m_store->save();
    if (m_repository)
        m_repository->save();
}

Result HSRadioManager::restoreLastState()
{
    qCInfo(hsradioManager).noquote() << "restoreLastState";
    if (!m_store || !m_scheduler)
        return Result::failure(ErrorCode::HardwareNotReady,
                               QStringLiteral("未初始化或调度器缺失"));
    const Band band = m_store->currentBand();
    const int freq = m_store->lastFrequency(band);
    m_currentBand = band;
    m_currentFrequencyKHz = freq;
    emit bandChanged(band);
    emit frequencyChanged(freq);
    m_scheduler->submitSetBand(band);
    enqueueReadState();
    setFrequency(freq);
    return Result::success();
}

Result HSRadioManager::factoryReset()
{
    qCInfo(hsradioManager).noquote() << "factoryReset";
    if (!m_store || !m_repository)
        return Result::failure(ErrorCode::InvalidParameter,
                               QStringLiteral("存储层未注入"));
    m_store->resetToDefault();
    m_repository->resetToDefault();
    m_store->save();
    m_repository->save();
    m_currentBand = m_store->currentBand();
    m_currentFrequencyKHz = m_store->lastFrequency(m_currentBand);
    emit bandChanged(m_currentBand);
    emit frequencyChanged(m_currentFrequencyKHz);
    emit stationsChanged(Band::FM);
    emit stationsChanged(Band::AM);
    emit favoritesChanged();
    return Result::success();
}

bool HSRadioManager::isInitialized() const { return m_initialized; }
bool HSRadioManager::isSearching() const { return m_searching; }
Band HSRadioManager::currentBand() const { return m_currentBand; }
int  HSRadioManager::currentFrequencyKHz() const { return m_currentFrequencyKHz; }
SignalInfo HSRadioManager::lastSignalInfo() const { return m_lastSignalInfo; }

RequestId HSRadioManager::setBand(Band band)
{
    if (!m_scheduler || !m_store)
        return 0;
    qCInfo(hsradioManager).noquote() << "setBand" << (band == Band::AM ? "AM" : "FM");
    m_store->setCurrentBand(band);
    m_store->save();
    m_currentBand = band;
    emit bandChanged(band);
    m_currentFrequencyKHz = m_store->lastFrequency(band);
    emit frequencyChanged(m_currentFrequencyKHz);
    const RequestId id = m_scheduler->submitSetBand(band);
    enqueueReadState();
    return id;
}

RequestId HSRadioManager::setFrequency(int frequencyKHz)
{
    if (!m_scheduler || !m_store || !m_repository)
        return 0;
    qCInfo(hsradioManager).noquote() << "setFrequency" << frequencyKHz << "kHz";
    const auto bc = m_repository->bandConfig(m_currentBand);
    int target = frequencyKHz;
    if (target < bc.minKHz || target > bc.maxKHz) {
        target = qBound(bc.minKHz, target, bc.maxKHz);
        qCWarning(hsradioManager).noquote()
            << QStringLiteral("frequency %1 clamped to [%2,%3]")
                   .arg(frequencyKHz).arg(bc.minKHz).arg(bc.maxKHz);
    }
    m_store->setLastFrequency(m_currentBand, target);
    m_store->save();
    m_currentFrequencyKHz = target;
    emit frequencyChanged(target);
    const RequestId id = m_scheduler->submitTune(target, IRadioHardware::TuneMode::Preset);
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
    qCInfo(hsradioManager).noquote() << "startSeek mode=" << (mode == SeekMode::FullBand ? "full" : "next")
                                     << "upward=" << upward;
    const Band band = m_currentBand;
    const int startKHz = m_currentFrequencyKHz;
    if (mode == SeekMode::FullBand)
        m_store->clearStations(band);   // 全频段扫描：先清空再边扫边写
    m_currentSeekUpward = upward;
    const SeekRequest request =
        m_repository->makeSeekRequest(band, upward, mode, startKHz);
    m_lastSeekId = m_scheduler->submitSeek(request);
    return m_lastSeekId;
}

RequestId HSRadioManager::stopSeek()
{
    qCInfo(hsradioManager).noquote() << "stopSeek";
    if (m_scheduler)
        m_scheduler->cancelSeek();
    return m_lastSeekId;   // 取消结果经 seekFinished 返回，无独立 RequestId 语义
}

RequestId HSRadioManager::setMute(bool muted)
{
    if (!m_scheduler)
        return 0;
    qCInfo(hsradioManager).noquote() << "setMute" << muted;
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
