#include "hsradio/storage/HSRadioRepository.h"

#include <QFile>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

namespace HSRadio {

namespace {

Result writeJsonFile(const QString &path, const QJsonDocument &doc)
{
    if (QFile::exists(path)) {
        QFile::remove(path + QStringLiteral(".bak"));
        QFile::rename(path, path + QStringLiteral(".bak"));
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return Result::failure(ErrorCode::HardwareOperationFailed,
                               QStringLiteral("无法写入配置文件: ") + path);
    const QByteArray data = doc.toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size()) {
        file.close();
        return Result::failure(ErrorCode::HardwareOperationFailed,
                               QStringLiteral("配置文件写入不完整: ") + path);
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
                               QStringLiteral("无法打开配置文件: ") + path);
    QJsonParseError err;
    *doc = QJsonDocument::fromJson(file.readAll(), &err);
    if (err.error != QJsonParseError::NoError)
        return Result::failure(ErrorCode::HardwareOperationFailed,
                               QStringLiteral("配置文件 JSON 解析失败: ") + err.errorString());
    return Result::success();
}

void parseBand(const QJsonObject &o, HSRadioRepository::BandConfig &cfg)
{
    cfg.minKHz = o.value(QStringLiteral("minKHz")).toInt(cfg.minKHz);
    cfg.maxKHz = o.value(QStringLiteral("maxKHz")).toInt(cfg.maxKHz);
    cfg.stepKHz = o.value(QStringLiteral("stepKHz")).toInt(cfg.stepKHz);
    cfg.defaultKHz = o.value(QStringLiteral("defaultKHz")).toInt(cfg.defaultKHz);
}

QJsonObject serializeBand(const HSRadioRepository::BandConfig &cfg)
{
    QJsonObject o;
    o.insert(QStringLiteral("minKHz"), cfg.minKHz);
    o.insert(QStringLiteral("maxKHz"), cfg.maxKHz);
    o.insert(QStringLiteral("stepKHz"), cfg.stepKHz);
    o.insert(QStringLiteral("defaultKHz"), cfg.defaultKHz);
    return o;
}

} // namespace

HSRadioRepository::HSRadioRepository(const QString &configFilePath,
                                   const QString &defaultConfigFilePath)
    : m_configFilePath(configFilePath)
    , m_defaultConfigFilePath(defaultConfigFilePath.isEmpty()
                                 ? configFilePath + QStringLiteral(".default")
                                 : defaultConfigFilePath)
{
}

Result HSRadioRepository::load()
{
    QJsonDocument doc;
    bool exists = false;
    const Result r = readJsonFile(m_configFilePath, &doc, &exists);
    if (!r.ok())
        return r;
    if (!exists) {
        if (QFile::exists(m_defaultConfigFilePath)) {
            QJsonDocument d;
            bool dex = false;
            const Result dr = readJsonFile(m_defaultConfigFilePath, &d, &dex);
            if (dr.ok() && dex && d.isObject()) {
                parseFromJson(d.object());
                return Result::success();
            }
        }
        return Result::success();   // 保留内置默认
    }
    if (doc.isObject())
        parseFromJson(doc.object());
    return Result::success();
}

Result HSRadioRepository::save()
{
    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("region"), m_region);

    QJsonObject bands;
    bands.insert(QStringLiteral("FM"), serializeBand(m_bandFM));
    bands.insert(QStringLiteral("AM"), serializeBand(m_bandAM));
    root.insert(QStringLiteral("bands"), bands);

    QJsonObject seek;
    seek.insert(QStringLiteral("direction"),
                m_seekUpward ? QStringLiteral("up") : QStringLiteral("down"));
    seek.insert(QStringLiteral("minRssi"), m_threshold.minRssi);
    seek.insert(QStringLiteral("maxUsn"), m_threshold.maxUsn);
    seek.insert(QStringLiteral("maxWam"), m_threshold.maxWam);
    seek.insert(QStringLiteral("maxOffset"), m_threshold.maxOffset);
    seek.insert(QStringLiteral("lockMs"), m_seekLockMs);
    seek.insert(QStringLiteral("sampleMs"), m_seekSampleMs);
    seek.insert(QStringLiteral("sampleCount"), m_seekSampleCount);
    seek.insert(QStringLiteral("maxInvalidRetries"), m_seekMaxInvalid);
    seek.insert(QStringLiteral("wrapAround"), m_seekWrapAround);
    root.insert(QStringLiteral("seek"), seek);

    return writeJsonFile(m_configFilePath, QJsonDocument(root));
}

Result HSRadioRepository::resetToDefault()
{
    if (QFile::exists(m_defaultConfigFilePath)) {
        QJsonDocument d;
        bool dex = false;
        const Result dr = readJsonFile(m_defaultConfigFilePath, &d, &dex);
        if (dr.ok() && dex && d.isObject()) {
            parseFromJson(d.object());
            return Result::success();
        }
    }
    setBuiltInDefaults();
    return Result::success();
}

