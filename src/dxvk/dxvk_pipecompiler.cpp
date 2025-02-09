#include "dxvk_device.h"
#include "dxvk_graphics.h"
#include "dxvk_pipecompiler.h"

namespace dxvk {

  DxvkPipelineCompiler::DxvkPipelineCompiler(const DxvkDevice* device) {
    // Determine the number of worker threads
    uint32_t numCpuCores = dxvk::thread::hardware_concurrency();
    uint32_t numWorkers  = ((std::max(1u, numCpuCores) - 1) * 5) / 7;

    // Clamp the thread count between 1 and 32
    if (numWorkers < 1)
      numWorkers = 1;
    if (numWorkers > 32)
      numWorkers = 32;

    // Override if the device configuration specifies a different thread count.
    if (device->config().numAsyncThreads > 0)
      numWorkers = device->config().numAsyncThreads;

    Logger::info(str::format("DXVK: Using ", numWorkers, " async compiler threads"));

    // Reserve and create the worker threads
    m_compilerThreads.reserve(numWorkers);
    for (uint32_t i = 0; i < numWorkers; i++) {
      m_compilerThreads.emplace_back([this] { this->runCompilerThread(); });
    }
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
    DxvkGraphicsPipeline*                   pipeline,
    const DxvkGraphicsPipelineStateInfo&    state,
    const DxvkRenderPass*                   renderPass) {
    {
      std::lock_guard<std::mutex> lock(m_compilerLock);
      // Emplace a new task to avoid an extra copy
      m_compilerQueue.emplace(PipelineEntry{ pipeline, state, renderPass });
    }
    // Only one thread needs to be notified
    m_compilerCond.notify_one();
  }

  void DxvkPipelineCompiler::runCompilerThread() {
    env::setThreadName("dxvk-pcompiler");

    while (true) {
      // Batch drain tasks to minimize the time spent holding the lock.
      std::vector<PipelineEntry> tasks;

      {
        std::unique_lock<std::mutex> lock(m_compilerLock);
        m_compilerCond.wait(lock, [this] {
          return m_compilerStop.load() || !m_compilerQueue.empty();
        });

        // If the stop flag is set and there are no remaining tasks, exit.
        if (m_compilerStop.load() && m_compilerQueue.empty())
          break;

        // Drain the queue to process multiple tasks without reacquiring the lock.
        while (!m_compilerQueue.empty()) {
          tasks.push_back(std::move(m_compilerQueue.front()));
          m_compilerQueue.pop();
        }
      }

      // Process each task outside the lock.
      for (auto& entry : tasks) {
        if (entry.pipeline != nullptr && entry.renderPass != nullptr) {
          if (entry.pipeline->compilePipeline(entry.state, entry.renderPass)) {
            entry.pipeline->writePipelineStateToCache(entry.state, entry.renderPass->format());
          }
        }
      }
    }
  }

} // namespace dxvk
