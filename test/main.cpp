// 调度器冒烟测试：在无 ALSA、无真机的环境（Ubuntu/WSL）里验证
// HSRadioOperationScheduler 的串行队列、搜台流程、取消与静音恢复。
//
// 用 MockRadioHardware 充当硬件，因此不需要 i.MX9 与 SAF775X。
// 构建：qmake HSRadioManagerTest.pro && make && ./hsradio_manager_test

#include "hsradio/HSRadioSeekPolicy.h"
#include "hsradio/hardware/MockRadioHardware.h"
#include "hsradio/scheduler/HSRadioOperationScheduler.h"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QObject>
#include <QThread>
#include <QVector>

#include <functional>
#include <memory>

using namespace HSRadio;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char *expression, int line)
{
    ++g_checks;
    if (condition)
        return;
    ++g_failures;
    qCritical().noquote()
        << QStringLiteral("    [FAIL] 第 %1 行：%2").arg(line).arg(QString::fromLatin1(expression));
}

// 用可变参数宏：断言表达式里常含逗号（函数实参、花括号初始化），
// 单参数宏会被预处理器按逗号拆错。
#define CHECK(...) check(static_cast<bool>(__VA_ARGS__), #__VA_ARGS__, __LINE__)

// 主线程事件循环泵：轮询等待条件成立。
// 调度器的信号靠事件循环跨线程投递，所以必须让主线程处理事件。
bool waitUntil(const std::function<bool()> &predicate, int timeoutMs = 15000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate()) {
        if (timer.elapsed() > timeoutMs)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    return true;
}

// 一个"好台"的信号质量：各项都明显优于默认阈值。
SignalInfo strongSignal()
{
    SignalInfo info;
    info.rssi = 40;
    info.usn = 5;
    info.wam = 5;
    info.offset = 10;
    info.stereo = true;
    info.hasRssi = true;
    info.hasUsn = true;
    info.hasWam = true;
    info.hasOffset = true;
    info.hasStereo = true;
    return info;
}

// 低开销搜台参数：真机上才需要 8ms 锁定 + 多次抽样。
SeekRequest fastSeekRequest(SeekMode mode, int startKHz, int minKHz, int maxKHz, int stepKHz)
{
    SeekRequest request;
    request.band = Band::FM;
    request.upward = true;
    request.mode = mode;
    request.startKHz = startKHz;
    request.minKHz = minKHz;
    request.maxKHz = maxKHz;
    request.stepKHz = stepKHz;
    request.lockMs = 1;
    request.sampleMs = 0;
    request.sampleCount = 2;
    request.maxInvalid = 1;
    request.wrapAround = true;
    request.muteDuringSeek = true;
    return request;
}

