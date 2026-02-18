#pragma once

#include <stdint.h>
#include <array>
#include <atomic>
#include <deque>
#include <assert.h>

#include "../../util/util_time.h"


namespace dxvk {

  class LatencyStats {

  public:

    using time_point = high_resolution_clock::time_point;

    LatencyStats( int32_t duration_ms ) : m_duration(duration_ms) { }

    void push( time_point t, int32_t latency ) {

      int32_t index = getBucketIndex(latency);

      ++m_buckets[index];
      ++m_numLatencies;

      QueueItem item;
      item.timeStamp = t;
      item.latency   = latency;

      m_queue.push_back(item);

      // remove old items from the queue
      while (!m_queue.empty() && m_queue.front().timeStamp
        < high_resolution_clock::now() - std::chrono::milliseconds(m_duration) ) {
        index = getBucketIndex(m_queue.front().latency);
        --m_buckets[index];
        --m_numLatencies;
        m_queue.pop_front();
      }

    }


    int32_t getMedian( uint64_t frameId ) {

      // use a cache so we can efficiently call this multiple times per frame
      if (frameId == 0 || m_cachedMedian.frameId == frameId)
        return m_cachedMedian.median;

      int32_t median = getPercentile(0.5);

      m_cachedMedian.frameId = frameId;
      m_cachedMedian.median  = median;

      return median;

    }


    int32_t getPercentile( float p ) const {

      assert( p >= 0 && p <= 1 );

      uint64_t targetCount = m_numLatencies * p;
      uint64_t count = 0;
      size_t index = 0;
      while (count < targetCount && index < m_buckets.size()) {
        count += m_buckets[index];
        ++index;
      }

      if (index > 0) --index;
      return index * 8;

    }


  private:

    int getBucketIndex( int32_t latency ) const {
      assert( latency >= 0 );
      size_t index = latency / 8;
      return std::min( m_buckets.size()-1, index );
    }

    constexpr static int32_t maxLatency = 5000;
    const int32_t m_duration;

    std::array< std::atomic<int64_t>, 1+(maxLatency / 8) > m_buckets = { };
    std::atomic< int64_t > m_numLatencies = { 0 };

    struct QueueItem {
      time_point timeStamp;
      int32_t    latency;
    };

    // must only be accessed from one thread
    std::deque< QueueItem > m_queue;

    struct CachedMedian {
      uint64_t frameId;
      int32_t  median;
    };

    CachedMedian m_cachedMedian = { };

  };


  class LatencyAverage {
  public:

    using time_point = high_resolution_clock::time_point;

    void push( int32_t latency ) {

      m_totalLatency -= m_latencies[m_curIndex];
      m_totalLatency += latency;
      m_latencies[m_curIndex] = latency;
      ++m_curIndex;

      m_average.store( m_totalLatency / size, std::memory_order_release );

    }

    int32_t getAverage() const
      { return m_average.load( std::memory_order_acquire ); }

  private:

    // note this cannot be changed unless m_curIndex is tracked accordingly
    // as we let it overflow automatically
    static constexpr size_t size = 256;
    std::array<int32_t, size> m_latencies = { };

    uint8_t m_curIndex = { 0 };
    int64_t m_totalLatency = { 0 };
    std::atomic<int32_t> m_average = { 0 };

  };

}
