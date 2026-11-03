#include "hsradio/scheduler/HSRadioOperationScheduler.h"

namespace HSRadio {

// 批 1 桩实现：仅保证接口可编译、可链接，工作线程与队列后续实现。

HSRadioOperationScheduler::HSRadioOperationScheduler(QObject *parent)
    : QObject(parent)
{
}

HSRadioOperationScheduler::~HSRadioOperationScheduler() = default;

Result HSRadioOperationScheduler::initialize(std::unique_ptr<IRadioHardware> hardware)
{
    (void)hardware;   // 批 1 暂不接管硬件
    return {};
}

void HSRadioOperationScheduler::shutdown() {}

RequestId HSRadioOperationScheduler::submitSetBand(Band) { return ++m_nextId; }
RequestId HSRadioOperationScheduler::submitTune(int, IRadioHardware::TuneMode)
{
    return ++m_nextId;
}
RequestId HSRadioOperationScheduler::submitSetMute(bool) { return ++m_nextId; }
RequestId HSRadioOperationScheduler::submitReadState() { return ++m_nextId; }
RequestId HSRadioOperationScheduler::submitReadSignal() { return ++m_nextId; }
RequestId HSRadioOperationScheduler::submitSeek(const SeekRequest &) { return ++m_nextId; }
void      HSRadioOperationScheduler::cancelSeek() {}

} // namespace HSRadio
