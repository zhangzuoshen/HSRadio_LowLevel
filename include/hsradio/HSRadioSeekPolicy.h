#pragma once

#include "hsradio/HSRadioTypes.h"

namespace HSRadio {

// 单次信号采样的有效性判定规则（调用方按 band 传入对应的一组阈值）。
//
// 纯函数、无副作用、不含计时与 I/O，便于单元测试，也便于调度器与
// 未来的诊断工具复用同一套判定标准。
//
// 判定口径（与参考驱动一致）：
//   FM：rssi >= th.minRssi 且 usn <= th.maxUsn 且 wam <= th.maxWam 且 offset <= th.maxOffset
//   AM：rssi >= th.minRssi 且 offset <= th.maxOffset 且 modulation <= th.maxModulation
//       （AM 不判 usn/wam，对应字段填 0）
// 驱动未上报的指标（hasXxx 为 false）一律按"不满足"处理，
// 避免把"没读到"误判成"读到了好信号"。
inline bool isStationValid(const SignalInfo &info, Band band,
                           const SeekThreshold &th)
{
    if (band == Band::FM) {
        if (!info.hasRssi || info.rssi < th.minRssi)
            return false;
        if (!info.hasUsn || info.usn > th.maxUsn)
            return false;
        if (!info.hasWam || info.wam > th.maxWam)
            return false;
        if (!info.hasOffset || info.offset > th.maxOffset)
            return false;
        return true;
    }

    // AM
    if (!info.hasRssi || info.rssi < th.minRssi)
        return false;
    if (!info.hasOffset || info.offset > th.maxOffset)
        return false;
    if (!info.hasModulation || info.modulation > th.maxModulation)
        return false;
    return true;
}

} // namespace HSRadio
