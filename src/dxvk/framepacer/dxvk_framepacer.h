#pragma once

#include "dxvk_framepacer_mode.h"
#include "dxvk_frame_sync.h"
#include "dxvk_latency_markers.h"
#include "dxvk_latency_stats.h"
#include "dxvk_calibrated_device_timestamps.h"
#include "../dxvk_latency.h"
#include "../../util/util_time.h"
#include "../../util/sync/sync_ringbuffer_allocator.h"


namespace dxvk {

  struct DxvkOptions;
  class DxvkDevice;

  /* \brief Frame pacer interface managing the CPU - GPU synchronization.
   *
   * GPUs render frames asynchronously to the game's and dxvk's CPU-side work
   * in order to improve fps-throughput. Aligning the cpu work to chosen time-
   * points allows to tune certain characteristics of the video presentation,
   * like smoothness and latency.
   */

  class FramePacer : public DxvkLatencyTracker {
    using microseconds = std::chrono::microseconds;
    using time_point   = high_resolution_clock::time_point;
  public:

    FramePacer( DxvkDevice* device, const DxvkOptions& options, uint64_t firstFrameId );
    ~FramePacer();

    void sleepAndBeginFrame(
            uint64_t                  frameId,
            double                    maxFrameRate) override {
      // wait for finished rendering of a previous frame, typically the one before last
      m_frameSync.waitRenderFinished(frameId);
      // potentially wait some more if the cpu gets too much ahead
      m_mode->startFrame(frameId);
      m_latencyMarkersStorage.registerFrameStart(frameId);
    }

    void notifyGpuPresentEnd( uint64_t frameId ) override {
      // the frame has been displayed to the screen
      m_latencyMarkersStorage.registerFrameEnd(frameId);
      m_mode->endFrame(frameId);
      m_frameSync.signalFrameFinished(frameId);
      m_gpuStarts[ (frameId-1) % m_gpuStarts.size() ].store(0);
      trackStats(frameId);
    }

    void notifyCsRenderBegin( uint64_t frameId ) override {
      auto now = high_resolution_clock::now();
      LatencyMarkers* m = m_latencyMarkersStorage.getMarkers(frameId);
      m->csStart = std::chrono::duration_cast<microseconds>(now - m->start).count();
    }

    void notifyCsRenderEnd( uint64_t frameId ) override {
      auto now = high_resolution_clock::now();
      LatencyMarkers* m = m_latencyMarkersStorage.getMarkers(frameId);
      m->csFinished = std::chrono::duration_cast<microseconds>(now - m->start).count();
      m_frameSync.signalCsFinished( frameId );
    }

    void notifySubmit( uint64_t frameId ) override {
      LatencyMarkers* m = m_latencyMarkersStorage.getMarkers(frameId);
      m->gpuSubmit.push_back(high_resolution_clock::now());
    }

    void notifyPresent( uint64_t frameId ) override {
      // dx to vk translation is finished
      if (frameId != 0) {
        auto now = high_resolution_clock::now();
        LatencyMarkers* m = m_latencyMarkersStorage.getMarkers(frameId);
        LatencyMarkers* next = m_latencyMarkersStorage.getMarkers(frameId+1);
        m->cpuFinished = std::chrono::duration_cast<microseconds>(now - m->start).count();
        next->gpuSubmit.clear();

        m_latencyMarkersStorage.m_timeline.cpuFinished.store(frameId);
      }
    }

    void notifyQueueSubmit( uint64_t frameId ) override {
      auto now = high_resolution_clock::now();
      LatencyMarkers* m = m_latencyMarkersStorage.getMarkers(frameId);
      m->gpuQueueSubmit.push_back(now);
      queueSubmitCheckGpuStart(frameId, m, now);
      m_mode->notifyQueueSubmit(frameId, now);
    }

    void notifyQueuePresentBegin( uint64_t frameId ) override {
      if (frameId != 0) {
        auto now = high_resolution_clock::now();
        LatencyMarkers* m = m_latencyMarkersStorage.getMarkers(frameId);
        LatencyMarkers* next = m_latencyMarkersStorage.getMarkers(frameId+1);
        next->gpuQueueSubmit.clear();
        queueSubmitCheckGpuStart(frameId, m, now);
      }
    }

    void notifyGpuExecutionEnd( uint64_t frameId, VkQueryPool* queryPool ) override {
      LatencyMarkers* m = m_latencyMarkersStorage.getMarkers(frameId);

      if (queryPool == nullptr) {
        auto now = high_resolution_clock::now();
        m->gpuReady.push_back(now);
        m_mode->notifyGpuReady(frameId, now);
        return;
      }

      uint64_t timestamp;
      VkResult res = getSubmitQueryPoolResult( queryPool, &timestamp );

      if (unlikely(res != VK_SUCCESS)) {
        auto now = high_resolution_clock::now();
        m->gpuReady.push_back(now);
        m_mode->notifyGpuReady(frameId, now);
        m_queryPools.free(queryPool);
        return;
      }

      auto t = m_calibratedDeviceTimestamps.getHostTimestamp(timestamp);
      if (unlikely(t == high_resolution_clock::time_point{}))
        t = high_resolution_clock::now();

      m->gpuReady.push_back(t);
      m_mode->notifyGpuReady(frameId, t);

      m_queryPools.free(queryPool);
    }

