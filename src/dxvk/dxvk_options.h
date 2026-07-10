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

    // Zero-initialize host-visible mapped memory on allocation.
    // Works around games that assume freshly mapped buffers are clean.
    bool zeroMappedMemory;

    /// Whether to use custom sin/cos approximation
    Tristate lowerSinCos = Tristate::Auto;

    /// Memory budget in bytes
    VkDeviceSize maxMemoryBudget;

    // Enable or disable Dyasync
    bool enableDyasync;

    // Number of compiler threads
    // when using Dyasync
    int32_t numDyasyncThreads;

    // Enable or disable the paged memory allocator. Falls back to
    // the legacy chunk allocator per-request when disabled, or
    // when a request doesn't fit the paged allocator's model.
    bool enablePagedAllocator;

    /// Frame pacing mode. Supported values: "", "low-latency", "min-latency".
    /// Empty (default) preserves Sarek existing behaviour unchanged.
    std::string framePace;

    /// Fine-tuning offset for low-latency frame pacing, in microseconds.
    /// Clamped to [-10000, 10000]. Defaults to 0.
    int32_t lowLatencyOffset = 0;

    /// Shader-related options
    Tristate useRawSsbo;

    /// Workaround for NVIDIA driver bug 3114283
    Tristate shrinkNvidiaHvvHeap;

    /// HUD elements
    std::string hud;
  };

}
