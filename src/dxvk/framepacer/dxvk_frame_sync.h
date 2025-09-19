#pragma once

#include "../util/sync/sync_signal.h"
#include <atomic>

namespace dxvk {

  class FrameSync {

  public:

    void waitRenderFinished( uint64_t frameId ) {
      if (m_waitIsActive) m_fenceGpuFinished.wait(frameId-m_waitLatency); }

    void signalRenderFinished( uint64_t frameId ) {
      if (m_signalIsActive) m_fenceGpuFinished.signal(frameId); }

    void signalFrameFinished( uint64_t frameId ) {
      if (m_signalIsActive) m_fenceFrameFinished.signal(frameId); }

    void signalGpuStart( uint64_t frameId ) {
      if (m_signalIsActive) m_fenceGpuStart.signal(frameId); }

    void signalCsFinished( uint64_t frameId ) {
      if (m_signalIsActive) m_fenceCsFinished.signal(frameId); }

    sync::Fence m_fenceGpuStart;
    sync::Fence m_fenceGpuFinished;
    sync::Fence m_fenceFrameFinished;
    sync::Fence m_fenceCsFinished;

    std::atomic< bool > m_waitIsActive   = { true };
    std::atomic< bool > m_signalIsActive = { true };
    std::atomic< int32_t > m_waitLatency = { 2 };

  };


}