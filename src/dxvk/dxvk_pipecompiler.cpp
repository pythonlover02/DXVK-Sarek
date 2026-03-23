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

    m_liveQueue.reserve(256);
    m_backgroundQueue.reserve(512);
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

    auto& queue = (priority == DxvkPipelinePriority::Low)
      ? m_backgroundQueue : m_liveQueue;
    m_pendingCount.fetch_add(1, std::memory_order_relaxed);
    queue.push_back(DxvkPipelineEntry{ pipeline, state, renderPass, priority });
    m_compilerCond.notify_one();
  }


  uint64_t DxvkPipelineCompiler::averageCompileTimeUs() const {
    return 0;
  }


  void DxvkPipelineCompiler::queueBatchCompilation(
    const std::vector<DxvkPipelineEntry>& entries) {
    std::lock_guard<std::mutex> lock(m_compilerLock);

    m_pendingCount.fetch_add(uint32_t(entries.size()), std::memory_order_relaxed);
    m_backgroundQueue.insert(m_backgroundQueue.end(), entries.begin(), entries.end());
    m_compilerCond.notify_all();
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
          return m_compilerStop.load()
            || !m_liveQueue.empty()
            || !m_backgroundQueue.empty();
        });

        if (m_compilerStop.load()
         && m_liveQueue.empty()
         && m_backgroundQueue.empty())
          return;

        bool found = false;

        if (!m_liveQueue.empty()) {
          uint32_t size = uint32_t(m_liveQueue.size());
          uint32_t scan = std::min(size, uint32_t(4));
          for (uint32_t i = 0; i < scan; i++) {
            uint32_t idx = size - 1 - i;
            if (!m_liveQueue[idx].pipeline->isCompiling()) {
              entry = std::move(m_liveQueue[idx]);
              if (idx != size - 1)
                m_liveQueue[idx] = std::move(m_liveQueue.back());
              m_liveQueue.pop_back();
              found = true;
              break;
            }
          }
          if (!found) {
            entry = std::move(m_liveQueue.back());
            m_liveQueue.pop_back();
            found = true;
          }
        }

        if (!found && !m_backgroundQueue.empty()) {
          uint32_t size = uint32_t(m_backgroundQueue.size());
          uint32_t scan = std::min(size, uint32_t(4));
          for (uint32_t i = 0; i < scan; i++) {
            uint32_t idx = size - 1 - i;
            if (!m_backgroundQueue[idx].pipeline->isCompiling()) {
              entry = std::move(m_backgroundQueue[idx]);
              if (idx != size - 1)
                m_backgroundQueue[idx] = std::move(m_backgroundQueue.back());
              m_backgroundQueue.pop_back();
              found = true;
              break;
            }
          }
          if (!found) {
            entry = std::move(m_backgroundQueue.back());
            m_backgroundQueue.pop_back();
            found = true;
          }
        }

        if (!found)
          continue;
      }

      if (entry.pipeline == nullptr || entry.renderPass == nullptr) {
        m_pendingCount.fetch_sub(1, std::memory_order_relaxed);
        continue;
      }

      if (entry.pipeline->compilePipeline(entry.state, entry.renderPass))
        entry.pipeline->writePipelineStateToCache(entry.state, entry.renderPass->format());

      m_pendingCount.fetch_sub(1, std::memory_order_relaxed);
    }
  }

}
