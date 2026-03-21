#pragma once

#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <vector>

#include "../util/thread.h"
#include "../util/util_time.h"
#include "dxvk_include.h"

namespace dxvk {

  class DxvkDevice;
  class DxvkGraphicsPipeline;
  class DxvkGraphicsPipelineStateInfo;
  class DxvkRenderPass;

  enum class DxvkPipelinePriority : uint32_t {
    Low      = 0,
    Normal   = 1,
    High     = 2,
    Critical = 3
  };

  struct DxvkPipelineEntry {
    DxvkGraphicsPipeline*         pipeline;
    DxvkGraphicsPipelineStateInfo state;
    const DxvkRenderPass*         renderPass;
    DxvkPipelinePriority          priority;
  };

  // Asynchronous pipeline compiler
  class DxvkPipelineCompiler : public RcObject {

  public:

    explicit DxvkPipelineCompiler(const DxvkDevice* device);
    ~DxvkPipelineCompiler();

    // Queues a pipeline for async compilation
    void queueCompilation(
      DxvkGraphicsPipeline*                pipeline,
      const DxvkGraphicsPipelineStateInfo& state,
      const DxvkRenderPass*                renderPass,
      DxvkPipelinePriority                 priority);

    uint64_t averageCompileTimeUs() const;
    uint32_t pendingCount() const;

  private:

    std::atomic<bool>               m_compilerStop{false};
    std::mutex                      m_compilerLock;
    std::condition_variable         m_compilerCond;
    std::vector<DxvkPipelineEntry>  m_compilerQueue;
    std::vector<dxvk::thread>       m_compilerThreads;

    std::atomic<uint64_t>           m_totalCompileTimeUs{0};
    std::atomic<uint32_t>           m_totalCompileCount{0};
    std::atomic<uint32_t>           m_pendingCount{0};

    void runCompilerThread();

  };

}
