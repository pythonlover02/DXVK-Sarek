#pragma once

#include "../../util/sync/sync_atomic_signal.h"
#include "../../util/util_sleep.h"
#include "../../util/thread.h"
#include <atomic>


namespace dxvk {


  class ThreadedSleep {

  public:

    using TimePoint = Sleep::TimePoint;

    ThreadedSleep()
    : m_thread([this] { threadFunc(); })
    { }


    ~ThreadedSleep() {

      m_stopped.store(true);
      m_start.signal_one();
      m_thread.join();

    }


    void sleepUntil( const TimePoint& t ) {

      constexpr bool threadedSleep = true;
      if constexpr (!threadedSleep) {
        Sleep::sleepUntil(high_resolution_clock::now(), t);
        return;
      }

      // don't sleep threaded if the sleep duration is too short
      // setup the wake up early enough so the scheduler "cannot" fail to miss "t"

      TimePoint t2 = t - std::chrono::microseconds(150);
      if (high_resolution_clock::now() - std::chrono::microseconds(100) < t2) {

        m_t.store(t2);
        m_start.signal_one();
        m_finish.wait();

      }

      // spin-wait the remaining microseconds

      TimePoint now;
      do {
        now = high_resolution_clock::now();
      } while (now < t);

    }


    void threadFunc() {

      while (!m_stopped.load( std::memory_order_acquire)) {

        m_start.wait();
        TimePoint t = m_t.load();
        Sleep::sleepUntil( high_resolution_clock::now(), t );
        m_finish.signal_one();

      }

      m_finish.signal_one();

    }

  private:

    std::atomic<bool> m_stopped = { false };
    dxvk::thread m_thread;

    sync::AtomicSignal m_start  = { "m_start", false };
    sync::AtomicSignal m_finish = { "m_finish", false };

    std::atomic<TimePoint> m_t = { high_resolution_clock::now() };

    static_assert( std::atomic<TimePoint>::is_always_lock_free );

  };


}
