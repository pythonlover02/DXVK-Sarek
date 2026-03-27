#include "dxvk_device.h"
#include "dxvk_graphics.h"
#include "dxvk_pipecompiler.h"

namespace dxvk {

  static uint32_t computeDyasyncWorkerCount(
    uint32_t cpuCores,
    int32_t  configCount,
    bool     useAllCores) {
    if (useAllCores)
      return cpuCores;

    if (configCount > 0)
      return uint32_t(configCount);

    uint32_t count = ((std::max(1u, cpuCores) - 1) * 5) / 7;
    count = std::clamp(count, 1u, 32u);

    if (env::is32BitHostPlatform())
      count = std::min(count, 16u);

    return count;
  }


  DxvkPipelineCompiler::DxvkPipelineCompiler(const DxvkDevice* device) {
    uint32_t numWorkers = computeDyasyncWorkerCount(
      dxvk::thread::hardware_concurrency(),
      device->config().numDyasyncThreads,
      env::getEnvVar("DXVK_ALL_CORES") == "1");

    Logger::info(str::format("DXVK: Using ", numWorkers, " dyasync compiler threads"));

    m_compilerThreads.reserve(numWorkers);
    for (uint32_t i = 0; i < numWorkers; i++)
      m_compilerThreads.emplace_back([this] { this->runCompilerThread(); });
  }


  DxvkPipelineCompiler::~DxvkPipelineCompiler() {
    m_compilerStop.store(true, std::memory_order_release);
    m_sleepCond.notify_all();

    for (auto& thread : m_compilerThreads)
      thread.join();
  }


  bool DxvkPipelineCompiler::queueCompilation(
    DxvkGraphicsPipeline*                pipeline,
    const DxvkGraphicsPipelineStateInfo& state,
    DxvkDyasyncPriority                  priority) {
    DxvkDyasyncEntry entry;
    entry.pipeline = pipeline;
    entry.state    = state;
    entry.priority = priority;

    bool pushed;
    if (priority == DxvkDyasyncPriority::Background)
      pushed = m_backgroundRing.tryPush(std::move(entry));
    else
      pushed = m_liveRing.tryPush(std::move(entry));

    if (pushed)
      m_sleepCond.notify_one();

    return pushed;
  }


  void DxvkPipelineCompiler::runCompilerThread() {
    env::setThreadName("dxvk-dyasync");

    while (true) {
      DxvkDyasyncEntry entry;

      uint32_t pick = m_pickCounter.fetch_add(1, std::memory_order_relaxed);
      bool preferBg = ((pick & 7) == 0);

      bool found;
      if (preferBg)
        found = m_backgroundRing.tryPop(entry) || m_liveRing.tryPop(entry);
      else
        found = m_liveRing.tryPop(entry) || m_backgroundRing.tryPop(entry);

      if (found) {
        if (entry.pipeline != nullptr) {
          entry.pipeline->compilePipeline(entry.state);
          // releasePipeline is always required to balance the acquire
          // performed in getPipelineHandle regardless of whether
          // compilePipeline did any work
          entry.pipeline->releasePipeline();
        }
        continue;
      }

      if (m_compilerStop.load(std::memory_order_acquire))
        return;

      std::unique_lock<std::mutex> lock(m_sleepMutex);
      m_sleepCond.wait(lock, [this] {
        return m_compilerStop.load(std::memory_order_acquire)
            || !m_liveRing.empty()
            || !m_backgroundRing.empty();
      });

      if (m_compilerStop.load(std::memory_order_acquire)
       && m_liveRing.empty()
       && m_backgroundRing.empty())
        return;
    }
  }

}
