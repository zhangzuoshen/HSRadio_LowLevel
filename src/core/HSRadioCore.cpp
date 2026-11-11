// HSRadioCore：把 HSRadioManager 的业务接口映射成 Group::Radio 消息。
//
// 生命周期：平台加载插件后调用 onLoaded()，先初始化调度器（异步等
// initialized 信号），再创建存储层与 Manager 并接线；所有 Manager 信号
// 转成 send()，所有客户端请求在 dispatch() 里同步/异步分流。
// 异步请求返回调度器 RequestId，用 pending 表映射回客户端 reqId；
// 同步接口直接回 updateResult。

#include "hsradio/core/HSRadioCore.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QString>

#include <memory>

#include "hsradio/HSRadioLogging.h"
#include "hsradio/HSRadioTypes.h"
#include "hsradio/hardware/IRadioHardware.h"
#include "hsradio/hardware/MockRadioHardware.h"
#if defined(HSRADIO_ENABLE_ALSA)
#include "hsradio/hardware/HSRadioSAF775XBackend.h"
#endif
#include "hsradio/manager/HSRadioManager.h"
#include "hsradio/scheduler/HSRadioOperationScheduler.h"
#include "hsradio/storage/HSRadioRepository.h"
#include "hsradio/storage/HSRadioStationStore.h"

// 专用日志类别只在本文件声明与定义，不暴露到公共头；默认 Info。
Q_LOGGING_CATEGORY(hsradioCore, "hsradio.core", QtInfoMsg)

using namespace HSRadio;
using namespace HSRadioMsg;

namespace {

QString errorCodeName(ErrorCode code)
{
    switch (code) {
    case ErrorCode::None: return QStringLiteral("None");
    case ErrorCode::InvalidParameter: return QStringLiteral("InvalidParameter");
    case ErrorCode::HardwareNotReady: return QStringLiteral("HardwareNotReady");
    case ErrorCode::AlsaOpenFailed: return QStringLiteral("AlsaOpenFailed");
    case ErrorCode::AlsaControlNotFound: return QStringLiteral("AlsaControlNotFound");
    case ErrorCode::AlsaControlTypeUnsupported: return QStringLiteral("AlsaControlTypeUnsupported");
    case ErrorCode::AlsaReadFailed: return QStringLiteral("AlsaReadFailed");
    case ErrorCode::AlsaWriteFailed: return QStringLiteral("AlsaWriteFailed");
    case ErrorCode::HardwareOperationFailed: return QStringLiteral("HardwareOperationFailed");
    }
    return QStringLiteral("UnknownError");
}

QString bandName(Band band)
{
    return band == Band::FM ? QStringLiteral("FM") : QStringLiteral("AM");
}

// 允许 "FM"/"AM" 字符串，也允许 0/1 数值；其余一律视为非法。
bool parseBand(const QJsonValue &v, Band *band)
{
    if (v.isString()) {
        const QString t = v.toString().trimmed().toUpper();
        if (t == QStringLiteral("FM")) { *band = Band::FM; return true; }
        if (t == QStringLiteral("AM")) { *band = Band::AM; return true; }
        return false;
    }
    if (v.isDouble()) {
        const int n = v.toInt();
        if (n == static_cast<int>(Band::FM)) { *band = Band::FM; return true; }
        if (n == static_cast<int>(Band::AM)) { *band = Band::AM; return true; }
    }
    return false;
}

QJsonObject objectOf(const QJsonValue &v)
{
    return v.isObject() ? v.toObject() : QJsonObject();
}

quint64 reqIdOf(const QJsonValue &v)
{
    if (!v.isObject())
        return 0;
    return static_cast<quint64>(v.toObject().value(QStringLiteral("reqId")).toDouble());
}

// QJsonValue 的数值构造统一走 double，兼容 Qt 5.12 的 qint64 重载差异。
QJsonValue jsonId(quint64 id)
{
    return QJsonValue(static_cast<double>(id));
}

QJsonObject stationToJson(const Station &s)
{
    return QJsonObject{
        {"id", s.id},
        {"band", bandName(s.band)},
        {"freqKHz", s.frequencyKHz},
        {"name", s.name},
        {"source", s.source},
    };
}

QJsonArray stationsToJson(const QVector<Station> &list)
{
    QJsonArray arr;
    for (const Station &s : list)
        arr.append(stationToJson(s));
    return arr;
}

QJsonObject signalToJson(const SignalInfo &info)
{
    QJsonObject o;
    if (info.hasRssi) o.insert(QStringLiteral("rssi"), info.rssi);
    if (info.hasUsn) o.insert(QStringLiteral("usn"), info.usn);
    if (info.hasWam) o.insert(QStringLiteral("wam"), info.wam);
    if (info.hasOffset) o.insert(QStringLiteral("offset"), info.offset);
    if (info.hasModulation) o.insert(QStringLiteral("modulation"), info.modulation);
    if (info.hasStereo) o.insert(QStringLiteral("stereo"), info.stereo);
    return o;
}

QJsonObject bandConfigToJson(const HSRadioRepository::BandConfig &cfg)
{
    return QJsonObject{
        {"minKHz", cfg.minKHz},
        {"maxKHz", cfg.maxKHz},
        {"stepKHz", cfg.stepKHz},
        {"defaultKHz", cfg.defaultKHz},
    };
}

QJsonObject thresholdToJson(const SeekThreshold &th)
{
    return QJsonObject{
        {"minRssi", th.minRssi},
        {"maxUsn", th.maxUsn},
        {"maxWam", th.maxWam},
        {"maxOffset", th.maxOffset},
        {"maxModulation", th.maxModulation},
        {"lockMs", th.lockMs},
    };
}

} // namespace

