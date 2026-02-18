#pragma once

#include <atomic>
#include <vector>
#include <array>
#include <assert.h>
#include "../../util/util_sleep.h"
#include "../../util/log/log.h"
#include "../../util/util_string.h"


namespace dxvk {

  class FramePacer;


  struct LatencyMarkers {

    using time_point = high_resolution_clock::time_point;

    time_point start;
    time_point end;

    int32_t csStart;
    int32_t csFinished;
    int32_t cpuFinished;
    int32_t gpuStart;
    int32_t gpuFinished;
    int32_t presentFinished;

    std::vector<time_point> gpuReady;
    std::vector<time_point> gpuSubmit;
    std::vector<time_point> gpuQueueSubmit;

  };


  /*
   * stores which information is accessible for which frame
   */
  struct LatencyMarkersTimeline {

    std::atomic<uint64_t> cpuFinished   = { 0 };
    std::atomic<uint64_t> gpuStart      = { 0 };
    std::atomic<uint64_t> gpuFinished   = { 0 };
    std::atomic<uint64_t> frameFinished = { 0 };

  };


  class LatencyMarkersStorage {
    friend class FramePacer;
  public:

    LatencyMarkersStorage( uint64_t firstFrameId )
    : m_firstFrameId(firstFrameId) {
      LatencyMarkers* markers = getMarkers(firstFrameId);
      markers->start = high_resolution_clock::now();
    }

    ~LatencyMarkersStorage() { }

    void registerFrameStart( uint64_t frameId ) {
      if (unlikely(frameId <= m_timeline.frameFinished.load())) {
        Logger::warn( str::format("internal error during registerFrameStart: expected frameId=",
          m_timeline.frameFinished.load()+1, ", got: ", frameId) );
      }
      auto now = high_resolution_clock::now();

      LatencyMarkers* markers = getMarkers(frameId);
      markers->start = now;
    }

    void registerFrameEnd( uint64_t frameId ) {
      if (unlikely(frameId <= m_timeline.frameFinished.load())) {
        Logger::warn( str::format("internal error during registerFrameEnd: expected frameId=",
          m_timeline.frameFinished.load()+1, ", got: ", frameId) );
      }
      auto now = high_resolution_clock::now();

      LatencyMarkers* markers = getMarkers(frameId);
      markers->presentFinished = std::chrono::duration_cast<std::chrono::microseconds>(
        now - markers->start).count();
      markers->end = now;

      m_timeline.frameFinished.store(frameId);
    }

    const LatencyMarkersTimeline* getTimeline() const {
      return &m_timeline;
    }

    const LatencyMarkers* getConstMarkers( uint64_t frameId ) const {
      return &m_markers[frameId % m_numMarkers];
    }


  private:

    LatencyMarkers* getMarkers( uint64_t frameId ) {
      return &m_markers[frameId % m_numMarkers];
    }

    // simple modulo hash mapping is used for frameIds. They are expected to monotonically increase by one.
    // only store a small number of past frames to keep the memory footprint low.
    static constexpr uint16_t m_numMarkers = 4;
    const uint64_t m_firstFrameId;
    std::array<LatencyMarkers, m_numMarkers> m_markers = { };
    LatencyMarkersTimeline m_timeline;

  };

}
