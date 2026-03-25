#include "dxvk_device.h"
#include "dxvk_graphics.h"
#include "dxvk_pipecompiler.h"

namespace dxvk {

  constexpr uint32_t get_dyasync_min_workers()       { return 1;  }
  constexpr uint32_t get_dyasync_max_workers()       { return 32; }
  constexpr uint32_t get_dyasync_max_workers_32bit() { return 16; }

  uint32_t calculate_base_worker_count(uint32_t cpu_cores) {
    return ((std::max(1u, cpu_cores) - 1) * 5) / 7;
  }

  uint32_t clamp_worker_count(uint32_t count) {
    return std::clamp(count, get_dyasync_min_workers(), get_dyasync_max_workers());
  }

  uint32_t apply_platform_cap(uint32_t count, bool is_32bit) {
    if (!is_32bit)
      return count;
    return std::min(count, get_dyasync_max_workers_32bit());
  }

  uint32_t calculate_worker_count(
      uint32_t cpu_cores,
      int32_t  config_thread_count,
      bool     is_32bit,
      bool     use_all_cores) {
    if (use_all_cores)
      return cpu_cores;
    if (config_thread_count > 0)
      return static_cast<uint32_t>(config_thread_count);
    return apply_platform_cap(
      clamp_worker_count(
        calculate_base_worker_count(cpu_cores)),
      is_32bit);
  }

  DxvkPipelineCompiler::DxvkPipelineCompiler(const DxvkDevice* device) {
    uint32_t numWorkers = calculate_worker_count(
      dxvk::thread::hardware_concurrency(),
      device->config().numDyasyncThreads,
      env::is32BitHostPlatform(),
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
