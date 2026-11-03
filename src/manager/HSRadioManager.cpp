#include "hsradio/manager/HSRadioManager.h"

namespace HSRadio {

// 批 1 桩实现：仅保证接口可编译、可链接，具体业务逻辑后续实现。

HSRadioManager::HSRadioManager(HSRadioOperationScheduler *scheduler,
                               HSRadioStationStore *store,
                               HSRadioBandConfig *config,
                               QObject *parent)
    : QObject(parent)
    , m_scheduler(scheduler)
    , m_store(store)
    , m_config(config)
{
}

HSRadioManager::~HSRadioManager() = default;

Result HSRadioManager::initialize() { return {}; }
void   HSRadioManager::shutdown() {}
Result HSRadioManager::restoreLastState() { return {}; }
Result HSRadioManager::factoryReset() { return {}; }

bool       HSRadioManager::isInitialized() const { return false; }
bool       HSRadioManager::isSearching() const { return false; }
Band       HSRadioManager::currentBand() const { return Band::FM; }
int        HSRadioManager::currentFrequencyKHz() const { return 0; }
SignalInfo HSRadioManager::lastSignalInfo() const { return {}; }

RequestId HSRadioManager::setBand(Band) { return 0; }
RequestId HSRadioManager::setFrequency(int) { return 0; }
RequestId HSRadioManager::stepUp() { return 0; }
RequestId HSRadioManager::stepDown() { return 0; }
RequestId HSRadioManager::startSeek(bool, SeekMode) { return 0; }
RequestId HSRadioManager::stopSeek() { return 0; }
RequestId HSRadioManager::setMute(bool) { return 0; }
RequestId HSRadioManager::refreshSignal() { return 0; }

QVector<Station> HSRadioManager::stations(Band) const { return {}; }
Result           HSRadioManager::addStation(const Station &) { return {}; }
Result           HSRadioManager::removeStation(const QString &) { return {}; }
Result           HSRadioManager::renameStation(const QString &, const QString &) { return {}; }
Result           HSRadioManager::clearStations(Band) { return {}; }

QVector<Station> HSRadioManager::favorites(Band) const { return {}; }
Result           HSRadioManager::addFavorite(const Station &) { return {}; }
Result           HSRadioManager::removeFavorite(const QString &) { return {}; }
bool             HSRadioManager::isFavorite(const QString &) const { return false; }

} // namespace HSRadio