class HSRadioCore_
{
public:
    QString stateFile = QStringLiteral("/data/hsradio/radio_state.json");
    QString configFile = QStringLiteral("/data/hsradio/radio_config.json");

    std::unique_ptr<HSRadioOperationScheduler> scheduler;
    std::unique_ptr<HSRadioStationStore> store;
    std::unique_ptr<HSRadioRepository> repository;
    std::unique_ptr<HSRadioManager> manager;

    // 调度器 RequestId -> 客户端 reqId；异步完成时按此回执。
    QHash<quint64, quint64> pending;
    HardwareState lastState;
};

HSRadioCore::HSRadioCore(QObject *parent)
    : HSMWatcher(parent)
    , p_(new HSRadioCore_)
{
}

HSRadioCore::~HSRadioCore(void)
{
    // 先让 Manager 防抖落盘，再停工作线程；顺序与调试工具一致。
    if (p_->manager)
        p_->manager->shutdown();
    if (p_->scheduler)
        p_->scheduler->shutdown();
    delete p_;
}

QVector<int> HSRadioCore::listenedGroups(void)
{
    return {groupID()};
}

int HSRadioCore::groupID(void)
{
    return Group::Radio;
}

void HSRadioCore::onLoaded(void)
{
    HSRADIO_LOG_INFO(hsradioCore).noquote() << "onLoaded";
    registerHSRadioMetaTypes();

    std::unique_ptr<IRadioHardware> hardware;
#if defined(HSRADIO_ENABLE_ALSA)
    HSRadioSAF775XConfig safConfig;   // 控件名与极性全部走驱动默认值
    hardware.reset(new HSRadioSAF775XBackend(safConfig));
#else
    hardware.reset(new MockRadioHardware);
#endif

    p_->scheduler = std::make_unique<HSRadioOperationScheduler>();
    connect(p_->scheduler.get(), &HSRadioOperationScheduler::initialized, this,
            [this](ErrorCode code) { onSchedulerReady(code); });

    const Result r = p_->scheduler->initialize(std::move(hardware));
    if (!r.ok()) {
        sendError(0, requireReady, r.code, r.message);
        send(updateReady, QJsonObject{{"module", QStringLiteral("HSRadioCore")}, {"ok", false}});
        return;
    }
    HSRADIO_LOG_INFO(hsradioCore).noquote() << "scheduler initializing, wait for initialized";
}

