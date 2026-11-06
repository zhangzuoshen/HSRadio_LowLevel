#pragma once

#include "hsradio/HSRadioTypes.h"

#include <QVector>

namespace HSRadio {

// 状态存储：负责 radio_state.json 的读写与内存镜像。
// 仅做数据持久化与内存管理，不含任何硬件 / 线程逻辑。
// 所有写操作只更新内存；是否落盘由调用方通过 save() 决定。
class HSRadioStationStore {
public:
    explicit HSRadioStationStore(const QString &stateFilePath);

    // 从磁盘加载；文件不存在时用内置默认（返回 success）。
    // JSON 解析失败才返回 failure（此时内存保持内置默认）。
    Result load();
    // 写回磁盘；policy 决定 .bak 的生成/保留/清除（默认生成）。
    Result save(JsonBackupPolicy policy = JsonBackupPolicy::Create);
    // 恢复出厂：当前频段回 FM、频率回默认、清空电台与收藏。
    Result resetToDefault();

    // —— 当前频段 / 上次频率（每频段各记一份）——
    Band currentBand() const;
    void setCurrentBand(Band band);

    int  lastFrequency(Band band) const;
    void setLastFrequency(Band band, int frequencyKHz);

    // —— 电台列表（FM / AM 各一份）——
    QVector<Station> stations(Band band) const;
    // 按 id upsert（同 id 覆盖，返回 success）；id 为空时自动生成。
    Result addStation(const Station &station);
    Result removeStation(const QString &id);
    Result renameStation(const QString &id, const QString &name);
    Result clearStations(Band band);

    // —— 收藏（FM / AM 各一份；自包含 id + frequencyKHz）——
    QVector<Station> favorites(Band band) const;
    Result addFavorite(const Station &station);
    Result removeFavorite(const QString &id);
    bool   isFavorite(const QString &id) const;

    // 生成电台 id，如 "fm-87500"。频率为 0 时返回空串。
    static QString makeStationId(Band band, int frequencyKHz);

private:
    const QVector<Station> *list(Band band, bool favorites) const;
    QVector<Station> *mutableList(Band band, bool favorites);
    static int indexOfId(const QVector<Station> &list, const QString &id);

    QString m_stateFilePath;
    Band    m_currentBand = Band::FM;
    int     m_lastFrequencyFM = 87500;
    int     m_lastFrequencyAM = 531;
    QVector<Station> m_stationsFM;
    QVector<Station> m_stationsAM;
    QVector<Station> m_favoritesFM;
    QVector<Station> m_favoritesAM;
};

} // namespace HSRadio
