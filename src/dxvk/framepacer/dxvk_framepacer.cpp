#include "dxvk_framepacer.h"
#include "dxvk_framepacer_mode_low_latency.h"
#include "dxvk_framepacer_mode_min_latency.h"
#include "dxvk_options.h"
#include "../dxvk_device.h"
#include "../../util/util_flush.h"
#include "../../util/util_env.h"
#include "../../util/log/log.h"

namespace dxvk {

  int getRefreshRate( std::string s ) {
    return std::abs( std::atoi( s.substr(16).c_str() ) );
  }


  FramePacer::FramePacer( DxvkDevice* device, const DxvkOptions& options, uint64_t firstFrameId )
  : m_latencyMarkersStorage(firstFrameId), m_device(device), m_calibratedDeviceTimestamps(device) {
    // We'll default to LOW_LATENCY, which generally provides the best "input lag"
    // along with time consistency and often appears the smoothest too.
    // MAX_FRAME_LATENCY can have advantages in some games like God of War that provide inconsistent
    // cpu frametimes. Also, it's tuned for highest fps which can be relevant in benchmarks.
    FramePacerMode::Mode mode = FramePacerMode::LOW_LATENCY;
    int refreshRate = 0;

    std::string configStr = env::getEnvVar("DXVK_FRAME_PACE");

    if (configStr.find("max-frame-latency") != std::string::npos) {
      mode = FramePacerMode::MAX_FRAME_LATENCY;
    } else if (configStr.find("low-latency-vrr-") != std::string::npos) {
      mode = FramePacerMode::LOW_LATENCY_VRR;
      refreshRate = getRefreshRate(configStr);
    } else if (configStr.find("low-latency") != std::string::npos) {
      mode = FramePacerMode::LOW_LATENCY;
    } else if (configStr.find("min-latency") != std::string::npos) {
      mode = FramePacerMode::MIN_LATENCY;
    } else if (options.framePace.find("max-frame-latency") != std::string::npos) {
      mode = FramePacerMode::MAX_FRAME_LATENCY;
    } else if (options.framePace.find("low-latency-vrr-") != std::string::npos) {
      mode = FramePacerMode::LOW_LATENCY_VRR;
      refreshRate = getRefreshRate(options.framePace);
    } else if (options.framePace.find("low-latency") != std::string::npos) {
      mode = FramePacerMode::LOW_LATENCY;
    } else if (options.framePace.find("min-latency") != std::string::npos) {
      mode = FramePacerMode::MIN_LATENCY;
    } else if (!configStr.empty()) {
      Logger::warn( str::format( "DXVK_FRAME_PACE=", configStr, " unknown" ));
    } else if (!options.framePace.empty()) {
      Logger::warn( str::format( "dxvk.framePace = ", options.framePace, " unknown" ));
    }

    switch (mode) {
      case FramePacerMode::MAX_FRAME_LATENCY:
        Logger::info( "Frame pace: max-frame-latency" );
        m_frameSync.m_waitIsActive = false;
        m_frameSync.m_signalIsActive = false;
        m_mode = std::make_unique<FramePacerMode>(FramePacerMode::MAX_FRAME_LATENCY, "max-frame-latency", &m_latencyMarkersStorage, &m_frameSync, firstFrameId);
        break;

      case FramePacerMode::LOW_LATENCY:
        Logger::info( "Frame pace: low-latency" );
        GpuFlushTracker::m_minPendingSubmissions = 1;
        GpuFlushTracker::m_minChunkCount = 1;
        m_calibratedDeviceTimestamps.enable();
        m_mode = std::make_unique<LowLatencyMode>(mode, &m_latencyMarkersStorage, &m_frameSync, options, firstFrameId);
        break;

      case FramePacerMode::LOW_LATENCY_VRR:
        Logger::info( "Frame pace: low-latency-vrr" );
        GpuFlushTracker::m_minPendingSubmissions = 1;
        GpuFlushTracker::m_minChunkCount = 1;
        m_calibratedDeviceTimestamps.enable();
        m_mode = std::make_unique<LowLatencyMode>(mode, &m_latencyMarkersStorage, &m_frameSync, options, firstFrameId, refreshRate);
        break;

      case FramePacerMode::MIN_LATENCY:
        Logger::info( "Frame pace: min-latency" );
        GpuFlushTracker::m_minPendingSubmissions = 1;
        GpuFlushTracker::m_minChunkCount = 1;
        m_frameSync.m_waitLatency = 1;
        m_mode = std::make_unique<MinLatencyMode>(mode, &m_latencyMarkersStorage, &m_frameSync, firstFrameId);
        break;
    }

    for (auto& gpuStart: m_gpuStarts) {
      gpuStart.store(0);
    }

    // be consistent that every frame has a gpuReady event from finishing the previous frame
    LatencyMarkers* m = m_latencyMarkersStorage.getMarkers( firstFrameId );
    auto now = high_resolution_clock::now();
    m->gpuReady.push_back( now );
    m_mode->notifyGpuReady( firstFrameId, now );
    m_gpuStarts[ firstFrameId % m_gpuStarts.size() ] = gpuReadyBit;

    LatencyMarkersTimeline& timeline = m_latencyMarkersStorage.m_timeline;
    timeline.cpuFinished.store   ( firstFrameId-1 );
    timeline.gpuStart.store      ( firstFrameId-1 );
    timeline.gpuFinished.store   ( firstFrameId-1 );
    timeline.frameFinished.store ( firstFrameId-1 );

    m_frameSync.signalGpuStart       ( firstFrameId-1 );
    m_frameSync.signalRenderFinished ( firstFrameId-1 );
    m_frameSync.signalFrameFinished  ( firstFrameId-1 );
    m_frameSync.signalCsFinished     ( firstFrameId );

    if (m_calibratedDeviceTimestamps.isEnabled()) {
      VkQueryPoolCreateInfo queryPoolInfo = {};
      queryPoolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
      queryPoolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
      queryPoolInfo.queryCount = 1;

      VkQueryPool* queryPools = m_queryPools.getDataUnsafe();
      for (int i=0; i<256; ++i) {
        VkResult res = device->vkd()->vkCreateQueryPool(device->handle(), &queryPoolInfo, nullptr, &queryPools[i]);

        if (res != VK_SUCCESS)
          throw DxvkError("FramePacer: Failed to create submit query pool");
      }
    }
  }


  FramePacer::~FramePacer() {
    delete m_presentationStats.load();
    delete m_gpuBufferStats.load();

    if (m_calibratedDeviceTimestamps.isEnabled()) {
      for (int i=0; i<256; ++i) {
        // calling queryPool.alloc() ensures it's not in use
        m_device->vkd()->vkDestroyQueryPool(m_device->handle(), *m_queryPools.alloc(), nullptr);
      }
    }
  }


  VkResult FramePacer::getSubmitQueryPoolResult( VkQueryPool* queryPool, uint64_t* timestamp ) {
    VkQueryResultFlags flags = VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT;
    VkResult res = m_device->vkd()->vkGetQueryPoolResults(
      m_device->handle(), *queryPool, 0, 1, sizeof(uint64_t),
      timestamp, sizeof(uint64_t), flags
    );

    if (unlikely(res != VK_SUCCESS))
      Logger::err( str::format( "FramePacer: vkGetQueryPoolResults returned ", res) );

    return res;
  }

}
