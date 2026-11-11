// HSRadioCoreTest —— HSRadioCore 的纯客户端消息调试工具。
//
// 参考平台 RadioProxy 的 watcher 收发方式，但接口按 HSRadioMsg 的 22 个请求
// 自定义为命令行参数；本进程不含任何服务端实现（Core / Manager / 硬件后端）。
// 通过 HSMConnecter::regist() 接入平台 DBus，向 Group::Radio 发请求、
// 打印收到的 update 通知，不做写死的用例与断言。

#include "HSMConnecter.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonObject>
#include <QJsonValue>
#include <QLoggingCategory>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QThread>

#include <initializer_list>

#include "HSRadioCoreClient.h"
#include "hsradio/HSRadioLogging.h"
#include "hsradio/core/HSRadioMsgDefs.h"

// 测试工具专用日志类别，供 main.cpp 与 HSRadioCoreClient.cpp 共用。
Q_LOGGING_CATEGORY(hsradioCoreTest, "hsradio.coretest", QtInfoMsg)

namespace {

QJsonObject makeArgs(std::initializer_list<QPair<QString, QJsonValue>> items)
{
    QJsonObject obj;
    for (const auto &kv : items)
        obj.insert(kv.first, kv.second);
    return obj;
}

bool parseIntegerOption(const QCommandLineParser &parser,
                        const QCommandLineOption &option, int *value)
{
    bool ok = false;
    const int parsed = parser.value(option).toInt(&ok);
    if (!ok)
        return false;
    *value = parsed;
    return true;
}

// "<freqKHz>[:name]"。
bool parseFreqName(const QString &text, int *freq, QString *name)
{
    const int sep = text.indexOf(QLatin1Char(':'));
    bool ok = false;
    *freq = (sep < 0 ? text : text.left(sep)).trimmed().toInt(&ok);
    if (!ok)
        return false;
    *name = sep < 0 ? QString() : text.mid(sep + 1);
    return true;
}

// "<id>:name"。
bool parseIdName(const QString &text, QString *id, QString *name)
{
    const int sep = text.indexOf(QLatin1Char(':'));
    *id = (sep < 0 ? text : text.left(sep)).trimmed();
    if (id->isEmpty())
        return false;
    *name = sep < 0 ? QString() : text.mid(sep + 1);
    return true;
}

bool isBandText(const QString &text)
{
    return text == QStringLiteral("FM") || text == QStringLiteral("AM");
}

void pumpFor(int ms)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
}

} // namespace

