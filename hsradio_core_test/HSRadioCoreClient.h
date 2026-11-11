#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QVector>

#include "HSMWatcher.h"

// 平台消息层客户端：只收发 Group::Radio 上的 HSRadioMsg 消息，
// 不持有任何服务端实现（Core / Manager / 硬件后端都不在本进程）。
// 接口按测试需要自定义，不照抄 RadioProxy 的 Q_PROPERTY / Q_INVOKABLE 集合。
class HSRadioCoreClient : public HSMWatcher
{
    Q_OBJECT

public:
    explicit HSRadioCoreClient(QObject *parent = Q_NULLPTR);

    // 不要 override name()：框架按 (group, name) 建 channel，
    // 而 send() 用 chan(group, "") 查找，非空 name 会导致发送失败。
    int groupID(void) override;
    QVector<int> listenedGroups(void) override;

    void onLoaded(void) override;
    void onMessage(int msg, const QJsonValue &value) override;
    void onMessage(int id, int msg, const QJsonValue &value) override;

    // 发送一个请求，自动补 reqId，返回本次 reqId。
    quint64 sendRequest(int msg, const QJsonObject &args = QJsonObject());

    static QString messageName(int msg);
    static QString jsonText(const QJsonValue &value);

    int sentCount(void) const;
    int receivedCount(void) const;
    int failedCount(void) const;
    int errorCount(void) const;

signals:
    void responseReceived(int msg, const QJsonValue &value);

private:
    void handleMessage(int msg, const QJsonValue &value);

    quint64 m_nextReqId = 0;
    int m_sent = 0;
    int m_received = 0;
    int m_failed = 0;
    int m_errors = 0;
};
