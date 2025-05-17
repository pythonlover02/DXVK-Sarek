#include "dxvk_device.h"
#include "dxvk_graphics.h"
#include "dxvk_pipecompiler.h"

namespace dxvk {

  DxvkPipelineCompiler::DxvkPipelineCompiler(const DxvkDevice* device) {
    std::string useAllCores = env::getEnvVar("DXVK_ALL_CORES");
    uint32_t numCpuCores = dxvk::thread::hardware_concurrency();
    uint32_t numWorkers = ((std::max(1u, numCpuCores) - 1) * 5) / 7;
    
    if (numWorkers <  1) numWorkers =  1;
    if (numWorkers > 32) numWorkers = 32;
    
    // Reduce worker count on 32-bit to save adderss space
    if (env::is32BitHostPlatform())
      numWorkers = std::min(numWorkers, 16u);
    
    if (device->config().numAsyncThreads > 0)
      numWorkers = device->config().numAsyncThreads;

    if (useAllCores == "1")
      numWorkers = numCpuCores;

    Logger::info(str::format("DXVK: Using ", numWorkers, " async compiler threads"));

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
    const DxvkRenderPass*                renderPass) {
    {
      std::lock_guard<std::mutex> lock(m_compilerLock);
      m_compilerQueue.emplace(PipelineEntry{ pipeline, state, renderPass });
    }
    m_compilerCond.notify_one();
  }

  void DxvkPipelineCompiler::runCompilerThread() {
    env::setThreadName("dxvk-pcompiler");

    while (true) {
      std::vector<PipelineEntry> tasks;

      {
        std::unique_lock<std::mutex> lock(m_compilerLock);
        m_compilerCond.wait(lock, [this] {
          return m_compilerStop.load() || !m_compilerQueue.empty();
        });

        if (m_compilerStop.load() && m_compilerQueue.empty())
          break;

        while (!m_compilerQueue.empty()) {
          tasks.push_back(std::move(m_compilerQueue.front()));
          m_compilerQueue.pop();
        }
      }

      for (auto& entry : tasks) {
        if (entry.pipeline && entry.renderPass) {
          if (entry.pipeline->compilePipeline(entry.state, entry.renderPass))
            entry.pipeline->writePipelineStateToCache(entry.state, entry.renderPass->format());
        }
      }
    }
  }

} // namespace dxvk
