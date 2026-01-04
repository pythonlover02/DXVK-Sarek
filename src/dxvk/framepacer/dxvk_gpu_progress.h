#pragma once

#include <atomic>
#include <stdint.h>
#include "dxvk_latency_markers.h"
#include "../../util/log/log.h"
#include "../../util/util_string.h"


namespace dxvk {

  /*
   * Keeps track of GPU submit processing in realtime to improve frame pacing.
   * This has been shown in testing to reduce the variance for the prediction error
   * (prediction vs. actual) with respect to when a frame will finish, as more
   * recent information is available, compared to doing the prediction when the
   * frame starts or when the GPU starts processing the first submit.
   */

  class GpuProgress {

    using microseconds = std::chrono::microseconds;
    using time_point = high_resolution_clock::time_point;

  public:

    GpuProgress( const LatencyMarkersStorage* storage )
    : m_markerStorage(storage) { }


    void notifyGpuReady( uint64_t frameId, time_point t ) {

      Data* data = getData(frameId);
      Submits expected = data->submits.load();
      Submits desired;

      if (expected.numSubmitsFinished != 0)
        endSubmit( frameId, t );

      do {
        desired = expected;
        desired.numSubmitsFinished++;
      } while (!data->submits.compare_exchange_weak( expected, desired ));

      if (desired.numSubmitsFinished <= desired.numSubmitsQueued)
        startSubmit( frameId, desired.numSubmitsFinished, t );

    }


    void notifyQueueSubmit( uint64_t frameId, time_point t ) {

      Data* data = getData(frameId);
      Submits expected = data->submits.load();
      Submits desired;

      do {
        desired = expected;
        desired.numSubmitsQueued++;
      } while (!data->submits.compare_exchange_weak( expected, desired ));

      if (desired.numSubmitsFinished == desired.numSubmitsQueued)
        startSubmit( frameId, desired.numSubmitsQueued, t );

    }


    void finishRender( uint64_t frameId ) {

      Data* data = getData(frameId);
      data->isFinished = true;

      // frameId+1 should already have received a gpuReady notification
      // so clear the frame after that within the ring buffer
      Data* dataNext = getData(frameId+2);
      dataNext->isFinished.store( false );
      dataNext->gpuRuntime.store( GpuRuntime{} );
      dataNext->submits.store( Submits{} );

    }


    void waitUntil( uint64_t frameId, int32_t targetGpuRuntime, int32_t cpuUntilGpuStart ) const {
      // GPU progress is measured as if the submits would be processed as late as possible
      // on the GPU while assuming the frame to be minimum latency since gpuStart.
      // This is necessary for this to work reliably accross games.

      using std::chrono::duration_cast;
      const Data* data = getConstData(frameId);
      const LatencyMarkers* m = m_markerStorage->getConstMarkers(frameId);

      // Sleep until the earlierst possible time stamp

      auto now = high_resolution_clock::now();
      int32_t totalTime = duration_cast<microseconds>(now - m->start).count();
      cpuUntilGpuStart = std::max( cpuUntilGpuStart, m->gpuStart );
      int32_t sleepUntil = cpuUntilGpuStart + targetGpuRuntime;
      int32_t sleepDelay = std::max( 0, sleepUntil - totalTime );

      Sleep::TimePoint t = now + microseconds(sleepDelay);
      Sleep::sleepUntil( now, t );

      // For the remainder of the frame we check the progress by polling.
      // Usually this will take far less than a millisecond, with the exception
      // of very slow frames where GPU progress is stalled.

      while (true) {

        int32_t gpuRuntime = getGpuRuntime( frameId, cpuUntilGpuStart );
        if (gpuRuntime >= targetGpuRuntime)
          break;
        if (data->isFinished)
          break;
        pause();

      }

    }


    int32_t getGpuRuntime( uint64_t frameId, int32_t cpuUntilGpuStart ) const {

      using std::chrono::duration_cast;
      const Data* data = getConstData(frameId);
      GpuRuntime gpuTime = data->gpuRuntime.load();
      const LatencyMarkers* m = m_markerStorage->getConstMarkers(frameId);

      auto now = high_resolution_clock::now();
      int32_t curTime = duration_cast<microseconds>(now - m->start).count();

      int32_t gpuRuntime = gpuTime.runtime;
      if (gpuTime.curSubmitStart != 0)
        gpuRuntime += curTime - gpuTime.curSubmitStart;

      int32_t gpuIdle = curTime - gpuRuntime;
      gpuRuntime -= std::max( 0, cpuUntilGpuStart - gpuIdle );
      return gpuRuntime;

    }


  private:


    void startSubmit( uint64_t frameId, uint16_t numSubmits, time_point t ) {
      // This is analog to start = std::max( m->gpuReady[i], m->gpuQueueSubmit[i] )
      // Testing has shown that the way we do it here might sometimes diverge from this - very slightly,
      // depending on in which order the messages arrive here, but we cannot access
      // the above variables safely here in real-time.

      using std::chrono::duration_cast;
      Data* data = getData(frameId);
      const LatencyMarkers* m = m_markerStorage->getConstMarkers(frameId);
      GpuRuntime gpuRuntime = data->gpuRuntime.load();

      // this shouldn't happen, but if it does, we will get informed
      if (unlikely(gpuRuntime.curSubmitStart != 0)) {
        Logger::err( "internal error: GpuProgress startSubmit() before endSubmit()" );
      }

      // store the starting timestamp this way so we can use an atomic
      int32_t submitStart = duration_cast<microseconds>(t - m->start).count();
      gpuRuntime.curSubmitStart = submitStart;
      data->gpuRuntime.store( gpuRuntime );

    }


    void endSubmit( uint64_t frameId, time_point t) {

      using std::chrono::duration_cast;
      const LatencyMarkers* m = m_markerStorage->getConstMarkers(frameId);
      Data* data = getData(frameId);
      GpuRuntime gpuRuntime = data->gpuRuntime.load();

      int32_t submitEnd = duration_cast<microseconds>(t - m->start).count();
      int32_t diff = submitEnd - gpuRuntime.curSubmitStart;
      gpuRuntime.runtime += diff;
      gpuRuntime.curSubmitStart = 0;

      data->gpuRuntime.store(gpuRuntime);

    }


    static void pause() {

      #if defined(DXVK_ARCH_X86)
      _mm_pause();
      #elif defined(DXVK_ARCH_ARM64)
      __asm__ __volatile__ ("yield");
      #else
      /* Do nothing (busy-loop). Please add more #elif above here if
       * your CPU architecture has a suitable pause/yield instruction */
      #endif

    }


    struct Submits {
      uint32_t numSubmitsQueued       = { 0 };
      uint32_t numSubmitsFinished     = { 0 };
    };

    struct GpuRuntime {
      int32_t curSubmitStart          = { 0 };
      int32_t runtime                 = { 0 };
    };

    struct Data {
      std::atomic<Submits> submits         = { };
      std::atomic<GpuRuntime> gpuRuntime   = { };
      std::atomic<bool> isFinished         = { false };
    };

    Data* getData( uint32_t frameId ) {
      return &m_data[ frameId % NUM_DATA ];
    }

    const Data* getConstData( uint32_t frameId ) const {
      return &m_data[ frameId % NUM_DATA ];
    }

    constexpr static int32_t NUM_DATA = 4;
    std::array< Data, NUM_DATA > m_data = { };

    const LatencyMarkersStorage* m_markerStorage;

  };

}

