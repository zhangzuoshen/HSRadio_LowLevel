#include "hsradio/storage/HSRadioStationStore.h"
#include "hsradio/HSRadioLogging.h"

#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QFileInfo>
#include <QDir>

#include <fcntl.h>
#include <unistd.h>

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

// 同步目录项：保证 rename 之后目录元数据也落盘。
void syncParentDirectory(const QString &path)
{
    const QDir dir = QFileInfo(path).absoluteDir();
    const QByteArray dirPath = QFile::encodeName(dir.absolutePath());
    const int fd = ::open(dirPath.constData(), O_RDONLY | O_DIRECTORY);
    if (fd < 0)
        return;
    ::fsync(fd);
    ::close(fd);
}

// 原子整文件替换：先在 commit 之前把上一版复制成 .bak（主文件全程不挪走），
// 再用 QSaveFile 写临时文件、flush + fsync、原子 rename 覆盖目标，
// 最后刷目标文件与目录项。失败时临时文件回滚，原文件完好。
Result writeJsonFile(const QString &path, const QJsonDocument &doc,
                     bool backupExisting = true)
{
    const QByteArray data = doc.toJson(QJsonDocument::Indented);
    const QString bakPath = path + QStringLiteral(".bak");

    if (backupExisting && QFile::exists(path)) {
        const QString tmpBak = bakPath + QStringLiteral(".tmp");
        QFile::remove(tmpBak);
        if (QFile::copy(path, tmpBak)) {
            QFile::remove(bakPath);
            QFile::rename(tmpBak, bakPath);
        } else {
            QFile::remove(tmpBak);
            HSRADIO_LOG_WARNING(hsradioStorageStore).noquote()
                << "backup copy failed (non-fatal):" << bakPath;
        }
    }

    QSaveFile saveFile(path);
    saveFile.setDirectWriteFallback(false);   // 禁止退化成直接写目标文件
    if (!saveFile.open(QIODevice::WriteOnly))
        return Result::failure(ErrorCode::HardwareOperationFailed,
                               QStringLiteral("无法打开临时文件: ") + path);
    if (saveFile.write(data) != data.size() || !saveFile.flush()) {
        saveFile.cancelWriting();
        return Result::failure(ErrorCode::HardwareOperationFailed,
                               QStringLiteral("状态文件写入不完整: ") + path);
    }
    if (::fsync(saveFile.handle()) != 0) {
        saveFile.cancelWriting();
        return Result::failure(ErrorCode::HardwareOperationFailed,
                               QStringLiteral("临时文件 fsync 失败: ") + path);
    }
    if (!saveFile.commit())
        return Result::failure(ErrorCode::HardwareOperationFailed,
                               QStringLiteral("QSaveFile commit 失败: ") + saveFile.errorString());

    QFile target(path);
    if (target.open(QIODevice::ReadOnly)) {
        ::fsync(target.handle());
        target.close();
    }
    syncParentDirectory(path);
    return Result::success();
}

bool loadJsonObject(const QString &path, QJsonDocument *doc)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    QJsonParseError err;
    const QJsonDocument parsed = QJsonDocument::fromJson(file.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !parsed.isObject())
        return false;
    *doc = parsed;
    return true;
}

// 主文件缺失或解析失败时自动从 .bak 恢复；恢复成功必须让 load() 拿到数据。
Result readJsonFile(const QString &path, QJsonDocument *doc, bool *hasData)
{
    const bool mainExists = QFile::exists(path);
    if (mainExists && loadJsonObject(path, doc)) {
        if (hasData) *hasData = true;
        return Result::success();
    }

    const QString bakPath = path + QStringLiteral(".bak");
    if (QFile::exists(bakPath) && loadJsonObject(bakPath, doc)) {
        HSRADIO_LOG_WARNING(hsradioStorageStore).noquote()
            << "主文件缺失或损坏，已从 .bak 恢复并重建主文件:" << path;
        // 重建主文件，但跳过备份步骤，避免覆盖刚救回数据的 .bak。
        const Result rebuilt = writeJsonFile(path, *doc, false);
        if (!rebuilt.ok())
            HSRADIO_LOG_WARNING(hsradioStorageStore).noquote()
                << "主文件重建失败:" << rebuilt.message;
        if (hasData) *hasData = true;
        return Result::success();
    }

    if (mainExists)
        return Result::failure(ErrorCode::HardwareOperationFailed,
                               QStringLiteral("主文件损坏且 .bak 不可用: ") + path);

    if (hasData) *hasData = false;
    return Result::success();   // 首次启动，用内置默认
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
    bool hasData = false;
    const Result r = readJsonFile(m_stateFilePath, &doc, &hasData);
    if (!r.ok()) {
        HSRADIO_LOG_WARNING(hsradioStorageStore).noquote() << "state load failed:" << r.message;
        return r;
    }
    if (!hasData)
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
