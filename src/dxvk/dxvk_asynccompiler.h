#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <vector>

#include "../util/thread.h"

// The queue entry stores a state vector by value, so a forward
// declaration is not enough. dxvk_graphics_state.h is not self-contained
// (it uses util:: and DxvkBindingMask without including them), so pull
// in dxvk_graphics.h, which brings the prerequisites with it.
#include "dxvk_graphics.h"
#include "dxvk_include.h"

namespace dxvk {

  class DxvkDevice;
  class DxvkRenderPass;

  /**
   * \brief Async pipeline compiler
   *
   * Compiles pipelines on background threads and substitutes nothing in
   * the meantime, so geometry whose pipeline is not ready is not drawn
   * until compilation finishes. This removes compilation stalls entirely
   * at the cost of correctness, and only behaves well where there are
   * spare cores to absorb the backlog.
   */
  class DxvkAsyncCompiler : public RcObject {

  public:

    explicit DxvkAsyncCompiler(const DxvkDevice* device);

    ~DxvkAsyncCompiler();

    /**
     * \brief Compiles a pipeline asynchronously
     *
     * This should be used to compile graphics pipeline instances
     * asynchronously.
     *
     * \param [in] pipeline   The pipeline object
     * \param [in] state      The pipeline state info object
     * \param [in] renderPass The render pass object
     */
    void queueCompilation(
      DxvkGraphicsPipeline*                pipeline,
      const DxvkGraphicsPipelineStateInfo& state,
      const DxvkRenderPass*                renderPass);

  private:

    struct PipelineEntry {
      DxvkGraphicsPipeline*                pipeline{nullptr};
      DxvkGraphicsPipelineStateInfo        state;
      const DxvkRenderPass*                renderPass{nullptr};
    };

    std::atomic<bool>           m_compilerStop{false};
    std::mutex                  m_compilerLock;
    std::condition_variable     m_compilerCond;
    std::queue<PipelineEntry>   m_compilerQueue;
    std::vector<dxvk::thread>   m_compilerThreads;

    void runCompilerThread();

  };

}
