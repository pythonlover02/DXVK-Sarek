#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>

#include "../../util/sync/sync_signal.h"
#include "../../util/util_time.h"

namespace dxvk {

  struct DxvkOptions;

  // Mirrors netborg-afps/dxvk-low-latency dxvk.framePace modes, scaled down
  // to what Sarek synchronous vk::Presenter and per-frame CallbackFence can
  // actually observe (no per-submission GPU timestamps, no async present
  // completion signal).
  enum class DxvkFramePace : uint32_t {
    MaxFrameLatency = 0,
    LowLatency      = 1,
    MinLatency      = 2,
  };

  class FramePacer {

  public:

    FramePacer(const DxvkOptions& options, uint64_t firstFrameId);

    // LowLatency and MinLatency both clamp to the same minimal buffering
    // depth here; Sarek has no per-submission GPU progress signal to tell
    // them apart the way upstream predictive low-latency mode does.
    uint32_t getEffectiveFrameLatency(uint32_t configuredLatency) const;

    bool needsGpuSignal() const { return m_mode == DxvkFramePace::LowLatency; }

    const Rc<sync::CallbackFence>& signal() const { return m_signal; }

    void beginFrame();

    void endFrame(uint64_t frameId);

  private:

    void recordFrameDuration(high_resolution_clock::time_point frameStart);

    DxvkFramePace m_mode               = DxvkFramePace::MaxFrameLatency;
    int32_t       m_lowLatencyOffsetUs = 0;

    std::optional<high_resolution_clock::time_point> m_lastFrameStart     = std::nullopt;
    std::atomic<int32_t>                              m_avgFrameDurationUs = { 0 };

    Rc<sync::CallbackFence> m_signal;

  };

}
