#include "hsradio/storage/HSRadioRepository.h"
#include "hsradio/HSRadioLogging.h"

#include <QFile>
#include <QIODevice>
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

// 原子整文件替换：QSaveFile 写临时文件、flush + fsync、原子 rename 覆盖目标，
// 最后刷目标文件与目录项。失败时临时文件回滚，原文件完好。
// policy 决定 .bak：Create 生成、KeepExisting 保留、Purge 成功后清除。
Result writeJsonFile(const QString &path, const QJsonDocument &doc,
                     JsonBackupPolicy policy = JsonBackupPolicy::Create)
{
    const QByteArray data = doc.toJson(QJsonDocument::Indented);
    const QString bakPath = path + QStringLiteral(".bak");

    if (policy == JsonBackupPolicy::Create && QFile::exists(path)) {
        const QString tmpBak = bakPath + QStringLiteral(".tmp");
        QFile::remove(tmpBak);
        if (QFile::copy(path, tmpBak)) {
            QFile::remove(bakPath);
            QFile::rename(tmpBak, bakPath);
        } else {
            QFile::remove(tmpBak);
            HSRADIO_LOG_WARNING(hsradioStorageRepository).noquote()
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
                               QStringLiteral("配置文件写入不完整: ") + path);
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
    if (policy == JsonBackupPolicy::Purge)
        QFile::remove(bakPath);   // commit 成功后才清除；写入失败仍保留旧备份
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
        HSRADIO_LOG_WARNING(hsradioStorageRepository).noquote()
            << "主文件缺失或损坏，已从 .bak 恢复并重建主文件:" << path;
        // 重建主文件，但跳过备份步骤，避免覆盖刚救回数据的 .bak。
        const Result rebuilt = writeJsonFile(path, *doc, JsonBackupPolicy::KeepExisting);
        if (!rebuilt.ok())
            HSRADIO_LOG_WARNING(hsradioStorageRepository).noquote()
                << "主文件重建失败:" << rebuilt.message;
        if (hasData) *hasData = true;
        return Result::success();
    }

    if (mainExists)
        return Result::failure(ErrorCode::HardwareOperationFailed,
                               QStringLiteral("主文件损坏且 .bak 不可用: ") + path);

    if (hasData) *hasData = false;
    return Result::success();   // 首次启动，默认模板/内置默认
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
    bool hasData = false;
    const Result r = readJsonFile(m_configFilePath, &doc, &hasData);
    if (!r.ok()) {
        HSRADIO_LOG_WARNING(hsradioStorageRepository).noquote() << "config load failed:" << r.message;
        return r;
    }
    if (!hasData) {
        // 首次启动 / 文件被删：从默认模板或内置默认初始化后立即落盘。
        // 防抖下 shutdown() 只在 dirty 时写，若这里不创建，config 永远不会重建。
        if (QFile::exists(m_defaultConfigFilePath)) {
            QJsonDocument d;
            bool defaultHasData = false;
            const Result dr = readJsonFile(m_defaultConfigFilePath, &d, &defaultHasData);
            if (dr.ok() && defaultHasData && d.isObject())
                parseFromJson(d.object());
        }
        return save();
    }
    if (doc.isObject())
        parseFromJson(doc.object());
    HSRADIO_LOG_INFO(hsradioStorageRepository).noquote() << "config loaded; region=" << m_region;
    return Result::success();
}

Result HSRadioRepository::save(JsonBackupPolicy policy)
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

    const Result r = writeJsonFile(m_configFilePath, QJsonDocument(root), policy);
    if (!r.ok())
        HSRADIO_LOG_CRITICAL(hsradioStorageRepository).noquote() << "config save failed:" << r.message;
    return r;
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
    HSRADIO_LOG_INFO(hsradioStorageRepository).noquote() << "reset to default";
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