// 把尚未处理完的排队事件跑掉，避免上一个用例的信号串到下一个用例的统计里。
void drainEvents()
{
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("HSRadioManagerTest"));

    // 预置三个有效电台：87.5 / 98.0 / 104.8 MHz。
    auto hardware = std::unique_ptr<MockRadioHardware>(new MockRadioHardware);
    MockRadioHardware *mock = hardware.get();
    mock->setStationSignal(87500, strongSignal());
    mock->setStationSignal(98000, strongSignal());
    mock->setStationSignal(104800, strongSignal());

    HSRadioOperationScheduler scheduler;

    QVector<quint64> finishedIds;
    QVector<HardwareState> readStates;
    QVector<int> hitFrequencies;
    QVector<int> progressFrequencies;
    int seekStartedCount = 0;
    int seekFinishedCount = 0;
    int seekFoundFlag = -1;
    int seekFinishedFrequency = -1;

    QObject::connect(&scheduler, &HSRadioOperationScheduler::operationFinished, &app,
                     [&](quint64 id, ErrorCode) { finishedIds.append(id); });
    QObject::connect(&scheduler, &HSRadioOperationScheduler::stateRead, &app,
                     [&](quint64, const HardwareState &state) { readStates.append(state); });
    QObject::connect(&scheduler, &HSRadioOperationScheduler::seekStarted, &app,
                     [&](quint64) { ++seekStartedCount; });
    QObject::connect(&scheduler, &HSRadioOperationScheduler::seekHit, &app,
                     [&](quint64, int frequencyKHz, const SignalInfo &) {
                         hitFrequencies.append(frequencyKHz);
                     });
    QObject::connect(&scheduler, &HSRadioOperationScheduler::seekProgress, &app,
                     [&](quint64, int frequencyKHz) { progressFrequencies.append(frequencyKHz); });
    QObject::connect(&scheduler, &HSRadioOperationScheduler::seekFinished, &app,
                     [&](quint64, int frequencyKHz, bool found) {
                         ++seekFinishedCount;
                         seekFinishedFrequency = frequencyKHz;
                         seekFoundFlag = found ? 1 : 0;
                     });

    // —— 用例 1：初始化 ——
    qInfo().noquote() << QStringLiteral("[1] 初始化（Mock 硬件）");
    bool initialized = false;
    ErrorCode initCode = ErrorCode::HardwareOperationFailed;
    QObject::connect(&scheduler, &HSRadioOperationScheduler::initialized, &app,
                     [&](ErrorCode code) { initialized = true; initCode = code; });

    CHECK(scheduler.initialize(std::move(hardware)).ok());
    CHECK(waitUntil([&] { return initialized; }));
    CHECK(initCode == ErrorCode::None);
    // 重复初始化必须被拒绝
    CHECK(!scheduler.initialize(std::unique_ptr<IRadioHardware>(new MockRadioHardware)).ok());

    // —— 用例 2：命令按 FIFO 串行执行 ——
    qInfo().noquote() << QStringLiteral("[2] 命令串行与顺序");
    drainEvents();
    finishedIds.clear();
    readStates.clear();

    const quint64 bandId = scheduler.submitSetBand(Band::FM);
    const quint64 tuneId = scheduler.submitTune(87500, IRadioHardware::TuneMode::Preset);
    const quint64 stateId = scheduler.submitReadState();
    CHECK(bandId != 0);
    CHECK(tuneId > bandId);
    CHECK(stateId > tuneId);
    CHECK(waitUntil([&] { return finishedIds.size() >= 3; }));
    CHECK(finishedIds.mid(0, 3) == (QVector<quint64>{bandId, tuneId, stateId}));
    // 读状态发生在 tune 之后，因此读到的应是调谐后的频率
    CHECK(readStates.size() == 1);
    CHECK(readStates.first().frequencyKHz == 87500);
    // 普通调谐用 Preset 模式，不是搜台的 Search 模式
    CHECK(mock->lastTuneMode() == IRadioHardware::TuneMode::Preset);

    // —— 用例 3：全频段搜台（FullBand），并验证强制静音/恢复 ——
    qInfo().noquote() << QStringLiteral("[3] 全频段搜台");
    drainEvents();
    hitFrequencies.clear();
    progressFrequencies.clear();
    seekStartedCount = 0;
    seekFinishedCount = 0;
    seekFoundFlag = -1;
    mock->resetObservations();

    CHECK(scheduler.submitSeek(fastSeekRequest(SeekMode::FullBand, 87000, 87000, 108000, 100)) != 0);
    CHECK(waitUntil([&] { return seekFinishedCount > 0; }));
    CHECK(seekStartedCount == 1);
    CHECK(hitFrequencies == (QVector<int>{87500, 98000, 104800}));
    CHECK(seekFoundFlag == 1);
    CHECK(!progressFrequencies.isEmpty());   // 进度信号按步长抽稀上报
    // 搜台前未静音 -> 强制静音一次 + 结束恢复一次 = 2 次
    CHECK(mock->muteCallCount() == 2);

    HardwareState afterSeek;
    mock->readState(afterSeek);
    CHECK(afterSeek.muted == false);
    // 搜台必须用 Search 模式调谐
    CHECK(mock->lastTuneMode() == IRadioHardware::TuneMode::Search);

    // —— 用例 4：找下一个台（NextStation），从当前台起步不重复命中自己 ——
    qInfo().noquote() << QStringLiteral("[4] 找下一个台");
    drainEvents();
    hitFrequencies.clear();
    seekFinishedCount = 0;
    seekFoundFlag = -1;
    seekFinishedFrequency = -1;

    CHECK(scheduler.submitSeek(fastSeekRequest(SeekMode::NextStation, 87500, 87000, 108000, 100)) != 0);
    CHECK(waitUntil([&] { return seekFinishedCount > 0; }));
    CHECK(hitFrequencies == (QVector<int>{98000}));
    CHECK(seekFoundFlag == 1);
    CHECK(seekFinishedFrequency == 98000);

    // —— 用例 5：搜台可被取消，且立即返回当前频点 ——
    qInfo().noquote() << QStringLiteral("[5] 取消搜台");
    drainEvents();
    hitFrequencies.clear();
    seekStartedCount = 0;
    seekFinishedCount = 0;
    seekFoundFlag = -1;
    seekFinishedFrequency = -1;

    SeekRequest slow = fastSeekRequest(SeekMode::FullBand, 88000, 88000, 108000, 100);
    slow.lockMs = 8;        // 每点约 8ms，整段约 1.6s
    slow.sampleMs = 2;
    slow.sampleCount = 1;

    QElapsedTimer cancelTimer;
    cancelTimer.start();
    CHECK(scheduler.submitSeek(slow) != 0);
    CHECK(waitUntil([&] { return seekStartedCount > 0; }, 3000));
    QThread::msleep(120);
    scheduler.cancelSeek();
    CHECK(waitUntil([&] { return seekFinishedCount > 0; }, 5000));

    const qint64 cancelElapsed = cancelTimer.elapsed();
    // 不取消时约 1.6s，取消后应远早于此结束
    CHECK(cancelElapsed < 1200);
    CHECK(seekFoundFlag == 0);                                        // 120ms 内扫不到台
    CHECK(seekFinishedFrequency >= 88000 && seekFinishedFrequency <= 108000);

    // —— 用例 6：硬件故障向上传递 ——
    qInfo().noquote() << QStringLiteral("[6] 硬件故障传递");
    drainEvents();
    QVector<ErrorCode> codes;
    QMetaObject::Connection codeConnection = QObject::connect(
        &scheduler, &HSRadioOperationScheduler::operationFinished, &app,
        [&](quint64, ErrorCode code) { codes.append(code); });

    mock->setTuneFailure(ErrorCode::AlsaWriteFailed);
    CHECK(scheduler.submitTune(90000, IRadioHardware::TuneMode::Preset) != 0);
    CHECK(waitUntil([&] { return !codes.isEmpty(); }));
    CHECK(codes.first() == ErrorCode::AlsaWriteFailed);
    mock->setTuneFailure(ErrorCode::None);
    QObject::disconnect(codeConnection);

    // —— 收尾 ——
    scheduler.shutdown();
    // 关闭后提交应被拒绝（返回 0），且不崩溃
    CHECK(scheduler.submitReadState() == 0);

    qInfo().noquote() << QString();
    if (g_failures == 0) {
        qInfo().noquote() << QStringLiteral("全部通过：%1 项检查").arg(g_checks);
    } else {
        qCritical().noquote()
            << QStringLiteral("失败 %1 / 共 %2 项检查").arg(g_failures).arg(g_checks);
    }

    return g_failures == 0 ? 0 : 1;
}
