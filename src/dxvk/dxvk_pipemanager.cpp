#include "dxvk_device.h"
#include "dxvk_pipemanager.h"
#include "dxvk_state_cache.h"

namespace dxvk {

  namespace {

    bool is_dyasync_enabled(const DxvkDevice* device) {
      return env::getEnvVar("DXVK_DISABLE_DYASYNC") != "1"
          && device->config().enableDyasync;
    }

    bool is_state_cache_enabled(const DxvkDevice* device) {
      return env::getEnvVar("DXVK_STATE_CACHE") != "0"
          && device->config().enableStateCache;
    }

  }


  DxvkPipelineManager::DxvkPipelineManager(
          DxvkDevice*         device,
          DxvkRenderPassPool* passManager)
    : m_device    (device)
    , m_cache     (new DxvkPipelineCache(device->vkd()))
    , m_stateCache(is_state_cache_enabled(device)
          ? new DxvkStateCache(device, this, passManager)
          : nullptr)
    , m_compiler  (is_dyasync_enabled(device)
          ? new DxvkPipelineCompiler(device)
          : nullptr) {

  }


  DxvkPipelineManager::~DxvkPipelineManager() = default;


  DxvkComputePipeline* DxvkPipelineManager::createComputePipeline(
          const DxvkComputePipelineShaders& shaders) {
    return shaders.cs == nullptr
      ? nullptr
      : findOrCreateComputePipeline(shaders);
  }


  DxvkComputePipeline* DxvkPipelineManager::findOrCreateComputePipeline(
          const DxvkComputePipelineShaders& shaders) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    return &m_computePipelines.try_emplace(shaders, this, shaders).first->second;
  }


  DxvkGraphicsPipeline* DxvkPipelineManager::createGraphicsPipeline(
          const DxvkGraphicsPipelineShaders& shaders) {
    return shaders.vs == nullptr
      ? nullptr
      : findOrCreateGraphicsPipeline(shaders);
  }


  DxvkGraphicsPipeline* DxvkPipelineManager::findOrCreateGraphicsPipeline(
          const DxvkGraphicsPipelineShaders& shaders) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    return &m_graphicsPipelines.try_emplace(shaders, this, shaders).first->second;
  }


  void DxvkPipelineManager::registerShader(
          const Rc<DxvkShader>& shader) {
    switch (m_stateCache != nullptr) {
      case true:  m_stateCache->registerShader(shader); break;
      case false: break;
    }
  }


  DxvkPipelineCount DxvkPipelineManager::getPipelineCount() const {
    return DxvkPipelineCount {
      .numGraphicsPipelines = m_numGraphicsPipelines.load(),
      .numComputePipelines  = m_numComputePipelines.load(),
    };
  }


  bool DxvkPipelineManager::isCompilingShaders() const {
    return m_stateCache != nullptr
        && m_stateCache->isCompilingShaders();
  }


  void DxvkPipelineManager::stopWorkerThreads() const {
    switch (m_stateCache != nullptr) {
      case true:  m_stateCache->stopWorkerThreads(); break;
      case false: break;
    }
  }

}
