#pragma once

#include "thread.h"
#include "util_time.h"

namespace dxvk {

  /**
   * \brief Frame pacing mode
   *
   * How the limiter waits out the remainder of a frame.
   *
   * \li \c Precise sleeps coarsely, then busy-waits the last stretch.
   *     Most accurate, and keeps a core hot every frame.
   * \li \c Sleep issues one sleep for the whole remainder. Cheapest, and
   *     only as accurate as the platform timer.
   * \li \c Sliced sleeps in bounded steps, re-measuring after each, then
   *     busy-waits a short final margin.
   * \li \c Spin busy-waits the entire remainder.
   *
   * Where the game is CPU-bound the busy-wait competes with it for a core
   * it needs, which is why the cheaper modes are worth having even though
   * they pace less tightly.
   */
  enum class FpsLimitPacing : uint32_t {
    Precise = 0,
    Sleep   = 1,
    Sliced  = 2,
    Spin    = 3,
  };

  /**
   * \brief Frame rate limiter method
   *
   * How the deadline for the next frame is derived.
   *
   * \li \c Deviation carries a correction term across frames so that
   *     sleep inaccuracy averages out.
   * \li \c Timeline holds an absolute cadence, advancing by exactly one
   *     interval per frame and resynchronising only after a frame that
   *     overran a whole interval.
   * \li \c Reactive measures each interval from the frame just presented
   *     and never tries to catch up.
   */
  enum class FpsLimitMethod : uint32_t {
    Deviation = 0,
    Timeline  = 1,
    Reactive  = 2,
  };

  /**
   * \brief Frame rate limiter
   *
   * Provides functionality to stall an application
   * thread in order to maintain a given frame rate.
   */
  class FpsLimiter {

  public:

    /**
     * \brief Creates frame rate limiter
     */
    FpsLimiter();

    ~FpsLimiter();

    /**
     * \brief Sets target frame rate
     * \param [in] frameRate Target frame rate
     */
    void setTargetFrameRate(double frameRate);

    /**
     * \brief Stalls calling thread as necessary
     *
     * Blocks the calling thread if the limiter is enabled
     * and the time since the last call to \ref delay is
     * shorter than the target interval.
     * \param [in] vsyncEnabled \c true if vsync is enabled
     */
    void delay(bool vsyncEnabled);

    /**
     * \brief Checks whether the frame rate limiter is enabled
     * \returns \c true if the target frame rate is non-zero.
     */
    bool isEnabled() const {
      return m_targetInterval != NtTimerDuration::zero();
    }

  private:

    using TimePoint = dxvk::high_resolution_clock::time_point;

    using NtTimerDuration = std::chrono::duration<int64_t, std::ratio<1, 10000000>>;
    using NtQueryTimerResolutionProc = UINT (WINAPI *) (ULONG*, ULONG*, ULONG*);
    using NtSetTimerResolutionProc = UINT (WINAPI *) (ULONG, BOOL, ULONG*);
    using NtDelayExecutionProc = UINT (WINAPI *) (BOOL, LARGE_INTEGER*);

    dxvk::mutex     m_mutex;

    NtTimerDuration m_targetInterval = NtTimerDuration::zero();
    bool            m_isSoftLimit    = false;
    NtTimerDuration m_deviation       = NtTimerDuration::zero();
    TimePoint       m_lastFrame;

    bool            m_initialized     = false;
    bool            m_envOverride     = false;

    NtTimerDuration m_sleepGranularity = NtTimerDuration::zero();
    NtTimerDuration m_sleepThreshold   = NtTimerDuration::zero();

    FpsLimitPacing  m_pacing = FpsLimitPacing::Precise;
    FpsLimitMethod  m_method = FpsLimitMethod::Deviation;

    TimePoint       m_nextFrame;
    bool            m_hasTarget         = false;
    NtTimerDuration m_targetOfLastFrame = NtTimerDuration::zero();

    NtDelayExecutionProc NtDelayExecution = nullptr;

    void delayDeviation();

    void delayTimeline();

    TimePoint nextTarget(TimePoint now);

    TimePoint sleep(TimePoint t0, NtTimerDuration duration);

    TimePoint sleepOnce(NtTimerDuration duration);

    TimePoint sleepPrecise(TimePoint t0, NtTimerDuration duration);

    TimePoint sleepSliced(TimePoint t0, NtTimerDuration duration);

    TimePoint spinUntil(TimePoint t0, NtTimerDuration duration);

    void initialize();

  };

}