int main(int argc, char *argv[])
{
    qSetMessagePattern("[%{time yyyy-MM-dd h:mm:ss.zzz}] [%{if-debug}D%{endif}%{if-info}I%{endif}%{if-warning}W%{endif}%{if-critical}C%{endif}%{if-fatal}F%{endif}] [%{category}] - %{message}");

    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("HSRadioCoreTest"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("HSRadioCore 纯客户端消息调试工具；经平台 DBus 向 Group::Radio 发请求并打印回执。"));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption delayOption(
        QStringLiteral("delay"),
        QStringLiteral("注册后等待 DBus 连接再发请求的时长，毫秒（默认 500）。"),
        QStringLiteral("ms"), QStringLiteral("500"));
    const QCommandLineOption waitOption(
        QStringLiteral("wait"),
        QStringLiteral("发完请求后继续收通知的时长，毫秒（默认 3000）。"),
        QStringLiteral("ms"), QStringLiteral("3000"));

    const QCommandLineOption readyOption(QStringLiteral("ready"), QStringLiteral("requireReady：查询 Core 就绪状态。"));
    const QCommandLineOption restoreOption(QStringLiteral("restore"), QStringLiteral("requireRestore：恢复上次状态（异步）。"));
    const QCommandLineOption factoryResetOption(QStringLiteral("factory-reset"), QStringLiteral("requireFactoryReset：恢复出厂。"));
    const QCommandLineOption stateOption(QStringLiteral("state"), QStringLiteral("requireState：查询当前状态。"));
    const QCommandLineOption configOption(QStringLiteral("config"), QStringLiteral("requireConfig：查询地区/频段/阈值。"));

    const QCommandLineOption setBandOption(QStringLiteral("set-band"), QStringLiteral("requireSetBand：FM 或 AM。"), QStringLiteral("FM|AM"));
    const QCommandLineOption setFreqOption(QStringLiteral("set-freq"), QStringLiteral("requireSetFreq：kHz（可重复）。"), QStringLiteral("kHz"));
    const QCommandLineOption tuneUpOption(QStringLiteral("tune-up"), QStringLiteral("requireTuneUp：步进 +1。"));
    const QCommandLineOption tuneDownOption(QStringLiteral("tune-down"), QStringLiteral("requireTuneDown：步进 -1。"));
    const QCommandLineOption setMuteOption(QStringLiteral("set-mute"), QStringLiteral("requireSetMute：on 或 off。"), QStringLiteral("on|off"));
    const QCommandLineOption refreshOption(QStringLiteral("refresh"), QStringLiteral("requireRefreshSignal：刷新信号质量。"));
    const QCommandLineOption seekOption(QStringLiteral("seek"), QStringLiteral("requireSeek：发起搜台。"));
    const QCommandLineOption seekDownOption(QStringLiteral("seek-down"), QStringLiteral("向下搜台（默认向上）。"));
    const QCommandLineOption seekModeOption(QStringLiteral("seek-mode"), QStringLiteral("full=全频段，next=下一个台（默认 full）。"), QStringLiteral("full|next"), QStringLiteral("full"));
    const QCommandLineOption stopSeekOption(QStringLiteral("stop-seek"), QStringLiteral("requireStopSeek：取消搜台。"));

    const QCommandLineOption stationsOption(QStringLiteral("stations"), QStringLiteral("requireStationList：列出该频段电台（可重复）。"), QStringLiteral("FM|AM"));
    const QCommandLineOption addStationOption(QStringLiteral("add-station"), QStringLiteral("requireAddStation：kHz[:name]（可重复）。"), QStringLiteral("spec"));
    const QCommandLineOption removeStationOption(QStringLiteral("remove-station"), QStringLiteral("requireRemoveStation：按 id 删除电台。"), QStringLiteral("id"));
    const QCommandLineOption renameStationOption(QStringLiteral("rename-station"), QStringLiteral("requireRenameStation：id[:name]。"), QStringLiteral("spec"));
    const QCommandLineOption clearStationsOption(QStringLiteral("clear-stations"), QStringLiteral("requireClearStations：清空该频段电台（可重复）。"), QStringLiteral("FM|AM"));

    const QCommandLineOption favoritesOption(QStringLiteral("favorites"), QStringLiteral("requireFavoriteList：列出该频段收藏（可重复）。"), QStringLiteral("FM|AM"));
    const QCommandLineOption addFavOption(QStringLiteral("add-fav"), QStringLiteral("requireAddFavorite：kHz[:name]（可重复）。"), QStringLiteral("spec"));
    const QCommandLineOption removeFavOption(QStringLiteral("remove-fav"), QStringLiteral("requireRemoveFavorite：按 id 删除收藏。"), QStringLiteral("id"));
    const QCommandLineOption isFavOption(QStringLiteral("is-fav"), QStringLiteral("requireIsFavorite：查询是否已收藏。"), QStringLiteral("id"));

    parser.addOption(delayOption);
    parser.addOption(waitOption);
    parser.addOption(readyOption);
    parser.addOption(restoreOption);
    parser.addOption(factoryResetOption);
    parser.addOption(stateOption);
    parser.addOption(configOption);
    parser.addOption(setBandOption);
    parser.addOption(setFreqOption);
    parser.addOption(tuneUpOption);
    parser.addOption(tuneDownOption);
    parser.addOption(setMuteOption);
    parser.addOption(refreshOption);
    parser.addOption(seekOption);
    parser.addOption(seekDownOption);
    parser.addOption(seekModeOption);
    parser.addOption(stopSeekOption);
    parser.addOption(stationsOption);
    parser.addOption(addStationOption);
    parser.addOption(removeStationOption);
    parser.addOption(renameStationOption);
    parser.addOption(clearStationsOption);
    parser.addOption(favoritesOption);
    parser.addOption(addFavOption);
    parser.addOption(removeFavOption);
    parser.addOption(isFavOption);

    parser.process(app);

    int delayMs = 500, waitMs = 3000;
    if (!parseIntegerOption(parser, delayOption, &delayMs) || delayMs < 0
        || !parseIntegerOption(parser, waitOption, &waitMs) || waitMs < 0) {
        HSRADIO_LOG_CRITICAL(hsradioCoreTest).noquote() << "Invalid --delay / --wait";
        return 2;
    }

    HSRadioCoreClient client;
    if (!HSMConnecter::regist(&client)) {
        HSRADIO_LOG_CRITICAL(hsradioCoreTest).noquote() << "[FAIL] HSMConnecter::regist failed";
        return 1;
    }

    // DBus 连接由框架线程异步建立，先泵一会儿事件再发请求，避免首包丢失。
    pumpFor(delayMs);

    bool anyInvalid = false;
    auto invalid = [&](const QString &text) {
        HSRADIO_LOG_CRITICAL(hsradioCoreTest).noquote() << "Invalid argument:" << text;
        anyInvalid = true;
    };
    auto request = [&](int msg, const QJsonObject &args = QJsonObject()) {
        client.sendRequest(msg, args);
    };

    if (parser.isSet(readyOption)) request(HSRadioMsg::requireReady);
    if (parser.isSet(restoreOption)) request(HSRadioMsg::requireRestore);
    if (parser.isSet(factoryResetOption)) request(HSRadioMsg::requireFactoryReset);
    if (parser.isSet(stateOption)) request(HSRadioMsg::requireState);
    if (parser.isSet(configOption)) request(HSRadioMsg::requireConfig);

    if (parser.isSet(setBandOption)) {
        const QString t = parser.value(setBandOption).trimmed().toUpper();
        if (isBandText(t))
            request(HSRadioMsg::requireSetBand, makeArgs({{QStringLiteral("band"), t}}));
        else
            invalid(QStringLiteral("--set-band ") + t);
    }
    for (const QString &text : parser.values(setFreqOption)) {
        bool ok = false;
        const int f = text.trimmed().toInt(&ok);
        if (!ok || f <= 0) { invalid(QStringLiteral("--set-freq ") + text); continue; }
        request(HSRadioMsg::requireSetFreq, makeArgs({{QStringLiteral("freqKHz"), f}}));
    }
    if (parser.isSet(tuneUpOption)) request(HSRadioMsg::requireTuneUp);
    if (parser.isSet(tuneDownOption)) request(HSRadioMsg::requireTuneDown);
    if (parser.isSet(setMuteOption)) {
        const QString t = parser.value(setMuteOption).trimmed().toLower();
        if (t == QStringLiteral("on") || t == QStringLiteral("off"))
            request(HSRadioMsg::requireSetMute,
                    makeArgs({{QStringLiteral("mute"), t == QStringLiteral("on")}}));
        else
            invalid(QStringLiteral("--set-mute ") + t);
    }
    if (parser.isSet(refreshOption)) request(HSRadioMsg::requireRefreshSignal);
    if (parser.isSet(seekOption)) {
        const QString mode = parser.value(seekModeOption).trimmed().toLower();
        if (mode == QStringLiteral("full") || mode == QStringLiteral("next"))
            request(HSRadioMsg::requireSeek,
                    makeArgs({{QStringLiteral("up"), !parser.isSet(seekDownOption)},
                              {QStringLiteral("mode"), mode}}));
        else
            invalid(QStringLiteral("--seek-mode ") + mode);
    }
    if (parser.isSet(stopSeekOption)) request(HSRadioMsg::requireStopSeek);

    for (const QString &t : parser.values(stationsOption)) {
        const QString band = t.trimmed().toUpper();
        if (isBandText(band))
            request(HSRadioMsg::requireStationList, makeArgs({{QStringLiteral("band"), band}}));
        else
            invalid(QStringLiteral("--stations ") + t);
    }
    for (const QString &t : parser.values(addStationOption)) {
        int f = 0; QString name;
        if (!parseFreqName(t, &f, &name) || f <= 0) { invalid(QStringLiteral("--add-station ") + t); continue; }
        request(HSRadioMsg::requireAddStation,
                makeArgs({{QStringLiteral("freqKHz"), f}, {QStringLiteral("name"), name}}));
    }
    if (parser.isSet(removeStationOption))
        request(HSRadioMsg::requireRemoveStation,
                makeArgs({{QStringLiteral("id"), parser.value(removeStationOption)}}));
    if (parser.isSet(renameStationOption)) {
        QString id, name;
        if (parseIdName(parser.value(renameStationOption), &id, &name))
            request(HSRadioMsg::requireRenameStation,
                    makeArgs({{QStringLiteral("id"), id}, {QStringLiteral("name"), name}}));
        else
            invalid(QStringLiteral("--rename-station ") + parser.value(renameStationOption));
    }
    for (const QString &t : parser.values(clearStationsOption)) {
        const QString band = t.trimmed().toUpper();
        if (isBandText(band))
            request(HSRadioMsg::requireClearStations, makeArgs({{QStringLiteral("band"), band}}));
        else
            invalid(QStringLiteral("--clear-stations ") + t);
    }

    for (const QString &t : parser.values(favoritesOption)) {
        const QString band = t.trimmed().toUpper();
        if (isBandText(band))
            request(HSRadioMsg::requireFavoriteList, makeArgs({{QStringLiteral("band"), band}}));
        else
            invalid(QStringLiteral("--favorites ") + t);
    }
    for (const QString &t : parser.values(addFavOption)) {
        int f = 0; QString name;
        if (!parseFreqName(t, &f, &name) || f <= 0) { invalid(QStringLiteral("--add-fav ") + t); continue; }
        request(HSRadioMsg::requireAddFavorite,
                makeArgs({{QStringLiteral("freqKHz"), f}, {QStringLiteral("name"), name}}));
    }
    if (parser.isSet(removeFavOption))
        request(HSRadioMsg::requireRemoveFavorite,
                makeArgs({{QStringLiteral("id"), parser.value(removeFavOption)}}));
    if (parser.isSet(isFavOption))
        request(HSRadioMsg::requireIsFavorite,
                makeArgs({{QStringLiteral("id"), parser.value(isFavOption)}}));

    pumpFor(waitMs);

    HSRADIO_LOG_INFO(hsradioCoreTest).noquote()
        << QStringLiteral("[CORE] summary: sent=%1 received=%2 failed=%3 error=%4")
               .arg(client.sentCount()).arg(client.receivedCount())
               .arg(client.failedCount()).arg(client.errorCount());
    if (client.receivedCount() == 0)
        HSRADIO_LOG_WARNING(hsradioCoreTest).noquote()
            << "no response received; check DBUS_SESSION_BUS_ADDRESS and whether HSRadio plugin is running";

    const int rc = (anyInvalid || client.failedCount() > 0 || client.errorCount() > 0) ? 1 : 0;
    HSRADIO_LOG_INFO(hsradioCoreTest).noquote() << QStringLiteral("[CORE] exit=%1").arg(rc);
    return rc;
}
