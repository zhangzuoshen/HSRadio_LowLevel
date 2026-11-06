#pragma once

#include "hsradio/HSRadioTypes.h"

namespace HSRadio {

// 单次信号采样的有效性判定规则（FM / AM 分两组阈值、分别判定）。
//
// 纯函数、无副作用、不含计时与 I/O，便于单元测试，也便于调度器与
// 未来的诊断工具复用同一套判定标准。
//
// 判定口径（与参考驱动一致）：
//   FM：rssi >= fm.minRssi 且 usn <= fm.maxUsn 且 wam <= fm.maxWam 且 offset <= fm.maxOffset
//   AM：rssi >= am.minRssi 且 offset <= am.maxOffset 且 modulation <= am.maxModulation
//       （AM 不判 usn/wam）
// 驱动未上报的指标（hasXxx 为 false）一律按"不满足"处理，
// 避免把"没读到"误判成"读到了好信号"。
inline bool isStationValid(const SignalInfo &info, Band band,
                           const FmSeekThreshold &fm, const AmSeekThreshold &am)
{
    if (band == Band::FM) {
        if (!info.hasRssi || info.rssi < fm.minRssi)
            return false;
        if (!info.hasUsn || info.usn > fm.maxUsn)
            return false;
        if (!info.hasWam || info.wam > fm.maxWam)
            return false;
        if (!info.hasOffset || info.offset > fm.maxOffset)
            return false;
        return true;
    }

    // AM
    if (!info.hasRssi || info.rssi < am.minRssi)
        return false;
    if (!info.hasOffset || info.offset > am.maxOffset)
        return false;
    if (!info.hasModulation || info.modulation > am.maxModulation)
        return false;
    return true;
}

} // namespace HSRadio