void HSRadioCore::onSchedulerReady(ErrorCode code)
{
    if (code != ErrorCode::None) {
        HSRADIO_LOG_CRITICAL(hsradioCore).noquote() << "scheduler init failed:" << errorCodeName(code);
        sendError(0, requireReady, code, QStringLiteral("scheduler initialize failed"));
        send(updateReady, QJsonObject{{"module", QStringLiteral("HSRadioCore")}, {"ok", false}});
        return;
    }

    p_->store = std::make_unique<HSRadioStationStore>(p_->stateFile);
    p_->repository = std::make_unique<HSRadioRepository>(p_->configFile);
    p_->manager = std::make_unique<HSRadioManager>(p_->scheduler.get(), p_->store.get(),
                                                  p_->repository.get());
    connectManagerSignals();

    const Result r = p_->manager->initialize();
    if (!r.ok()) {
        sendError(0, requireReady, r.code, r.message);
        send(updateReady, QJsonObject{{"module", QStringLiteral("HSRadioCore")}, {"ok", false}});
        return;
    }

    send(updateReady, QJsonObject{{"module", QStringLiteral("HSRadioCore")}, {"ok", true}});
    publishState();
    publishStationList(Band::FM);
    publishStationList(Band::AM);
    publishFavorites(Band::FM);
    publishFavorites(Band::AM);
    HSRADIO_LOG_INFO(hsradioCore).noquote() << "core ready";
}

void HSRadioCore::connectManagerSignals(void)
{
    HSRadioManager *m = p_->manager.get();

    connect(m, &HSRadioManager::initialized, this, [this](ErrorCode code) {
        HSRADIO_LOG_INFO(hsradioCore).noquote() << "manager initialized:" << errorCodeName(code);
        publishState();
    });
    connect(m, &HSRadioManager::stateChanged, this, [this](const HardwareState &s) {
        p_->lastState = s;
        publishState();
    });
    connect(m, &HSRadioManager::bandChanged, this, [this](Band b) {
        send(updateBand, QJsonObject{{"band", bandName(b)}});
        publishStationList(b);
    });
    connect(m, &HSRadioManager::frequencyChanged, this, [this](int f) {
        send(updateFreq, QJsonObject{{"freqKHz", f}});
    });
    connect(m, &HSRadioManager::signalInfoChanged, this, [this](const SignalInfo &info) {
        send(updateSignal, signalToJson(info));
    });
    connect(m, &HSRadioManager::stationsChanged, this, [this](Band b) {
        publishStationList(b);
    });
    connect(m, &HSRadioManager::favoritesChanged, this, [this]() {
        publishFavorites(p_->manager->currentBand());
    });
    connect(m, &HSRadioManager::seekStarted, this, [this](quint64 id, bool upward) {
        send(updateSeekStarted, QJsonObject{
            {"reqId", jsonId(p_->pending.value(id, 0))},
            {"requestId", jsonId(id)},
            {"upward", upward},
        });
    });
    connect(m, &HSRadioManager::seekProgress, this, [this](quint64 id, int frequencyKHz) {
        send(updateSeekProgress, QJsonObject{
            {"requestId", jsonId(id)},
            {"freqKHz", frequencyKHz},
        });
    });
    connect(m, &HSRadioManager::seekFinished, this, [this](quint64 id, int f, bool found) {
        send(updateSeekFinished, QJsonObject{
            {"reqId", jsonId(p_->pending.value(id, 0))},
            {"requestId", jsonId(id)},
            {"freqKHz", f},
            {"found", found},
        });
    });
    connect(m, &HSRadioManager::restoreFinished, this,
            [this](quint64 id, ErrorCode code, Band band, int frequencyKHz) {
        send(updateRestoreFinished, QJsonObject{
            {"reqId", jsonId(p_->pending.value(id, 0))},
            {"requestId", jsonId(id)},
            {"code", errorCodeName(code)},
            {"band", bandName(band)},
            {"freqKHz", frequencyKHz},
        });
    });
    connect(m, &HSRadioManager::operationFinished, this, [this](quint64 id, ErrorCode code) {
        const quint64 reqId = p_->pending.take(id);
        send(updateOperationFinished, QJsonObject{
            {"reqId", jsonId(reqId)},
            {"requestId", jsonId(id)},
            {"code", errorCodeName(code)},
        });
    });
    connect(m, &HSRadioManager::errorOccurred, this,
            [this](ErrorCode code, const QString &text) {
        sendError(0, 0, code, text);
    });
}

