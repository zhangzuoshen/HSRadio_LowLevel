#include "hsradio/scheduler/HSRadioOperationScheduler.h"

#include "hsradio/HSRadioSeekPolicy.h"
#include "hsradio/HSRadioLogging.h"

#include <QMutex>
#include <QMutexLocker>
#include <QObject>
#include <QThread>
#include <QtGlobal>
#include <QWaitCondition>

#include <atomic>
#include <deque>
#include <memory>
#include <utility>

namespace HSRadio {

namespace {

// 进度信号抽稀步长：全频段扫描有 200+ 个频点，
// 若每点都发一次跨线程信号会让接收端事件队列积压，故按步长抽稀。
constexpr int kProgressStride = 8;

// 工作线程队列中的命令种类。
enum class CommandType {
    Initialize,
    SetBand,
    Tune,
    SetMute,
    ReadState,
    ReadSignal,
    Seek,
    Shutdown
};

// 一条排队中的硬件命令。
// 采用"大结构体 + 判别式"而非多态，避免每个命令一次堆分配。
struct Command {
    CommandType type = CommandType::ReadState;
    RequestId   id = 0;
    Band        band = Band::FM;
    int         frequencyKHz = 0;
    IRadioHardware::TuneMode tuneMode = IRadioHardware::TuneMode::Preset;
    bool        muted = false;
    SeekRequest seek;
};

} // namespace

// 工作线程：持有硬件对象，按 FIFO 串行执行队列中的命令。
//
// 不声明 Q_OBJECT：本类不定义自己的信号，一律通过所属调度器对象发信号。
// 从本线程 emit 主线程对象的信号是允许的：Qt 会按 Qt::AutoConnection
// 自动把调用排队到接收者所在线程，因此接收方的槽始终在主线程执行。
class HSRadioOperationScheduler::Worker : public QThread {
public:
    explicit Worker(HSRadioOperationScheduler *owner)
        : m_owner(owner)
    {
    }

    void attach(std::unique_ptr<IRadioHardware> hardware) { m_hardware = std::move(hardware); }

    // 线程安全入队。返回 false 表示工作线程已停止，命令未被接受。
    bool post(const Command &command)
    {
        QMutexLocker locker(&m_mutex);
        if (m_stop)
            return false;
        m_queue.push_back(command);
        m_cond.wakeOne();
        return true;
    }

    // 请求停止，并把尚未执行的命令交回调用方（用于补发失败通知）。
    std::deque<Command> requestStop()
    {
        QMutexLocker locker(&m_mutex);
        m_stop = true;
        std::deque<Command> pending;
        pending.swap(m_queue);
        m_cond.wakeAll();
        return pending;
    }

    // 取消标志：不进队列、不重排，直接置位，扫描循环每步检查。
    void requestCancelSeek() { m_cancelSeek.store(true); }

    // 提交新的搜台请求前清除上一次遗留的取消标志。
    void resetCancel() { m_cancelSeek.store(false); }

protected:
    void run() override;

private:
    void execute(const Command &command);
    void executeSeek(const Command &command);
    bool sampleStationValid(IRadioHardware &hardware,
                            const SeekRequest &request,
                            SignalInfo *lastInfo);

    HSRadioOperationScheduler *m_owner = nullptr;
    std::unique_ptr<IRadioHardware> m_hardware;

    QMutex m_mutex;
    QWaitCondition m_cond;
    std::deque<Command> m_queue;
    bool m_stop = false;

