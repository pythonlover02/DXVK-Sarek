#include "dxvk_device.h"
#include "dxvk_graphics.h"
#include "dxvk_pipecompiler.h"

namespace dxvk {

  constexpr uint32_t dyasync_min_workers()       { return 1;  }
  constexpr uint32_t dyasync_max_workers()       { return 32; }
  constexpr uint32_t dyasync_max_workers_32bit() { return 16; }

  DxvkPipelineCompiler::DxvkPipelineCompiler(const DxvkDevice* device) {
    uint32_t numCpuCores = dxvk::thread::hardware_concurrency();
    uint32_t numWorkers  = ((std::max(1u, numCpuCores) - 1) * 5) / 7;

    if (numWorkers < dyasync_min_workers()) numWorkers = dyasync_min_workers();
    if (numWorkers > dyasync_max_workers()) numWorkers = dyasync_max_workers();

    if (env::is32BitHostPlatform())
      numWorkers = std::min(numWorkers, dyasync_max_workers_32bit());

    if (device->config().numDyasyncThreads > 0)
      numWorkers = static_cast<uint32_t>(device->config().numDyasyncThreads);

    if (env::getEnvVar("DXVK_ALL_CORES") == "1")
      numWorkers = numCpuCores;

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
    const DxvkRenderPass*                renderPass,
    DxvkPipelinePriority                 priority) {
    DxvkPipelineEntry entry{ pipeline, state, renderPass, priority };

    bool pushed;
    if (priority == DxvkPipelinePriority::Background)
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
      DxvkPipelineEntry entry;

      uint32_t pick = m_pickCounter.fetch_add(1, std::memory_order_relaxed);
      bool preferBg = ((pick & 7) == 0);

      bool found;
      if (preferBg)
        found = m_backgroundRing.tryPop(entry) || m_liveRing.tryPop(entry);
      else
        found = m_liveRing.tryPop(entry) || m_backgroundRing.tryPop(entry);

      if (found) {
        if (entry.pipeline != nullptr && entry.renderPass != nullptr) {
          if (entry.pipeline->compilePipeline(entry.state, entry.renderPass))
            entry.pipeline->writePipelineStateToCache(entry.state, entry.renderPass->format());
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
