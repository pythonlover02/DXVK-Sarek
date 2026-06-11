#include "dxvk_options.h"

namespace dxvk {

  DxvkOptions::DxvkOptions(const Config& config) {
    enableDebugUtils      = config.getOption<bool>    ("dxvk.enableDebugUtils",       false);
    enableStateCache      = config.getOption<bool>    ("dxvk.enableStateCache",       true);
    numCompilerThreads    = config.getOption<int32_t> ("dxvk.numCompilerThreads",     0);
    useRawSsbo            = config.getOption<Tristate>("dxvk.useRawSsbo",             Tristate::Auto);
    shrinkNvidiaHvvHeap   = config.getOption<Tristate>("dxvk.shrinkNvidiaHvvHeap",    Tristate::Auto);
    hud                   = config.getOption<std::string>("dxvk.hud", "");
    hideIntegratedGraphics = config.getOption<bool>   ("dxvk.hideIntegratedGraphics", false);
    deviceFilter          = config.getOption<std::string>("dxvk.deviceFilter",        "");
    auto budget = config.getOption<int32_t>("dxvk.maxMemoryBudget", 0);
    maxMemoryBudget = VkDeviceSize(std::max(budget, 0)) << 20u;
    enableDyasync         = config.getOption<bool>    ("dxvk.enableDyasync",          true);
    numDyasyncThreads     = config.getOption<int32_t> ("dxvk.numDyasyncThreads",      0);
  }

}
