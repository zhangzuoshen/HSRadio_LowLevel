#include "hsradio/storage/HSRadioStationStore.h"
#include "hsradio/HSRadioLogging.h"

#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

namespace HSRadio {

namespace {

QString bandKey(Band band)
{
    return band == Band::AM ? QStringLiteral("AM") : QStringLiteral("FM");
}

Band bandFromKey(const QString &key)
{
    return key.compare(QStringLiteral("AM"), Qt::CaseInsensitive) == 0
               ? Band::AM : Band::FM;
}

// 安全写：先把旧文件备份为 <path>.bak，再写新内容。
Result writeJsonFile(const QString &path, const QJsonDocument &doc)
{
    if (QFile::exists(path)) {
        QFile::remove(path + QStringLiteral(".bak"));
        QFile::rename(path, path + QStringLiteral(".bak"));
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return Result::failure(ErrorCode::HardwareOperationFailed,
                               QStringLiteral("无法写入状态文件: ") + path);
    const QByteArray data = doc.toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size()) {
        file.close();
        return Result::failure(ErrorCode::HardwareOperationFailed,
                               QStringLiteral("状态文件写入不完整: ") + path);
    }
    file.close();
    return Result::success();
}

Result readJsonFile(const QString &path, QJsonDocument *doc, bool *exists)
{
    if (exists)
        *exists = QFile::exists(path);
    if (!QFile::exists(path))
        return Result::success();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return Result::failure(ErrorCode::HardwareOperationFailed,
                               QStringLiteral("无法打开状态文件: ") + path);
    QJsonParseError err;
    *doc = QJsonDocument::fromJson(file.readAll(), &err);
    if (err.error != QJsonParseError::NoError)
        return Result::failure(ErrorCode::HardwareOperationFailed,
                               QStringLiteral("状态文件 JSON 解析失败: ") + err.errorString());
    return Result::success();
}

Station parseStation(const QJsonObject &o, Band fallbackBand)
{
    Station s;
    s.id = o.value(QStringLiteral("id")).toString();
    s.band = bandFromKey(o.value(QStringLiteral("band")).toString());
    if (o.contains(QStringLiteral("band")) && o.value(QStringLiteral("band")).toString().isEmpty())
        s.band = fallbackBand;
    s.frequencyKHz = o.value(QStringLiteral("frequencyKHz")).toInt(0);
    s.name = o.value(QStringLiteral("name")).toString();
    s.source = o.value(QStringLiteral("source")).toString();
    if (s.id.isEmpty())
        s.id = HSRadioStationStore::makeStationId(s.band, s.frequencyKHz);
    return s;
}

QJsonObject serializeStation(const Station &s)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), s.id);
    o.insert(QStringLiteral("band"), bandKey(s.band));
    o.insert(QStringLiteral("frequencyKHz"), s.frequencyKHz);
    o.insert(QStringLiteral("name"), s.name);
    o.insert(QStringLiteral("source"), s.source);
    return o;
}

} // namespace

HSRadioStationStore::HSRadioStationStore(const QString &stateFilePath)
    : m_stateFilePath(stateFilePath)
{
}