void HSRadioRepository::setBuiltInDefaults()
{
    m_region = QStringLiteral("CN");
    m_bandFM = BandConfig{87500, 108000, 100, 87500};
    m_bandAM = BandConfig{531, 1602, 9, 531};
    m_threshold = SeekThreshold{};   // 21/25/25/40
    m_seekUpward = true;
    m_seekWrapAround = true;
    m_seekLockMs = 8;
    m_seekSampleMs = 2;
    m_seekSampleCount = 4;
    m_seekMaxInvalid = 3;
}

void HSRadioRepository::parseFromJson(const QJsonObject &root)
{
    m_region = root.value(QStringLiteral("region")).toString(m_region);

    const QJsonObject bands = root.value(QStringLiteral("bands")).toObject();
    parseBand(bands.value(QStringLiteral("FM")).toObject(), m_bandFM);
    parseBand(bands.value(QStringLiteral("AM")).toObject(), m_bandAM);

    const QJsonObject seek = root.value(QStringLiteral("seek")).toObject();
    const QString dir = seek.value(QStringLiteral("direction")).toString();
    m_seekUpward = dir.compare(QStringLiteral("down"), Qt::CaseInsensitive) != 0;

    m_threshold.minRssi = seek.value(QStringLiteral("minRssi")).toInt(m_threshold.minRssi);
    m_threshold.maxUsn = seek.value(QStringLiteral("maxUsn")).toInt(m_threshold.maxUsn);
    m_threshold.maxWam = seek.value(QStringLiteral("maxWam")).toInt(m_threshold.maxWam);
    m_threshold.maxOffset = seek.value(QStringLiteral("maxOffset")).toInt(m_threshold.maxOffset);

    m_seekWrapAround = seek.value(QStringLiteral("wrapAround")).toBool(m_seekWrapAround);
    m_seekLockMs = seek.value(QStringLiteral("lockMs")).toInt(m_seekLockMs);
    m_seekSampleMs = seek.value(QStringLiteral("sampleMs")).toInt(m_seekSampleMs);
    m_seekSampleCount = seek.value(QStringLiteral("sampleCount")).toInt(m_seekSampleCount);
    m_seekMaxInvalid = seek.value(QStringLiteral("maxInvalidRetries")).toInt(m_seekMaxInvalid);
}

QString HSRadioRepository::region() const { return m_region; }
void HSRadioRepository::setRegion(const QString &region) { m_region = region; }

HSRadioRepository::BandConfig HSRadioRepository::bandConfig(Band band) const
{
    return band == Band::AM ? m_bandAM : m_bandFM;
}

void HSRadioRepository::setBandConfig(Band band, const BandConfig &cfg)
{
    if (band == Band::AM)
        m_bandAM = cfg;
    else
        m_bandFM = cfg;
}

SeekThreshold HSRadioRepository::seekThreshold() const { return m_threshold; }
void HSRadioRepository::setSeekThreshold(const SeekThreshold &th) { m_threshold = th; }

bool HSRadioRepository::seekUpward() const { return m_seekUpward; }
void HSRadioRepository::setSeekUpward(bool upward) { m_seekUpward = upward; }

bool HSRadioRepository::seekWrapAround() const { return m_seekWrapAround; }
void HSRadioRepository::setSeekWrapAround(bool wrap) { m_seekWrapAround = wrap; }

int HSRadioRepository::seekLockMs() const { return m_seekLockMs; }
void HSRadioRepository::setSeekLockMs(int ms) { m_seekLockMs = ms; }

int HSRadioRepository::seekSampleMs() const { return m_seekSampleMs; }
void HSRadioRepository::setSeekSampleMs(int ms) { m_seekSampleMs = ms; }

int HSRadioRepository::seekSampleCount() const { return m_seekSampleCount; }
void HSRadioRepository::setSeekSampleCount(int n) { m_seekSampleCount = n; }

int HSRadioRepository::seekMaxInvalid() const { return m_seekMaxInvalid; }
void HSRadioRepository::setSeekMaxInvalid(int n) { m_seekMaxInvalid = n; }

SeekRequest HSRadioRepository::makeSeekRequest(Band band, bool upward, SeekMode mode,
                                              int startKHz) const
{
    SeekRequest req;
    req.band = band;
    req.upward = upward;
    req.mode = mode;
    req.startKHz = startKHz;
    const BandConfig bc = bandConfig(band);
    req.minKHz = bc.minKHz;
    req.maxKHz = bc.maxKHz;
    req.stepKHz = bc.stepKHz;
    req.threshold = m_threshold;
    req.lockMs = m_seekLockMs;
    req.sampleMs = m_seekSampleMs;
    req.sampleCount = m_seekSampleCount;
    req.maxInvalid = m_seekMaxInvalid;
    req.wrapAround = m_seekWrapAround;
    req.muteDuringSeek = true;
    return req;
}

} // namespace HSRadio
