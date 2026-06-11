#pragma once

#include "../util/config/config.h"

#include "../vulkan/vulkan_loader.h"

namespace dxvk {

  struct DxvkOptions {
    DxvkOptions() { }
    DxvkOptions(const Config& config);

    /// Enable debug utils (alternative to DXVK_PERF_EVENTS=1)
    bool enableDebugUtils;

    /// Enable state cache
    bool enableStateCache;

    /// Number of compiler threads
    /// when using the state cache
    int32_t numCompilerThreads;

    // Hides integrated GPUs if dedicated GPUs are
    // present. May be necessary for some games that
    // incorrectly assume monitor layouts.
    bool hideIntegratedGraphics;

    /// Device name
    std::string deviceFilter;

    // Tiler GPU tweaks. Currently biases host-visible
    // allocations toward cached memory on tilers; the
    // render-pass-op side is detected but not yet acted on.
    Tristate tilerMode;

    /// Memory budget in bytes
    VkDeviceSize maxMemoryBudget;

    // Enable or disable Dyasync
    bool enableDyasync;

    // Number of compiler threads
    // when using Dyasync
    int32_t numDyasyncThreads;

    /// Shader-related options
    Tristate useRawSsbo;

    /// Workaround for NVIDIA driver bug 3114283
    Tristate shrinkNvidiaHvvHeap;

    /// HUD elements
    std::string hud;
  };

}
