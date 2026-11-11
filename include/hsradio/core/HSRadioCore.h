#pragma once

#include <QJsonValue>
#include <QObject>
#include <QVector>

#include "HSMWatcher.h"
#include "HSGroupDefs.h"

#include "hsradio/HSRadioTypes.h"
#include "hsradio/core/HSRadioMsgDefs.h"

// 平台消息层门面：继承 HSMWatcher，把 HSRadioManager 的业务接口
// 以 Group::Radio 消息的形式释放给客户端。
//
// 本类编入 hsradio_hardware 静态库，不是独立插件；插件元数据
// （Q_PLUGIN_METADATA）与 MODULE_NAME 由承载它的插件工程提供。
// 频率单位 kHz、频段仅 FM/AM、请求带 reqId，详见 HSRadioMsgDefs.h。
//
// 编译期需要平台头：HSMessageFramework/include、HSCommon/include、
// HSTL/include、thirdparty；链接期由最终插件提供 HSMessageFramework。

class HSRadioCore_;

class HSRadioCore : public HSMWatcher
{
    Q_OBJECT
    Q_INTERFACES(HSMWatcher)

public:
    explicit HSRadioCore(QObject *parent = Q_NULLPTR);
    ~HSRadioCore(void) override;

    // 目前只接本组消息；Setting / System / Diag 按需再扩展。
    QVector<int> listenedGroups(void) override;
    int groupID(void) override;

    void onLoaded(void) override;
    void onMessage(int msg, const QJsonValue &value) override;
    void onMessage(int id, int msg, const QJsonValue &value) override;

private:
    friend class HSRadioCore_;

    void dispatch(int msg, const QJsonValue &value);
    void onSchedulerReady(HSRadio::ErrorCode code);
    void connectManagerSignals(void);

    // 异步请求入队后记录 reqId <-> 调度器 RequestId 的映射并回执。
    void accept(quint64 reqId, int msg, HSRadio::RequestId id);
    void sendResult(quint64 reqId, int msg, const HSRadio::Result &r);
    void sendError(quint64 reqId, int msg, HSRadio::ErrorCode code, const QString &text);

    void publishState(void);
    void publishStationList(HSRadio::Band band);
    void publishFavorites(HSRadio::Band band);
    void publishConfig(void);

    HSRadioCore_ *p_ = nullptr;
};
