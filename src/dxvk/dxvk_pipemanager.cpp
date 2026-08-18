#include <string>

#include "dxvk_asynccompiler.h"
#include "dxvk_device.h"
#include "dxvk_pipecompiler.h"
#include "dxvk_pipemanager.h"
#include "dxvk_state_cache.h"

namespace dxvk {

  namespace {

    const char* method_name(DxvkShaderCompileMethod method) {
      switch (method) {
        case DxvkShaderCompileMethod::Dyasync: return "dyasync";
        case DxvkShaderCompileMethod::Async:   return "async";
        case DxvkShaderCompileMethod::None:    return "none";
      }

      return "none";
    }

    DxvkShaderCompileMethod method_from_name(
      const std::string&            name,
            DxvkShaderCompileMethod fallback) {
      if (name == "none")
        return DxvkShaderCompileMethod::None;

      if (name == "dyasync")
        return DxvkShaderCompileMethod::Dyasync;

      if (name == "async")
        return DxvkShaderCompileMethod::Async;

      return fallback;
    }

    // The environment variable wins over the configuration file. A name
    // neither of them recognizes falls through to the next source rather
    // than silently disabling background compilation.
    DxvkShaderCompileMethod resolve_compile_method(const DxvkDevice* device) {
      return method_from_name(
        env::getEnvVar("DXVK_SHADER_COMPILATION_METHOD"),
        method_from_name(
          device->config().shaderCompilationMethod,
          DxvkShaderCompileMethod::Dyasync));
    }

    bool is_state_cache_enabled(const DxvkDevice* device) {
      return env::getEnvVar("DXVK_STATE_CACHE") != "0"
          && device->config().enableStateCache;
    }

  }


  DxvkPipelineManager::DxvkPipelineManager(
          DxvkDevice*         device,
          DxvkRenderPassPool* passManager)
    : m_device       (device)
    , m_cache        (new DxvkPipelineCache(device->vkd()))
    , m_stateCache   (is_state_cache_enabled(device)
          ? new DxvkStateCache(device, this, passManager)
          : nullptr)
    , m_compileMethod(resolve_compile_method(device))
    , m_dyasync      (m_compileMethod == DxvkShaderCompileMethod::Dyasync
          ? new DxvkPipelineCompiler(device)
          : nullptr)
    , m_async        (m_compileMethod == DxvkShaderCompileMethod::Async
          ? new DxvkAsyncCompiler(device)
          : nullptr) {
    Logger::info(str::format(
      "DXVK: Shader compilation method: ", method_name(m_compileMethod)));
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
    DxvkPipelineCount result;
      result.numGraphicsPipelines = m_numGraphicsPipelines.load();
      result.numComputePipelines  = m_numComputePipelines.load();
      return result;
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