Result HSRadioStationStore::load()
{
    QJsonDocument doc;
    bool exists = false;
    const Result r = readJsonFile(m_stateFilePath, &doc, &exists);
    if (!r.ok()) {
        HSRADIO_LOG_WARNING(hsradioStorageStore).noquote() << "state load failed:" << r.message;
        return r;
    }
    if (!exists)
        return Result::success();   // 用内置默认

    const QJsonObject root = doc.object();

    m_currentBand = bandFromKey(root.value(QStringLiteral("currentBand")).toString());

    const QJsonObject lf = root.value(QStringLiteral("lastFrequency")).toObject();
    m_lastFrequencyFM = lf.value(QStringLiteral("FM")).toInt(87500);
    m_lastFrequencyAM = lf.value(QStringLiteral("AM")).toInt(531);

    const QJsonObject st = root.value(QStringLiteral("stations")).toObject();
    m_stationsFM.clear();
    for (const QJsonValue &v : st.value(QStringLiteral("FM")).toArray())
        m_stationsFM.append(parseStation(v.toObject(), Band::FM));
    m_stationsAM.clear();
    for (const QJsonValue &v : st.value(QStringLiteral("AM")).toArray())
        m_stationsAM.append(parseStation(v.toObject(), Band::AM));

    const QJsonObject fa = root.value(QStringLiteral("favorites")).toObject();
    m_favoritesFM.clear();
    for (const QJsonValue &v : fa.value(QStringLiteral("FM")).toArray())
        m_favoritesFM.append(parseStation(v.toObject(), Band::FM));
    m_favoritesAM.clear();
    for (const QJsonValue &v : fa.value(QStringLiteral("AM")).toArray())
        m_favoritesAM.append(parseStation(v.toObject(), Band::AM));

    HSRADIO_LOG_INFO(hsradioStorageStore).noquote() << "state loaded; band=" << bandKey(m_currentBand) << "FM stations=" << m_stationsFM.size() << "AM stations=" << m_stationsAM.size();
    return Result::success();
}

Result HSRadioStationStore::save()
{
    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("currentBand"), bandKey(m_currentBand));

    QJsonObject lf;
    lf.insert(QStringLiteral("FM"), m_lastFrequencyFM);
    lf.insert(QStringLiteral("AM"), m_lastFrequencyAM);
    root.insert(QStringLiteral("lastFrequency"), lf);

    QJsonObject st;
    QJsonArray fmArr, amArr;
    for (const Station &s : m_stationsFM) fmArr.append(serializeStation(s));
    for (const Station &s : m_stationsAM) amArr.append(serializeStation(s));
    st.insert(QStringLiteral("FM"), fmArr);
    st.insert(QStringLiteral("AM"), amArr);
    root.insert(QStringLiteral("stations"), st);

    QJsonObject fa;
    QJsonArray ffmArr, famArr;
    for (const Station &s : m_favoritesFM) ffmArr.append(serializeStation(s));
    for (const Station &s : m_favoritesAM) famArr.append(serializeStation(s));
    fa.insert(QStringLiteral("FM"), ffmArr);
    fa.insert(QStringLiteral("AM"), famArr);
    root.insert(QStringLiteral("favorites"), fa);

    const Result r = writeJsonFile(m_stateFilePath, QJsonDocument(root));
    if (!r.ok())
        HSRADIO_LOG_CRITICAL(hsradioStorageStore).noquote() << "state save failed:" << r.message;
    return r;
}

Result HSRadioStationStore::resetToDefault()
{
    m_currentBand = Band::FM;
    m_lastFrequencyFM = 87500;
    m_lastFrequencyAM = 531;
    m_stationsFM.clear();
    m_stationsAM.clear();
    m_favoritesFM.clear();
    m_favoritesAM.clear();
    HSRADIO_LOG_INFO(hsradioStorageStore).noquote() << "reset to default";
    return Result::success();
}

Band HSRadioStationStore::currentBand() const { return m_currentBand; }

void HSRadioStationStore::setCurrentBand(Band band) { m_currentBand = band; }

int HSRadioStationStore::lastFrequency(Band band) const
{
    return band == Band::AM ? m_lastFrequencyAM : m_lastFrequencyFM;
}

void HSRadioStationStore::setLastFrequency(Band band, int frequencyKHz)
{
    if (band == Band::AM)
        m_lastFrequencyAM = frequencyKHz;
    else
        m_lastFrequencyFM = frequencyKHz;
}

const QVector<Station> *HSRadioStationStore::list(Band band, bool favorites) const
{
    if (favorites)
        return band == Band::AM ? &m_favoritesAM : &m_favoritesFM;
    return band == Band::AM ? &m_stationsAM : &m_stationsFM;
}

QVector<Station> *HSRadioStationStore::mutableList(Band band, bool favorites)
{
    if (favorites)
        return band == Band::AM ? &m_favoritesAM : &m_favoritesFM;
    return band == Band::AM ? &m_stationsAM : &m_stationsFM;
}