void HSRadioCore::onMessage(int id, int msg, const QJsonValue &value)
{
    // 框架只会把 listenedGroups() 里的消息送到这里；当前只处理本组。
    if (id != groupID()) {
        HSRADIO_LOG_DEBUG(hsradioCore).noquote() << "ignore message from group" << id;
        return;
    }
    dispatch(msg, value);
}

void HSRadioCore::onMessage(int msg, const QJsonValue &value)
{
    dispatch(msg, value);
}

void HSRadioCore::dispatch(int msg, const QJsonValue &value)
{
    const quint64 reqId = reqIdOf(value);
    const QJsonObject args = objectOf(value);

    // 就绪查询在 Manager 创建之前也要能回答。
    if (msg == requireReady) {
        const bool ok = p_->manager && p_->manager->isInitialized();
        send(updateReady, QJsonObject{{"module", QStringLiteral("HSRadioCore")}, {"ok", ok}});
        return;
    }

    if (!p_->manager) {
        sendError(reqId, msg, ErrorCode::HardwareNotReady, QStringLiteral("core not initialized"));
        return;
    }

    switch (msg) {
    case requireRestore:
        accept(reqId, msg, p_->manager->restoreLastState());
        break;
    case requireFactoryReset: {
        const Result r = p_->manager->factoryReset();
        sendResult(reqId, msg, r);
        if (r.ok()) {
            publishState();
            publishStationList(Band::FM);
            publishStationList(Band::AM);
            publishFavorites(Band::FM);
            publishFavorites(Band::AM);
            publishConfig();
        }
        break;
    }
    case requireState:
        publishState();
        sendResult(reqId, msg, Result::success());
        break;
    case requireConfig:
        publishConfig();
        sendResult(reqId, msg, Result::success());
        break;

    case requireSetBand: {
        Band band = p_->manager->currentBand();
        if (args.contains(QStringLiteral("band"))
            && !parseBand(args.value(QStringLiteral("band")), &band)) {
            sendError(reqId, msg, ErrorCode::InvalidParameter, QStringLiteral("invalid band"));
            break;
        }
        accept(reqId, msg, p_->manager->setBand(band));
        break;
    }
    case requireSetFreq: {
        const int f = args.value(QStringLiteral("freqKHz")).toInt(0);
        if (f <= 0) {
            sendError(reqId, msg, ErrorCode::InvalidParameter, QStringLiteral("invalid freqKHz"));
            break;
        }
        accept(reqId, msg, p_->manager->setFrequency(f));
        break;
    }
    case requireTuneUp:
        accept(reqId, msg, p_->manager->stepUp());
        break;
    case requireTuneDown:
        accept(reqId, msg, p_->manager->stepDown());
        break;
    case requireSetMute:
        accept(reqId, msg, p_->manager->setMute(
                   args.value(QStringLiteral("mute")).toBool(false)));
        break;
    case requireRefreshSignal:
        accept(reqId, msg, p_->manager->refreshSignal());
        break;

    case requireSeek: {
        const bool upward = args.value(QStringLiteral("up")).toBool(true);
        const QString modeText = args.value(QStringLiteral("mode"))
                                     .toString(QStringLiteral("full")).trimmed().toLower();
        const SeekMode mode = modeText == QStringLiteral("next")
                                 ? SeekMode::NextStation : SeekMode::FullBand;
        accept(reqId, msg, p_->manager->startSeek(upward, mode));
        break;
    }
    case requireStopSeek:
        accept(reqId, msg, p_->manager->stopSeek());
        break;

    case requireStationList: {
        Band band = p_->manager->currentBand();
        if (args.contains(QStringLiteral("band"))
            && !parseBand(args.value(QStringLiteral("band")), &band)) {
            sendError(reqId, msg, ErrorCode::InvalidParameter, QStringLiteral("invalid band"));
            break;
        }
        publishStationList(band);
        sendResult(reqId, msg, Result::success());
        break;
    }
    case requireAddStation: {
        Band band = p_->manager->currentBand();
        const int f = args.value(QStringLiteral("freqKHz")).toInt(0);
        if ((args.contains(QStringLiteral("band"))
             && !parseBand(args.value(QStringLiteral("band")), &band)) || f <= 0) {
            sendError(reqId, msg, ErrorCode::InvalidParameter, QStringLiteral("invalid band or freqKHz"));
            break;
        }
        Station s;
        s.band = band;
        s.frequencyKHz = f;
        s.name = args.value(QStringLiteral("name")).toString();
        s.source = QStringLiteral("manual");
        const Result r = p_->manager->addStation(s);
        sendResult(reqId, msg, r);
        if (r.ok()) publishStationList(band);
        break;
    }
    case requireRemoveStation: {
        const QString id = args.value(QStringLiteral("id")).toString();
        const Result r = p_->manager->removeStation(id);
        sendResult(reqId, msg, r);
        if (r.ok()) publishStationList(p_->manager->currentBand());
        break;
    }
    case requireRenameStation: {
        const QString id = args.value(QStringLiteral("id")).toString();
        const QString name = args.value(QStringLiteral("name")).toString();
        const Result r = p_->manager->renameStation(id, name);
        sendResult(reqId, msg, r);
        if (r.ok()) publishStationList(p_->manager->currentBand());
        break;
    }
    case requireClearStations: {
        Band band = p_->manager->currentBand();
        if (args.contains(QStringLiteral("band"))
            && !parseBand(args.value(QStringLiteral("band")), &band)) {
            sendError(reqId, msg, ErrorCode::InvalidParameter, QStringLiteral("invalid band"));
            break;
        }
        const Result r = p_->manager->clearStations(band);
        sendResult(reqId, msg, r);
        if (r.ok()) publishStationList(band);
        break;
    }

    case requireFavoriteList: {
        Band band = p_->manager->currentBand();
        if (args.contains(QStringLiteral("band"))
            && !parseBand(args.value(QStringLiteral("band")), &band)) {
            sendError(reqId, msg, ErrorCode::InvalidParameter, QStringLiteral("invalid band"));
            break;
        }
        publishFavorites(band);
        sendResult(reqId, msg, Result::success());
        break;
    }
    case requireAddFavorite: {
        Band band = p_->manager->currentBand();
        const int f = args.value(QStringLiteral("freqKHz")).toInt(0);
        if ((args.contains(QStringLiteral("band"))
             && !parseBand(args.value(QStringLiteral("band")), &band)) || f <= 0) {
            sendError(reqId, msg, ErrorCode::InvalidParameter, QStringLiteral("invalid band or freqKHz"));
            break;
        }
        Station s;
        s.band = band;
        s.frequencyKHz = f;
        s.name = args.value(QStringLiteral("name")).toString();
        s.source = QStringLiteral("manual");
        const Result r = p_->manager->addFavorite(s);
        sendResult(reqId, msg, r);
        if (r.ok()) publishFavorites(band);
        break;
    }
    case requireRemoveFavorite: {
        const QString id = args.value(QStringLiteral("id")).toString();
        const Result r = p_->manager->removeFavorite(id);
        sendResult(reqId, msg, r);
        if (r.ok()) publishFavorites(p_->manager->currentBand());
        break;
    }
    case requireIsFavorite: {
        const QString id = args.value(QStringLiteral("id")).toString();
        const bool favorite = p_->manager->isFavorite(id);
        send(updateIsFavorite, QJsonObject{
            {"reqId", jsonId(reqId)},
            {"id", id},
            {"favorite", favorite},
        });
        sendResult(reqId, msg, Result::success());
        break;
    }

    default:
        HSRADIO_LOG_WARNING(hsradioCore).noquote() << "unsupported msg" << msg;
        sendError(reqId, msg, ErrorCode::InvalidParameter, QStringLiteral("unsupported message id"));
        break;
    }
}