    std::atomic<bool> m_cancelSeek{false};
};

void HSRadioOperationScheduler::Worker::run()
{
    for (;;) {
        Command command;
        {
            QMutexLocker locker(&m_mutex);
            while (m_queue.empty() && !m_stop)
                m_cond.wait(&m_mutex);
            if (m_queue.empty())
                break;   // 只有 m_stop 为真且队列已空时才会走到这里
            command = m_queue.front();
            m_queue.pop_front();
        }

        if (command.type == CommandType::Shutdown)
            break;

        execute(command);
    }

    // 硬件对象在本线程内使用，也在本线程内释放，避免跨线程析构。
    if (m_hardware) {
        m_hardware->shutdown();
        m_hardware.reset();
    }
}

void HSRadioOperationScheduler::Worker::execute(const Command &command)
{
    if (!m_hardware) {
        HSRADIO_LOG_WARNING(hsradioManager).noquote() << "execute: no hardware, op" << command.id << "HardwareNotReady";
        emit m_owner->operationFinished(command.id, ErrorCode::HardwareNotReady);
        return;
    }

    switch (command.type) {
    case CommandType::Initialize: {
        const Result result = m_hardware->initialize();
        if (!result.ok())
            emit m_owner->errorOccurred(result.code, result.message);
        emit m_owner->initialized(result.code);
        break;
    }
    case CommandType::SetBand: {
        const Result result = m_hardware->setBand(command.band);
        if (!result.ok())
            emit m_owner->errorOccurred(result.code, result.message);
        emit m_owner->operationFinished(command.id, result.code);
        break;
    }
    case CommandType::Tune: {
        const Result result = m_hardware->tune(command.frequencyKHz, command.tuneMode);
        if (!result.ok())
            emit m_owner->errorOccurred(result.code, result.message);
        emit m_owner->operationFinished(command.id, result.code);
        break;
    }
    case CommandType::SetMute: {
        const Result result = m_hardware->setMute(command.muted);
        if (!result.ok())
            emit m_owner->errorOccurred(result.code, result.message);
        emit m_owner->operationFinished(command.id, result.code);
        break;
    }
    case CommandType::ReadState: {
        HardwareState state;
        const Result result = m_hardware->readState(state);
        if (result.ok())
            emit m_owner->stateRead(command.id, state);
        else
            emit m_owner->errorOccurred(result.code, result.message);
        emit m_owner->operationFinished(command.id, result.code);
        break;
    }
    case CommandType::ReadSignal: {
        SignalInfo info;
        const Result result = m_hardware->readSignalInfo(info);
        if (result.ok())
            emit m_owner->signalRead(command.id, info);
        else
            emit m_owner->errorOccurred(result.code, result.message);
        emit m_owner->operationFinished(command.id, result.code);
        break;
    }
    case CommandType::Seek:
        executeSeek(command);
        break;
    case CommandType::Shutdown:
        break;
    }
}

bool HSRadioOperationScheduler::Worker::sampleStationValid(IRadioHardware &hardware,
                                                          const SeekRequest &request,
                                                          SignalInfo *lastInfo)
{
    // 调谐后先等 PLL 锁定，再多次抽样（参考驱动实现：8ms 锁定 + 2ms x 4）。
    if (request.lockMs > 0)
        QThread::msleep(static_cast<unsigned long>(request.lockMs));

    const int samples = qMax(1, request.sampleCount);
    const int maxInvalid = qMax(1, request.maxInvalid);

    int validCount = 0;
    int invalidCount = 0;

    for (int i = 0; i < samples; ++i) {
        if (m_cancelSeek.load())
            break;

        SignalInfo info;
        if (!hardware.readSignalInfo(info).ok())
            break;

        if (lastInfo)
            *lastInfo = info;

        if (isStationValid(info, request.threshold))
            ++validCount;
        else
            ++invalidCount;

        // 连续无效达到上限即认为该频点无台，提前结束抽样。
        if (invalidCount >= maxInvalid)
            break;

        if (i + 1 < samples && request.sampleMs > 0)
            QThread::msleep(static_cast<unsigned long>(request.sampleMs));
    }

    // 多次抽样中至少一次有效即判为有效电台。
    return validCount >= 1;
}

void HSRadioOperationScheduler::Worker::executeSeek(const Command &command)
{
    const SeekRequest &request = command.seek;
    const RequestId id = command.id;

    // 命令还在队列里等待时就被取消：直接以"未找到"结束，不做任何硬件动作。
    if (m_cancelSeek.load()) {
        emit m_owner->seekFinished(id, request.startKHz, false);
        emit m_owner->operationFinished(id, ErrorCode::None);
        return;
    }

    if (!m_hardware) {
        emit m_owner->seekFinished(id, request.startKHz, false);
        emit m_owner->operationFinished(id, ErrorCode::HardwareNotReady);
        return;
    }

    if (request.stepKHz <= 0 || request.maxKHz < request.minKHz) {
        emit m_owner->errorOccurred(ErrorCode::InvalidParameter,
                                    QStringLiteral("搜台参数非法：步进必须为正，且上限不得小于下限"));
        emit m_owner->seekFinished(id, request.startKHz, false);
        emit m_owner->operationFinished(id, ErrorCode::InvalidParameter);
        return;
    }

    HSRADIO_LOG_INFO(hsradioManager).noquote() << "seek started id=" << id << "mode=" << (request.mode == SeekMode::FullBand ? "full" : "next");
    emit m_owner->seekStarted(id);

    const Result bandResult = m_hardware->setBand(request.band);
    if (!bandResult.ok()) {
        emit m_owner->errorOccurred(bandResult.code, bandResult.message);
        emit m_owner->seekFinished(id, request.startKHz, false);
        emit m_owner->operationFinished(id, bandResult.code);
        return;
    }

    // 搜台期间强制静音：只有原本未静音时才需要恢复，
    // 原本就静音则保持静音（不能因为搜台结束而替用户解除静音）。
    bool restoreMute = false;
    if (request.muteDuringSeek) {
        HardwareState state;
        if (m_hardware->readState(state).ok() && !state.muted) {
            restoreMute = m_hardware->setMute(true).ok();
            HSRADIO_LOG_DEBUG(hsradioManager).noquote() << "seek: forced mute on during seek";
        }
    }

    const int step = request.stepKHz;
    const int direction = request.upward ? 1 : -1;
    // 一个频段内的总频点数，用于保证全频段扫描恰好走完一圈。
    const int totalPoints = (request.maxKHz - request.minKHz) / step + 1;

    int frequency = qBound(request.minKHz, request.startKHz, request.maxKHz);

    // "找下一个台"要从当前频点的下一格开始，
    // 否则当前正在收听的台会被立刻再次命中，表现为"按了没反应"。
    if (request.mode == SeekMode::NextStation) {
        frequency += direction * step;
        if (frequency > request.maxKHz)
            frequency = request.minKHz;
        else if (frequency < request.minKHz)
            frequency = request.maxKHz;
    }

    bool found = false;
    int stopFrequency = frequency;
    ErrorCode code = ErrorCode::None;

    for (int i = 0; i < totalPoints; ++i) {
        if (m_cancelSeek.load()) {
            HSRADIO_LOG_INFO(hsradioManager).noquote() << "seek cancelled at" << frequency << "kHz";
            break;
        }

        const Result tuneResult = m_hardware->tune(frequency, IRadioHardware::TuneMode::Search);
        if (!tuneResult.ok()) {
            code = tuneResult.code;
            emit m_owner->errorOccurred(tuneResult.code, tuneResult.message);
            break;
        }

        // 只要调谐成功就更新停止点：取消时返回的正是"当时停在的频点"。
        stopFrequency = frequency;

        SignalInfo info;
        if (sampleStationValid(*m_hardware, request, &info)) {
            found = true;
            emit m_owner->seekHit(id, frequency, info);
            if (request.mode == SeekMode::NextStation)
                break;
        }

        if (i % kProgressStride == 0)
            emit m_owner->seekProgress(id, frequency);

        frequency += direction * step;
        if (frequency > request.maxKHz) {
            if (!request.wrapAround)
                break;
            frequency = request.minKHz;
        } else if (frequency < request.minKHz) {
            if (!request.wrapAround)
                break;
            frequency = request.maxKHz;
        }
    }

    // 无论正常结束、被取消还是出错，都恢复进入搜台前的静音状态。
    // 用户在搜台期间按下的静音命令排在队列后面，FIFO 保证最终仍以用户意图为准。
    if (restoreMute)
        m_hardware->setMute(false);

    HSRADIO_LOG_INFO(hsradioManager).noquote() << "seek finished id=" << id << "stop=" << stopFrequency << "kHz found=" << found;
    emit m_owner->seekFinished(id, stopFrequency, found);
    emit m_owner->operationFinished(id, code);
}

HSRadioOperationScheduler::HSRadioOperationScheduler(QObject *parent)
    : QObject(parent)
{
    ::registerHSRadioMetaTypes();
}

HSRadioOperationScheduler::~HSRadioOperationScheduler()
{
    shutdown();
}

Result HSRadioOperationScheduler::initialize(std::unique_ptr<IRadioHardware> hardware)
{
    if (!hardware)
        return Result::failure(ErrorCode::InvalidParameter,
                               QStringLiteral("硬件对象为空"));
    if (m_worker)
        return Result::failure(ErrorCode::HardwareOperationFailed,
                               QStringLiteral("调度器已经初始化，不能重复初始化"));

    m_worker = new Worker(this);

    // 真实后端（如 HSRadioSAF775XBackend）是 QObject 子类，会校验调用线程与
    // QObject::thread() 一致。调度器接管所有权后所有硬件调用都发生在工作线程，
    // 因此在这里一并迁移线程亲和性；Mock 等非 QObject 实现无需迁移。
    // 本方法在主线程执行，符合 moveToThread 必须在对象当前所属线程调用的要求。
    if (QObject *object = dynamic_cast<QObject *>(hardware.get())) {
        // 带 parent 的对象无法迁移线程，且父子关系会与 unique_ptr 重复管理生命周期。
        if (object->parent()) {
            delete m_worker;
            m_worker = nullptr;
            return Result::failure(ErrorCode::InvalidParameter,
                                   QStringLiteral("硬件对象不能带 QObject parent"));
        }
        object->moveToThread(m_worker);
    }

    m_worker->attach(std::move(hardware));
    m_worker->start();
    HSRADIO_LOG_INFO(hsradioManager).noquote() << "scheduler initialized, hardware attached";

    Command command;
    command.type = CommandType::Initialize;
    m_worker->resetCancel();
    if (!m_worker->post(command)) {
        shutdown();
        return Result::failure(ErrorCode::HardwareOperationFailed,
                               QStringLiteral("工作线程启动失败"));
    }
    return {};
}

void HSRadioOperationScheduler::shutdown()
{
    if (!m_worker)
        return;
    HSRADIO_LOG_INFO(hsradioManager).noquote() << "shutdown";

    // 先摘下指针，避免 shutdown 期间新的提交还在用工作对象。
    Worker *worker = m_worker;
    m_worker = nullptr;

    const std::deque<Command> pending = worker->requestStop();
    worker->wait();
    delete worker;

    // 已入队但没轮到执行的命令补发失败通知，
    // 否则调用方会一直等一个永远不会到来的 RequestId。
    for (const Command &command : pending) {
        if (command.type == CommandType::Initialize)
            continue;
        emit operationFinished(command.id, ErrorCode::HardwareNotReady);
    }
}

RequestId HSRadioOperationScheduler::submitSetBand(Band band)
{
    Command command;
    command.type = CommandType::SetBand;
    command.band = band;
    command.id = ++m_nextId;

    if (!m_worker || !m_worker->post(command))
        return 0;
    return command.id;
}

RequestId HSRadioOperationScheduler::submitTune(int frequencyKHz, IRadioHardware::TuneMode mode)
{
    Command command;
    command.type = CommandType::Tune;
    command.frequencyKHz = frequencyKHz;
    command.tuneMode = mode;
    command.id = ++m_nextId;

    if (!m_worker || !m_worker->post(command))
        return 0;
    return command.id;
}

RequestId HSRadioOperationScheduler::submitSetMute(bool muted)
{
    Command command;
    command.type = CommandType::SetMute;
    command.muted = muted;
    command.id = ++m_nextId;

    if (!m_worker || !m_worker->post(command))
        return 0;
    return command.id;
}

RequestId HSRadioOperationScheduler::submitReadState()
{
    Command command;
    command.type = CommandType::ReadState;
    command.id = ++m_nextId;

    if (!m_worker || !m_worker->post(command))
        return 0;
    return command.id;
}

RequestId HSRadioOperationScheduler::submitReadSignal()
{
    Command command;
    command.type = CommandType::ReadSignal;
    command.id = ++m_nextId;

    if (!m_worker || !m_worker->post(command))
        return 0;
    return command.id;
}

RequestId HSRadioOperationScheduler::submitSeek(const SeekRequest &request)
{
    Command command;
    command.type = CommandType::Seek;
    command.seek = request;
    command.id = ++m_nextId;

    if (!m_worker)
        return 0;

    // 清除上一次搜台遗留的取消标志，再入队。
    m_worker->resetCancel();
    if (!m_worker->post(command))
        return 0;
    return command.id;
}

void HSRadioOperationScheduler::cancelSeek()
{
    if (m_worker)
        m_worker->requestCancelSeek();
}

} // namespace HSRadio
