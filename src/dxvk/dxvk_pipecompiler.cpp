#include <algorithm>
#include <utility>

#include "dxvk_device.h"
#include "dxvk_graphics.h"
#include "dxvk_pipecompiler.h"

namespace dxvk {

  namespace {

    constexpr uint32_t DYASYNC_MIN_WORKERS         = 1;
    constexpr uint32_t DYASYNC_MAX_WORKERS         = 32;
    constexpr uint32_t DYASYNC_MAX_WORKERS_32BIT   = 16;
    constexpr uint32_t DYASYNC_QUEUE_CAPACITY      = 4096;
    constexpr uint32_t DYASYNC_BACKGROUND_INTERVAL = 8;

    enum class TakeLane : uint32_t {
      None       = 0,
      Live       = 1,
      Background = 2,
    };

    uint32_t base_worker_count(uint32_t cpu_cores) {
      return ((std::max(1u, cpu_cores) - 1u) * 5u) / 7u;
    }

    uint32_t clamp_worker_count(uint32_t count) {
      return std::clamp(count, DYASYNC_MIN_WORKERS, DYASYNC_MAX_WORKERS);
    }

    uint32_t apply_platform_cap(uint32_t count, bool is_32bit) {
      return is_32bit ? std::min(count, DYASYNC_MAX_WORKERS_32BIT) : count;
    }

    uint32_t resolve_worker_count(
            uint32_t cpu_cores,
            int32_t  config_thread_count,
            bool     is_32bit,
            bool     use_all_cores) {
      return use_all_cores           ? cpu_cores
           : config_thread_count > 0 ? uint32_t(config_thread_count)
           :                           apply_platform_cap(
                                         clamp_worker_count(base_worker_count(cpu_cores)),
                                         is_32bit);
    }

    bool prefer_background(uint32_t pick) {
      return (pick % DYASYNC_BACKGROUND_INTERVAL) == 0;
    }

    bool entry_is_valid(const DxvkPipelineEntry& entry) {
      return entry.pipeline   != nullptr
          && entry.renderPass != nullptr;
    }

    bool work_available(
            bool                           stop,
      const std::deque<DxvkPipelineEntry>& liveQueue,
      const std::deque<DxvkPipelineEntry>& backgroundQueue) {
      return stop
          || !liveQueue.empty()
          || !backgroundQueue.empty();
    }

    TakeLane select_lane(bool live_empty, bool bg_empty, bool prefer_bg) {
      return (prefer_bg && !bg_empty) ? TakeLane::Background
           : (!live_empty)            ? TakeLane::Live
           : (!bg_empty)              ? TakeLane::Background
           :                            TakeLane::None;
    }

    DxvkPipelineEntry pop_front(std::deque<DxvkPipelineEntry>& queue) {
      DxvkPipelineEntry entry = std::move(queue.front());
      queue.pop_front();
      return entry;
    }

  }


  DxvkPipelineCompiler::DxvkPipelineCompiler(const DxvkDevice* device) {
    uint32_t numWorkers = resolve_worker_count(
      dxvk::thread::hardware_concurrency(),
      device->config().numDyasyncThreads,
      env::is32BitHostPlatform(),
      env::getEnvVar("DXVK_ALL_CORES") == "1");

    Logger::info(str::format("DXVK: Using ", numWorkers, " dyasync compiler threads"));

    this->spawnWorkers(numWorkers);
  }


  DxvkPipelineCompiler::~DxvkPipelineCompiler() {
    this->requestStop();
    this->joinAll();
  }


  bool DxvkPipelineCompiler::queueCompilation(
          DxvkGraphicsPipeline*          pipeline,
    const DxvkGraphicsPipelineStateInfo& state,
    const DxvkRenderPass*                renderPass,
          DxvkPipelinePriority           priority) {
    bool pushed = this->pushEntry(DxvkPipelineEntry {
      .pipeline   = pipeline,
      .state      = state,
      .renderPass = renderPass,
      .priority   = priority,
    });

    this->notifyIfPushed(pushed);
    return pushed;
  }


  void DxvkPipelineCompiler::spawnWorkers(uint32_t count) {
    m_workers.reserve(count);

    for (uint32_t i = 0; i < count; i++)
      m_workers.emplace_back([this] { this->runCompilerThread(); });
  }


  void DxvkPipelineCompiler::requestStop() {
    { std::lock_guard<std::mutex> lock(m_mutex);
      m_stop = true; }

    m_cond.notify_all();
  }


  void DxvkPipelineCompiler::joinAll() {
    for (dxvk::thread& worker : m_workers)
      worker.join();
  }


  std::deque<DxvkPipelineEntry>& DxvkPipelineCompiler::laneFor(DxvkPipelinePriority priority) {
    return priority == DxvkPipelinePriority::Background
      ? m_backgroundQueue
      : m_liveQueue;
  }


  bool DxvkPipelineCompiler::pushEntry(DxvkPipelineEntry&& entry) {
    std::deque<DxvkPipelineEntry>& queue = this->laneFor(entry.priority);

    std::lock_guard<std::mutex> lock(m_mutex);

    switch (queue.size() >= DYASYNC_QUEUE_CAPACITY) {
      case true:  return false;
      case false: queue.push_back(std::move(entry)); return true;
    }

    return false;
  }


  void DxvkPipelineCompiler::notifyIfPushed(bool pushed) {
    switch (pushed) {
      case true:  m_cond.notify_one(); break;
      case false: break;
    }
  }


  bool DxvkPipelineCompiler::takeEntry(DxvkPipelineEntry& entry, bool preferBackground) {
    switch (select_lane(m_liveQueue.empty(), m_backgroundQueue.empty(), preferBackground)) {
      case TakeLane::Live:       entry = pop_front(m_liveQueue);       return true;
      case TakeLane::Background: entry = pop_front(m_backgroundQueue); return true;
      case TakeLane::None:       return false;
    }

    return false;
  }


  bool DxvkPipelineCompiler::waitForWork(DxvkPipelineEntry& entry) {
    uint32_t pick = m_pickCounter.fetch_add(1, std::memory_order_relaxed);

    std::unique_lock<std::mutex> lock(m_mutex);

    m_cond.wait(lock, [this] {
      return work_available(m_stop, m_liveQueue, m_backgroundQueue);
    });

    return this->takeEntry(entry, prefer_background(pick));
  }


  void DxvkPipelineCompiler::compileAndCache(const DxvkPipelineEntry& entry) {
    switch (entry.pipeline->compilePipeline(entry.state, entry.renderPass)) {
      case true:  entry.pipeline->writePipelineStateToCache(entry.state, entry.renderPass->format()); break;
      case false: break;
    }
  }


  void DxvkPipelineCompiler::processEntry(const DxvkPipelineEntry& entry) {
    switch (entry_is_valid(entry)) {
      case true:  this->compileAndCache(entry); break;
      case false: break;
    }
  }


  void DxvkPipelineCompiler::runCompilerThread() {
    env::setThreadName("dxvk-dyasync");

    for (DxvkPipelineEntry entry; this->waitForWork(entry); )
      this->processEntry(entry);
  }

}
