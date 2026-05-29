#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <vector>

#include "../util/thread.h"

#include "dxvk_include.h"

namespace dxvk {

  class DxvkDevice;
  class DxvkGraphicsPipeline;
  struct DxvkGraphicsPipelineStateInfo;
  class DxvkRenderPass;

  enum class DxvkPipelinePriority : uint32_t {
    Background = 0,
    Live       = 1,
  };

  struct DxvkPipelineEntry {
    DxvkGraphicsPipeline*         pipeline;
    DxvkGraphicsPipelineStateInfo state;
    const DxvkRenderPass*         renderPass;
    DxvkPipelinePriority          priority;
  };

  class DxvkPipelineCompiler : public RcObject {

  public:

    explicit DxvkPipelineCompiler(const DxvkDevice* device);

    ~DxvkPipelineCompiler();

    bool queueCompilation(
            DxvkGraphicsPipeline*          pipeline,
      const DxvkGraphicsPipelineStateInfo& state,
      const DxvkRenderPass*                renderPass,
            DxvkPipelinePriority           priority);

  private:

    std::mutex                    m_mutex;
    std::condition_variable       m_cond;
    bool                          m_stop = false;

    std::deque<DxvkPipelineEntry> m_liveQueue;
    std::deque<DxvkPipelineEntry> m_backgroundQueue;

    std::atomic<uint32_t>         m_pickCounter = { 0 };

    std::vector<dxvk::thread>     m_workers;

    void spawnWorkers(uint32_t count);

    void requestStop();

    void joinAll();

    std::deque<DxvkPipelineEntry>& laneFor(DxvkPipelinePriority priority);

    bool pushEntry(DxvkPipelineEntry&& entry);

    void notifyIfPushed(bool pushed);

    bool takeEntry(DxvkPipelineEntry& entry, bool preferBackground);

    bool waitForWork(DxvkPipelineEntry& entry);

    void compileAndCache(const DxvkPipelineEntry& entry);

    void processEntry(const DxvkPipelineEntry& entry);

    void runCompilerThread();

  };

}
