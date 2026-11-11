#include "HSRadioCoreClient.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>

#include "HSGroupDefs.h"
#include "hsradio/HSRadioLogging.h"
#include "hsradio/core/HSRadioMsgDefs.h"

// 日志类别定义在 main.cpp，这里只做声明，两边共用 hsradio.coretest。
Q_DECLARE_LOGGING_CATEGORY(hsradioCoreTest)

namespace {

QHash<int, QString> makeMessageNames()
{
    QHash<int, QString> names;
#define ADD_MSG(x) names.insert(HSRadioMsg::x, QStringLiteral(#x))
    ADD_MSG(requireReady);
    ADD_MSG(requireRestore);
    ADD_MSG(requireFactoryReset);
    ADD_MSG(requireState);
    ADD_MSG(requireConfig);
    ADD_MSG(requireSetBand);
    ADD_MSG(requireSetFreq);
    ADD_MSG(requireTuneUp);
    ADD_MSG(requireTuneDown);
    ADD_MSG(requireSetMute);
    ADD_MSG(requireRefreshSignal);
    ADD_MSG(requireSeek);
    ADD_MSG(requireStopSeek);
    ADD_MSG(requireStationList);
    ADD_MSG(requireAddStation);
    ADD_MSG(requireRemoveStation);
    ADD_MSG(requireRenameStation);
    ADD_MSG(requireClearStations);
    ADD_MSG(requireFavoriteList);
    ADD_MSG(requireAddFavorite);
    ADD_MSG(requireRemoveFavorite);
    ADD_MSG(requireIsFavorite);

    ADD_MSG(updateReady);
    ADD_MSG(updateAccepted);
    ADD_MSG(updateResult);
    ADD_MSG(updateError);
    ADD_MSG(updateState);
    ADD_MSG(updateBand);
    ADD_MSG(updateFreq);
    ADD_MSG(updateSignal);
    ADD_MSG(updateStationList);
    ADD_MSG(updateFavorites);
    ADD_MSG(updateConfig);
    ADD_MSG(updateIsFavorite);
    ADD_MSG(updateSeekStarted);
    ADD_MSG(updateSeekProgress);
    ADD_MSG(updateSeekFinished);
    ADD_MSG(updateRestoreFinished);
    ADD_MSG(updateOperationFinished);
#undef ADD_MSG
    return names;
}

} // namespace

HSRadioCoreClient::HSRadioCoreClient(QObject *parent)
    : HSMWatcher(parent)
{
}

int HSRadioCoreClient::groupID(void)
{
    return Group::Radio;
}

QVector<int> HSRadioCoreClient::listenedGroups(void)
{
    return {groupID()};
}

void HSRadioCoreClient::onLoaded(void)
{
    HSRADIO_LOG_INFO(hsradioCoreTest).noquote()
        << "client loaded; group=" << groupID();
}

void HSRadioCoreClient::onMessage(int msg, const QJsonValue &value)
{
    handleMessage(msg, value);
}

void HSRadioCoreClient::onMessage(int id, int msg, const QJsonValue &value)
{
    // 框架把监听组内所有消息送到这里；本客户端只关心 Radio 组。
    if (id != groupID())
        return;
    handleMessage(msg, value);
}

quint64 HSRadioCoreClient::sendRequest(int msg, const QJsonObject &args)
{
    QJsonObject payload = args;
    const quint64 reqId = ++m_nextReqId;
    payload.insert(QStringLiteral("reqId"), QJsonValue(static_cast<double>(reqId)));
    HSRADIO_LOG_INFO(hsradioCoreTest).noquote()
        << QStringLiteral("[SEND] reqId=%1 msg=0x%2(%3) %4")
               .arg(reqId).arg(msg, 0, 16)
               .arg(messageName(msg))
               .arg(jsonText(QJsonValue(payload)));
    ++m_sent;
    send(msg, QJsonValue(payload));
    return reqId;
}

QString HSRadioCoreClient::messageName(int msg)
{
    static const QHash<int, QString> names = makeMessageNames();
    return names.value(msg, QStringLiteral("unknown"));
}

QString HSRadioCoreClient::jsonText(const QJsonValue &value)
{
    if (value.isObject())
        return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
    if (value.isArray())
        return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
    if (value.isNull() || value.isUndefined())
        return QStringLiteral("null");
    return value.toVariant().toString();
}

int HSRadioCoreClient::sentCount(void) const
{
    return m_sent;
}

int HSRadioCoreClient::receivedCount(void) const
{
    return m_received;
}

int HSRadioCoreClient::failedCount(void) const
{
    return m_failed;
}

int HSRadioCoreClient::errorCount(void) const
{
    return m_errors;
}

void HSRadioCoreClient::handleMessage(int msg, const QJsonValue &value)
{
    // 同一组内发送者也会收到自己广播的 require* 消息；只统计服务端发来的 update*。
    if (msg < HSRadioMsg::updateReady)
        return;

    ++m_received;
    HSRADIO_LOG_INFO(hsradioCoreTest).noquote()
        << QStringLiteral("[RECV] msg=0x%1(%2) %3")
               .arg(msg, 0, 16).arg(messageName(msg)).arg(jsonText(value));

    switch (msg) {
    case HSRadioMsg::updateResult:
        if (!value.toObject().value(QStringLiteral("ok")).toBool())
            ++m_failed;
        break;
    case HSRadioMsg::updateOperationFinished:
        if (value.toObject().value(QStringLiteral("code")).toString()
            != QStringLiteral("None"))
            ++m_failed;
        break;
    case HSRadioMsg::updateError:
        ++m_errors;
        break;
    default:
        break;
    }
    emit responseReceived(msg, value);
}