    virtual void notifyGpuPresentBegin( uint64_t frameId ) override {
      // we get frameId == 0 for repeated presents (SyncInterval)
      if (frameId != 0) {
        LatencyMarkers* m = m_latencyMarkersStorage.getMarkers(frameId);
        LatencyMarkers* next = m_latencyMarkersStorage.getMarkers(frameId+1);
        assert( !m->gpuReady.empty() );
        auto t = m->gpuReady.back();
        m->gpuFinished = std::chrono::duration_cast<microseconds>(t - m->start).count();
        next->gpuReady.clear();
        next->gpuReady.push_back(t);
        m_mode->notifyGpuReady(frameId+1, t);
        m_calibratedDeviceTimestamps.calibrate();

        gpuExecutionCheckGpuStart(frameId+1, next, t);

        m_latencyAverage.push(m->gpuFinished);
        m_latencyMarkersStorage.m_timeline.gpuFinished.store(frameId);
        m_mode->finishRender(frameId);
        m_frameSync.signalRenderFinished(frameId);
      }
    }

    VkQueryPool* allocSubmitQueryPool() override
      { return m_calibratedDeviceTimestamps.isEnabled() ? m_queryPools.alloc() : nullptr; }

    void freeSubmitQueryPool( VkQueryPool* queryPool ) override
      { m_queryPools.free( queryPool ); }

    FramePacerMode::Mode getMode() const {
      return m_mode->m_mode;
    }

    FramePacerMode* getFramePacerMode() {
      return m_mode.get();
    }

    void setTargetFrameRate( double frameRate ) {
      m_mode->setTargetFrameRate(frameRate);
    }

    bool needsAutoMarkers() override {
      return true;
    }

    LatencyMarkersStorage m_latencyMarkersStorage;
    FrameSync m_frameSync;


    // not implemented methods


    void notifyCpuPresentBegin( uint64_t frameId ) override { }
    void notifyCpuPresentEnd( uint64_t frameId ) override { }
    void notifyQueuePresentEnd( uint64_t frameId, VkResult status) override { }
    void notifyGpuExecutionBegin( uint64_t frameId ) override { }
    void discardTimings() override { }
    DxvkLatencyStats getStatistics( uint64_t frameId ) override
      { return DxvkLatencyStats(); }


    // non-overriding methods


    VkResult getSubmitQueryPoolResult( VkQueryPool* queryPool, uint64_t* timestamp );

    int32_t getLatencyAverage() const
      { return m_latencyAverage.getAverage(); }

    const LatencyStats* getGpuBufferStats() const
      { return m_gpuBufferStats.load(); }

    const LatencyStats* getPresentStats() const
      { return m_presentationStats.load(); }

    std::atomic< bool > m_enableGpuBufferTracking = { false };
    std::atomic< bool > m_enableVSyncBufferTracking = { false };

  private:

    void signalGpuStart( uint64_t frameId, LatencyMarkers* m, const time_point& t ) {
      m->gpuStart = std::chrono::duration_cast<microseconds>(t - m->start).count();
      m_latencyMarkersStorage.m_timeline.gpuStart.store(frameId);
      m_frameSync.signalGpuStart(frameId);
    }

    void queueSubmitCheckGpuStart( uint64_t frameId, LatencyMarkers* m, const time_point& t ) {
      auto& gpuStart = m_gpuStarts[ frameId % m_gpuStarts.size() ];
      uint16_t val = gpuStart.fetch_or(queueSubmitBit);
      if (val == gpuReadyBit)
        signalGpuStart( frameId, m, t );
    }

    void gpuExecutionCheckGpuStart( uint64_t frameId, LatencyMarkers* m, const time_point& t ) {
      auto& gpuStart = m_gpuStarts[ frameId % m_gpuStarts.size() ];
      uint16_t val = gpuStart.fetch_or(gpuReadyBit);
      if (val == queueSubmitBit)
        signalGpuStart( frameId, m, t );
    }

    void trackStats( uint64_t frameId ) {
      const LatencyMarkers* m = m_latencyMarkersStorage.getConstMarkers(frameId);

      // will be re-enabled for VK_EXT_present_timing, but for now this isn't accurate enough
      if (false && m_enableVSyncBufferTracking) {
        if (!m_presentationStats)
          m_presentationStats.store( new LatencyStats(3000) );
        m_presentationStats.load()->push( m->end, m->presentFinished - m->gpuFinished );
      }

      if (m_enableGpuBufferTracking) {
        if (!m_gpuBufferStats)
          m_gpuBufferStats.store( new LatencyStats(3000) );

        int64_t minDiff = std::numeric_limits<int64_t>::max();
        size_t i = 0;
        while (m->gpuSubmit.size() > i && m->gpuReady.size() > i) {
          int64_t diff = std::chrono::duration_cast<microseconds>(
            m->gpuReady[i] - m->gpuSubmit[i]).count();
          diff = std::max( (int64_t) 0, diff );
          minDiff = std::min( minDiff, diff );
          ++i;
        }

        if (minDiff != std::numeric_limits<int64_t>::max())
          m_gpuBufferStats.load()->push( m->end, minDiff );
      }
    }

    DxvkDevice* m_device;
    std::unique_ptr<FramePacerMode> m_mode;

    std::array< std::atomic< uint16_t >, 8 > m_gpuStarts = { };
    static constexpr uint16_t queueSubmitBit = 1;
    static constexpr uint16_t gpuReadyBit    = 2;

    std::atomic<LatencyStats*> m_gpuBufferStats = { nullptr };
    std::atomic<LatencyStats*> m_presentationStats = { nullptr };
    LatencyAverage m_latencyAverage;

    CalibratedDeviceTimestamps m_calibratedDeviceTimestamps;
    sync::RingbufferAllocator<VkQueryPool, 256> m_queryPools;

  };

}
