#include "dxvk_device.h"
#include "dxvk_pipemanager.h"
#include "dxvk_state_cache.h"

namespace dxvk {

  DxvkPipelineManager::DxvkPipelineManager(
          DxvkDevice*         device,
          DxvkRenderPassPool* passManager)
  : m_device    (device),
    m_cache     (new DxvkPipelineCache(device->vkd())) {

    std::string useStateCache   = env::getEnvVar("DXVK_STATE_CACHE");
    std::string disableDyasync  = env::getEnvVar("DXVK_DISABLE_DYASYNC");
    std::string disableAsync    = env::getEnvVar("DXVK_DISABLE_ASYNC");

    bool forceDisable = disableDyasync == "1" || disableAsync == "1";

    if (!forceDisable) {
      std::string useAsync   = env::getEnvVar("DXVK_ASYNC");
      std::string useDyasync = env::getEnvVar("DXVK_DYASYNC");

      if (useAsync == "1" || useDyasync == "1" || device->config().enableDyasync)
        m_compiler = new DxvkPipelineCompiler(device);
    }

    if (useStateCache != "0" && device->config().enableStateCache)
      m_stateCache = new DxvkStateCache(device, this, passManager);
  }


  DxvkPipelineManager::~DxvkPipelineManager() {

  }


  DxvkComputePipeline* DxvkPipelineManager::createComputePipeline(
    const DxvkComputePipelineShaders& shaders) {
    if (shaders.cs == nullptr)
      return nullptr;

    std::lock_guard<dxvk::mutex> lock(m_mutex);

    auto pair = m_computePipelines.find(shaders);
    if (pair != m_computePipelines.end())
      return &pair->second;

    auto iter = m_computePipelines.emplace(
      std::piecewise_construct,
      std::tuple(shaders),
      std::tuple(this, shaders));
    return &iter.first->second;
  }


  DxvkGraphicsPipeline* DxvkPipelineManager::createGraphicsPipeline(
    const DxvkGraphicsPipelineShaders& shaders) {
    if (shaders.vs == nullptr)
      return nullptr;

    std::lock_guard<dxvk::mutex> lock(m_mutex);

    auto pair = m_graphicsPipelines.find(shaders);
    if (pair != m_graphicsPipelines.end())
      return &pair->second;

    auto iter = m_graphicsPipelines.emplace(
      std::piecewise_construct,
      std::tuple(shaders),
      std::tuple(this, shaders));
    return &iter.first->second;
  }


  void DxvkPipelineManager::registerShader(
    const Rc<DxvkShader>&         shader) {
    if (m_stateCache != nullptr)
      m_stateCache->registerShader(shader);
  }


  DxvkPipelineCount DxvkPipelineManager::getPipelineCount() const {
    DxvkPipelineCount result;
    result.numComputePipelines  = m_numComputePipelines.load();
    result.numGraphicsPipelines = m_numGraphicsPipelines.load();
    return result;
  }


  bool DxvkPipelineManager::isCompilingShaders() const {
    return m_stateCache != nullptr
        && m_stateCache->isCompilingShaders();
  }


  void DxvkPipelineManager::stopWorkerThreads() const {
    if (m_stateCache != nullptr)
      m_stateCache->stopWorkerThreads();
  }

}