void HSRadioCore::accept(quint64 reqId, int msg, RequestId id)
{
    if (id == 0) {
        sendError(reqId, msg, ErrorCode::HardwareNotReady, QStringLiteral("scheduler not ready"));
        return;
    }
    p_->pending.insert(id, reqId);
    send(updateAccepted, QJsonObject{
        {"reqId", jsonId(reqId)},
        {"requestId", jsonId(id)},
        {"msg", msg},
    });
}

void HSRadioCore::sendResult(quint64 reqId, int msg, const Result &r)
{
    send(updateResult, QJsonObject{
        {"reqId", jsonId(reqId)},
        {"msg", msg},
        {"ok", r.ok()},
        {"code", errorCodeName(r.code)},
        {"message", r.message},
    });
}

void HSRadioCore::sendError(quint64 reqId, int msg, ErrorCode code, const QString &text)
{
    HSRADIO_LOG_WARNING(hsradioCore).noquote()
        << "error:" << errorCodeName(code) << text;
    send(updateError, QJsonObject{
        {"reqId", jsonId(reqId)},
        {"msg", msg},
        {"code", errorCodeName(code)},
        {"message", text},
    });
}

void HSRadioCore::publishState(void)
{
    HSRadioManager *m = p_->manager.get();
    send(updateState, QJsonObject{
        {"band", bandName(m->currentBand())},
        {"freqKHz", m->currentFrequencyKHz()},
        {"muted", p_->lastState.muted},
        {"initialized", m->isInitialized()},
        {"searching", m->isSearching()},
        {"hardwareTuned", m->isHardwareTuned()},
    });
}

void HSRadioCore::publishStationList(Band band)
{
    send(updateStationList, QJsonObject{
        {"band", bandName(band)},
        {"stations", stationsToJson(p_->manager->stations(band))},
    });
}

void HSRadioCore::publishFavorites(Band band)
{
    send(updateFavorites, QJsonObject{
        {"band", bandName(band)},
        {"favorites", stationsToJson(p_->manager->favorites(band))},
    });
}

void HSRadioCore::publishConfig(void)
{
    HSRadioRepository *repo = p_->repository.get();
    send(updateConfig, QJsonObject{
        {"region", repo->region()},
        {"bands", QJsonObject{
            {"FM", bandConfigToJson(repo->bandConfig(Band::FM))},
            {"AM", bandConfigToJson(repo->bandConfig(Band::AM))},
        }},
        {"seek", QJsonObject{
            {"FM", thresholdToJson(repo->fmSeekThreshold())},
            {"AM", thresholdToJson(repo->amSeekThreshold())},
            {"upward", repo->seekUpward()},
            {"wrapAround", repo->seekWrapAround()},
            {"sampleMs", repo->seekSampleMs()},
            {"sampleCount", repo->seekSampleCount()},
            {"maxInvalid", repo->seekMaxInvalid()},
        }},
    });
}
