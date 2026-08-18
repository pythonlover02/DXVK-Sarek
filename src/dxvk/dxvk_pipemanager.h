#pragma once

#include <mutex>
#include <unordered_map>

#include "dxvk_compute.h"
#include "dxvk_graphics.h"

namespace dxvk {

  // Forward declared rather than included: the compiler headers pull in
  // dxvk_graphics.h and dxvk_include.h, which reach back here through
  // dxvk_objects.h. Rc members only need a declaration, and every use of
  // these types is in a translation unit that includes their header.
  class DxvkAsyncCompiler;
  class DxvkPipelineCompiler;
  class DxvkStateCache;

  /**
   * \brief Shader compilation method
   *
   * Decides what happens when a draw needs a pipeline that has not been
   * compiled yet.
   *
   * \li \c None compiles it there and then, stalling the draw. This is
   *     unpatched 1.10.x behaviour and the reference point for anything
   *     that looks like a rendering bug.
   * \li \c Dyasync substitutes the closest already-compiled variant of
   *     the same shaders and compiles the correct one in the background,
   *     so something valid is always on screen.
   * \li \c Async draws nothing for that pipeline until it is ready.
   */
  enum class DxvkShaderCompileMethod : uint32_t {
    None    = 0,
    Dyasync = 1,
    Async   = 2,
  };

  /**
   * \brief Pipeline count
   *
   * Stores number of graphics and
   * compute pipelines, individually.
   */
  struct DxvkPipelineCount {
    uint32_t numGraphicsPipelines;
    uint32_t numComputePipelines;
  };

  /**
   * \brief Pipeline manager
   *
   * Creates and stores graphics pipelines and compute
   * pipelines for each combination of shaders that is
   * used within the application. This is necessary
   * because DXVK does not expose the concept of shader
   * pipeline objects to the client API.
   */
  class DxvkPipelineManager {
    friend class DxvkComputePipeline;
    friend class DxvkGraphicsPipeline;
  public:

    DxvkPipelineManager(
            DxvkDevice*         device,
            DxvkRenderPassPool* passManager);
    ~DxvkPipelineManager();

    /**
     * \brief Retrieves a compute pipeline object
     *
     * If a pipeline for the given shader stage object
     * already exists, it will be returned. Otherwise,
     * a new pipeline will be created.
     * \param [in] shaders Shaders for the pipeline
     * \returns Compute pipeline object
     */
    DxvkComputePipeline* createComputePipeline(
      const DxvkComputePipelineShaders& shaders);

    /**
     * \brief Retrieves a graphics pipeline object
     *
     * If a pipeline for the given shader stage objects
     * already exists, it will be returned. Otherwise,
     * a new pipeline will be created.
     * \param [in] shaders Shaders for the pipeline
     * \returns Graphics pipeline object
     */
    DxvkGraphicsPipeline* createGraphicsPipeline(
      const DxvkGraphicsPipelineShaders& shaders);

    /*
     * \brief Registers a shader
     *
     * Starts compiling pipelines asynchronously
     * in case the state cache contains state
     * vectors for this shader.
     * \param [in] shader Newly compiled shader
     */
    void registerShader(
      const Rc<DxvkShader>& shader);

    /**
     * \brief Retrieves total pipeline count
     * \returns Number of compute/graphics pipelines
     */
    DxvkPipelineCount getPipelineCount() const;

    /**
     * \brief Checks whether the compiler is busy
     * \returns \c true if shaders are being compiled
     */
    bool isCompilingShaders() const;

    /**
     * \brief Stops background compiler threads
     */
    void stopWorkerThreads() const;

    /**
     * \brief Queries the active shader compilation method
     * \returns The method resolved at construction
     */
    DxvkShaderCompileMethod compileMethod() const {
      return m_compileMethod;
    }

  private:

    DxvkDevice*               m_device = nullptr;
    Rc<DxvkPipelineCache>     m_cache;
    Rc<DxvkStateCache>        m_stateCache;

    DxvkShaderCompileMethod   m_compileMethod = DxvkShaderCompileMethod::None;

    Rc<DxvkPipelineCompiler>  m_dyasync;
    Rc<DxvkAsyncCompiler>     m_async;

    std::atomic<uint32_t> m_numComputePipelines  = { 0 };
    std::atomic<uint32_t> m_numGraphicsPipelines = { 0 };

    dxvk::mutex m_mutex;

    std::unordered_map<
      DxvkComputePipelineShaders,
      DxvkComputePipeline,
      DxvkHash, DxvkEq> m_computePipelines;

    std::unordered_map<
      DxvkGraphicsPipelineShaders,
      DxvkGraphicsPipeline,
      DxvkHash, DxvkEq> m_graphicsPipelines;

    DxvkComputePipeline* findOrCreateComputePipeline(
      const DxvkComputePipelineShaders& shaders);

    DxvkGraphicsPipeline* findOrCreateGraphicsPipeline(
      const DxvkGraphicsPipelineShaders& shaders);

  };

}
