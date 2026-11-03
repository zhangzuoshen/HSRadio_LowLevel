#pragma once

#include "hsradio/HSRadioTypes.h"
#include "hsradio/hardware/IRadioHardware.h"

#include <QObject>
#include <memory>

namespace HSRadio {

// 操作调度器：串行执行硬件命令。
// 对象创建于主线程；内部持有工作线程，所有 IRadioHardware 调用
// 只在该工作线程上发生。命令一次执行一条（FIFO 串行）。
class HSRadioOperationScheduler : public QObject {
    Q_OBJECT
public:
    explicit HSRadioOperationScheduler(QObject *parent = nullptr);
    ~HSRadioOperationScheduler() override;

    // 启动工作线程并接管硬件对象（调度器持有其生命周期）。
    Result initialize(std::unique_ptr<IRadioHardware> hardware);
    void   shutdown();       // 停止线程并释放硬件

    // —— 原子命令（异步，串行入队）——
    RequestId submitSetBand(Band band);
    RequestId submitTune(int frequencyKHz, IRadioHardware::TuneMode mode);
    RequestId submitSetMute(bool muted);
    RequestId submitReadState();
    RequestId submitReadSignal();

    // —— 复合长任务（在工作线程内跑扫描循环，独占线程）——
    RequestId submitSeek(const SeekRequest &request);
    void      cancelSeek();   // 置取消标志，最高优先、异步生效

signals:
    void initialized(ErrorCode code);
    void operationFinished(quint64 id, ErrorCode code);
    void stateRead(quint64 id, const HardwareState &state);
    void signalRead(quint64 id, const SignalInfo &info);
    void seekStarted(quint64 id);
    void seekProgress(quint64 id, int frequencyKHz);
    void seekHit(quint64 id, int frequencyKHz, const SignalInfo &info);
    void seekFinished(quint64 id, int frequencyKHz, bool found);
    void errorOccurred(ErrorCode code, const QString &message);

private:
    class Worker;             // 内部工作对象（定义在 .cpp）
    Worker   *m_worker = nullptr;
    RequestId m_nextId = 0;
};

} // namespace HSRadio
