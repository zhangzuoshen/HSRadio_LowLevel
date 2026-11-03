#pragma once

#include "hsradio/HSRadioTypes.h"

namespace HSRadio {

// 单次信号采样的有效性判定规则。
//
// 纯函数、无副作用、不含计时与 I/O，便于单元测试，也便于调度器与
// 未来的诊断工具复用同一套判定标准。
//
// 判定条件（与参考驱动时序一致）：
//   rssi >= minRssi 且 usn <= maxUsn 且 wam <= maxWam 且 offset <= maxOffset。
// 驱动未上报的指标（hasXxx 为 false）一律按"不满足"处理，
// 避免把"没读到"误判成"读到了好信号"。
inline bool isStationValid(const SignalInfo &info, const SeekThreshold &threshold)
{
    if (!info.hasRssi || info.rssi < threshold.minRssi)
        return false;
    if (!info.hasUsn || info.usn > threshold.maxUsn)
        return false;
    if (!info.hasWam || info.wam > threshold.maxWam)
        return false;
    if (!info.hasOffset || info.offset > threshold.maxOffset)
        return false;
    return true;
}

} // namespace HSRadio
