#pragma once

#include "../util/config/config.h"

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