int HSRadioStationStore::indexOfId(const QVector<Station> &list, const QString &id)
{
    for (int i = 0; i < list.size(); ++i)
        if (list.at(i).id == id)
            return i;
    return -1;
}

QVector<Station> HSRadioStationStore::stations(Band band) const
{
    return *list(band, false);
}

Result HSRadioStationStore::addStation(const Station &station)
{
    Station s = station;
    if (s.id.isEmpty())
        s.id = makeStationId(s.band, s.frequencyKHz);
    if (s.id.isEmpty())
        return Result::failure(ErrorCode::InvalidParameter,
                               QStringLiteral("电台缺少有效频率，无法生成 id"));
    QVector<Station> *l = mutableList(s.band, false);
    const int idx = indexOfId(*l, s.id);
    if (idx >= 0)
        (*l)[idx] = s;   // upsert（覆盖同 id）
    else
        l->append(s);
    HSRADIO_LOG_DEBUG(hsradioStorageStore).noquote() << "addStation" << s.id;
    return Result::success();
}

Result HSRadioStationStore::removeStation(const QString &id)
{
    for (bool fav : {false, true}) {
        for (Band band : {Band::FM, Band::AM}) {
            QVector<Station> *l = mutableList(band, fav);
            const int idx = indexOfId(*l, id);
            if (idx >= 0) {
                l->removeAt(idx);
                HSRADIO_LOG_DEBUG(hsradioStorageStore).noquote() << "removeStation" << id;
                return Result::success();
            }
        }
    }
    return Result::failure(ErrorCode::InvalidParameter,
                           QStringLiteral("未找到要删除的电台: ") + id);
}

Result HSRadioStationStore::renameStation(const QString &id, const QString &name)
{
    for (bool fav : {false, true}) {
        for (Band band : {Band::FM, Band::AM}) {
            QVector<Station> *l = mutableList(band, fav);
            const int idx = indexOfId(*l, id);
            if (idx >= 0) {
                (*l)[idx].name = name;
                return Result::success();
            }
        }
    }
    return Result::failure(ErrorCode::InvalidParameter,
                           QStringLiteral("未找到要重命名的电台: ") + id);
}

Result HSRadioStationStore::clearStations(Band band)
{
    mutableList(band, false)->clear();
    return Result::success();
}

QVector<Station> HSRadioStationStore::favorites(Band band) const
{
    return *list(band, true);
}

Result HSRadioStationStore::addFavorite(const Station &station)
{
    Station s = station;
    if (s.id.isEmpty())
        s.id = makeStationId(s.band, s.frequencyKHz);
    if (s.id.isEmpty())
        return Result::failure(ErrorCode::InvalidParameter,
                               QStringLiteral("收藏缺少有效频率，无法生成 id"));
    QVector<Station> *l = mutableList(s.band, true);
    const int idx = indexOfId(*l, s.id);
    if (idx >= 0)
        (*l)[idx] = s;
    else
        l->append(s);
    return Result::success();
}

Result HSRadioStationStore::removeFavorite(const QString &id)
{
    for (Band band : {Band::FM, Band::AM}) {
        QVector<Station> *l = mutableList(band, true);
        const int idx = indexOfId(*l, id);
        if (idx >= 0) {
            l->removeAt(idx);
            HSRADIO_LOG_DEBUG(hsradioStorageStore).noquote() << "removeFavorite" << id;
            return Result::success();
        }
    }
    return Result::failure(ErrorCode::InvalidParameter,
                           QStringLiteral("未找到要删除的收藏: ") + id);
}

bool HSRadioStationStore::isFavorite(const QString &id) const
{
    for (Band band : {Band::FM, Band::AM})
        if (indexOfId(*list(band, true), id) >= 0)
            return true;
    return false;
}

QString HSRadioStationStore::makeStationId(Band band, int frequencyKHz)
{
    if (frequencyKHz <= 0)
        return QString();
    return bandKey(band) + QStringLiteral("-") + QString::number(frequencyKHz);
}

} // namespace HSRadio
