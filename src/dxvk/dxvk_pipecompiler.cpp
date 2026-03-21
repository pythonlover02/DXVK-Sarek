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

    // save address space on 32-bit
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
    {
      std::lock_guard<std::mutex> lock(m_compilerLock);
      m_compilerStop.store(true);
    }

    m_compilerCond.notify_all();

    for (auto& thread : m_compilerThreads)
      thread.join();
  }


  void DxvkPipelineCompiler::queueCompilation(
    DxvkGraphicsPipeline*                pipeline,
    const DxvkGraphicsPipelineStateInfo& state,
    const DxvkRenderPass*                renderPass,
    DxvkPipelinePriority                 priority) {
    std::lock_guard<std::mutex> lock(m_compilerLock);

    // skip if already queued for this pipeline + render pass + state
    for (const auto& entry : m_compilerQueue) {
      if (entry.pipeline   == pipeline
       && entry.renderPass == renderPass
       && entry.state      == state)
        return;
    }

    m_pendingCount.fetch_add(1, std::memory_order_relaxed);
    m_compilerQueue.push_back(DxvkPipelineEntry{ pipeline, state, renderPass, priority });
    m_compilerCond.notify_one();
  }


  uint64_t DxvkPipelineCompiler::averageCompileTimeUs() const {
    uint32_t count = m_totalCompileCount.load(std::memory_order_relaxed);
    if (count == 0)
      return 0;
    return m_totalCompileTimeUs.load(std::memory_order_relaxed) / count;
  }


  uint32_t DxvkPipelineCompiler::pendingCount() const {
    return m_pendingCount.load(std::memory_order_relaxed);
  }


  void DxvkPipelineCompiler::runCompilerThread() {
    env::setThreadName("dxvk-dyasync");

    while (true) {
      DxvkPipelineEntry entry;

      {
        std::unique_lock<std::mutex> lock(m_compilerLock);
        m_compilerCond.wait(lock, [this] {
          return m_compilerStop.load() || !m_compilerQueue.empty();
        });

        if (m_compilerStop.load() && m_compilerQueue.empty())
          return;

        // pop highest priority entry, O(1) swap-with-back
        auto best = m_compilerQueue.begin();
        for (auto it = best + 1; it != m_compilerQueue.end(); ++it) {
          if (static_cast<uint32_t>(it->priority) > static_cast<uint32_t>(best->priority))
            best = it;
        }

        entry = std::move(*best);
        if (best != m_compilerQueue.end() - 1)
          *best = std::move(m_compilerQueue.back());
        m_compilerQueue.pop_back();
      }

      if (entry.pipeline == nullptr || entry.renderPass == nullptr) {
        m_pendingCount.fetch_sub(1, std::memory_order_relaxed);
        continue;
      }

      auto t0 = dxvk::high_resolution_clock::now();

      if (entry.pipeline->compilePipeline(entry.state, entry.renderPass))
        entry.pipeline->writePipelineStateToCache(entry.state, entry.renderPass->format());

      auto t1 = dxvk::high_resolution_clock::now();
      m_totalCompileTimeUs.fetch_add(
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count(),
        std::memory_order_relaxed);
      m_totalCompileCount.fetch_add(1, std::memory_order_relaxed);
      m_pendingCount.fetch_sub(1, std::memory_order_relaxed);
    }
  }

}
