#pragma once

// HSRadioCore 对外消息 ID（Group::Radio）。
//
// 平台旧实现 HSMsgDefs.h 里的 Radio::* 由 HS_DEF_MSG 宏生成，
// 是 Group::Radio 组内从 0 起连续枚举，并会在 namespace Radio 里生成
// EM_Message__ / HSRadioMessage__Msg2Str__ 等固定名字。
// 本头刻意不依赖平台宏、也不放进 namespace Radio：
//   - 避免与 HSMsgDefs.h 在同一翻译单元里重复定义（编译错误）；
//   - ID 另开 0x1000 / 0x1100 两段，不与旧消息撞值，新旧消息按数值区分；
//   - 不调用 HSMConnecter::registMsgIDString，不覆盖平台日志的 ID->名字表。
// 客户端与 Core 需使用同一份定义；本头是纯 C++ 头，不依赖 Qt / 平台库。
//
// 数据口径：频率一律 kHz（FM 87500~108000、AM 531~1602），
// 频段只有 FM / AM 两种；每个请求在 QJsonObject 里带 reqId，
// 异步结果按同一 reqId 回执。

namespace HSRadioMsg {

// 请求：客户端 -> Core。
enum Request : int {
    requireReady            = 0x1001, // 查询 Core 是否就绪
    requireRestore          = 0x1002, // 恢复上次状态（异步）
    requireFactoryReset     = 0x1003, // 恢复出厂（同步）
    requireState            = 0x1004, // 查询当前状态（同步）
    requireConfig           = 0x1005, // 查询地区/频段/阈值（同步）

    requireSetBand          = 0x1010, // {"band":"FM|AM"}，异步
    requireSetFreq          = 0x1011, // {"freqKHz":94900}，异步
    requireTuneUp           = 0x1012, // 步进 +1（异步）
    requireTuneDown         = 0x1013, // 步进 -1（异步）
    requireSetMute          = 0x1014, // {"mute":true|false}，异步
    requireRefreshSignal    = 0x1015, // 刷新信号质量（异步）

    requireSeek             = 0x1020, // {"up":true,"mode":"full|next"}，异步
    requireStopSeek         = 0x1021, // 取消搜台（异步）

    requireStationList      = 0x1030, // {"band":"FM|AM"}，同步
    requireAddStation       = 0x1031, // {"band","freqKHz","name"}，同步
    requireRemoveStation    = 0x1032, // {"id":"FM-94900"}，同步
    requireRenameStation    = 0x1033, // {"id","name"}，同步
    requireClearStations    = 0x1034, // {"band"}，同步

    requireFavoriteList     = 0x1040, // {"band"}，同步
    requireAddFavorite      = 0x1041, // {"band","freqKHz","name"}，同步
    requireRemoveFavorite   = 0x1042, // {"id"}，同步
    requireIsFavorite       = 0x1043, // {"id"}，同步
};

// 通知：Core -> 客户端。
enum Update : int {
    updateReady             = 0x1100, // {"module","ok"}
    updateAccepted          = 0x1101, // {"reqId","requestId","msg"}：异步请求已入队
    updateResult            = 0x1102, // {"reqId","msg","ok","code","message"}：同步接口结果
    updateError             = 0x1103, // {"reqId","msg","code","message"}
    updateState             = 0x1110, // {"band","freqKHz","muted","initialized","searching","hardwareTuned"}
    updateBand              = 0x1111, // {"band"}
    updateFreq              = 0x1112, // {"freqKHz"}
    updateSignal            = 0x1113, // {"rssi","usn",...}：只带有效字段
    updateStationList       = 0x1114, // {"band","stations":[...]}
    updateFavorites         = 0x1115, // {"band","favorites":[...]}
    updateConfig            = 0x1116, // {"region","bands":{...},"seek":{...}}
    updateIsFavorite        = 0x1117, // {"reqId","id","favorite"}

    updateSeekStarted       = 0x1120, // {"reqId","requestId","upward"}
    updateSeekProgress      = 0x1121, // {"requestId","freqKHz"}
    updateSeekFinished      = 0x1122, // {"reqId","requestId","freqKHz","found"}
    updateRestoreFinished   = 0x1123, // {"reqId","requestId","code","band","freqKHz"}
    updateOperationFinished = 0x1124, // {"reqId","requestId","code"}
};

} // namespace HSRadioMsg
