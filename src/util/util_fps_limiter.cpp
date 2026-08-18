#include <algorithm>
#include <thread>

#include "thread.h"
#include "util_env.h"
#include "util_fps_limiter.h"
#include "util_string.h"

#include "./log/log.h"

namespace dxvk {

  namespace {

    // Wait budgets in 100 ns units, matching NtTimerDuration's period. The
    // slice step bounds how long any single sleep call may run, so one
    // mispredicted sleep costs at most a step rather than the rest of the
    // frame.
    constexpr int64_t SliceMarginTicks = 1500;   // 0.15 ms
    constexpr int64_t SliceStepTicks   = 10000;  // 1 ms

    FpsLimitPacing pacing_from_env(FpsLimitPacing fallback) {
      const std::string name = env::getEnvVar("DXVK_FRAME_RATE_PACING");

      if (name == "precise")
        return FpsLimitPacing::Precise;

      if (name == "sleep")
        return FpsLimitPacing::Sleep;

      if (name == "sliced")
        return FpsLimitPacing::Sliced;

      if (name == "spin")
        return FpsLimitPacing::Spin;

      return fallback;
    }

    FpsLimitMethod method_from_env(FpsLimitMethod fallback) {
      const std::string name = env::getEnvVar("DXVK_FRAME_RATE_METHOD");

      if (name == "deviation")
        return FpsLimitMethod::Deviation;

      if (name == "timeline")
        return FpsLimitMethod::Timeline;

      if (name == "reactive")
        return FpsLimitMethod::Reactive;

      return fallback;
    }

  }


  FpsLimiter::FpsLimiter() {
    std::string env = env::getEnvVar("DXVK_FRAME_RATE");

    if (!env.empty()) {
      try {
        setTargetFrameRate(std::stod(env));
        m_envOverride = true;
      } catch (const std::invalid_argument&) {
        // no-op
      }
    }

    m_pacing = pacing_from_env(m_pacing);
    m_method = method_from_env(m_method);
  }


  FpsLimiter::~FpsLimiter() {

  }


  void FpsLimiter::setTargetFrameRate(double frameRate) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    if (!m_envOverride) {
      // Negative rates are a soft cap (used by the swapchain to align frame pacing
      // with refresh when SyncInterval > 1); positive rates are a hard cap.
      m_isSoftLimit = frameRate < 0.0;
      double absRate = m_isSoftLimit ? -frameRate : frameRate;

      m_targetInterval = absRate > 0.0
        ? NtTimerDuration(int64_t(double(NtTimerDuration::period::den) / absRate))
        : NtTimerDuration::zero();

      if (isEnabled() && !m_initialized)
        initialize();
    }
  }


  void FpsLimiter::delay(bool vsyncEnabled) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    if (!isEnabled())
      return;

    switch (m_method) {
      case FpsLimitMethod::Deviation:
        this->delayDeviation();
        break;

      case FpsLimitMethod::Timeline:
      case FpsLimitMethod::Reactive:
        this->delayTimeline();
        break;
    }
  }


  void FpsLimiter::delayDeviation() {
    auto t0 = m_lastFrame;
    auto t1 = dxvk::high_resolution_clock::now();

    auto frameTime = std::chrono::duration_cast<NtTimerDuration>(t1 - t0);

    if (frameTime * 100 > m_targetInterval * 103 - m_deviation * 100) {
      // If we have a slow frame, reset the deviation since we do not want to
      // compensate for low performance later on. For soft limits we keep the
      // deviation so the limiter only really nudges when the app is consistently
      // rendering faster than the target, which is the point of negative rates.
      if (!m_isSoftLimit)
        m_deviation = NtTimerDuration::zero();
    } else {
      // Don't call sleep if the amount of time to sleep is shorter
      // than the time the function calls are likely going to take
      NtTimerDuration sleepDuration = m_targetInterval - m_deviation - frameTime;
      t1 = sleep(t1, sleepDuration);

      // Compensate for any sleep inaccuracies in the next frame, and
      // limit cumulative deviation in order to avoid stutter in case we
      // have a number of slow frames immediately followed by a fast one.
      frameTime = std::chrono::duration_cast<NtTimerDuration>(t1 - t0);
      m_deviation += frameTime - m_targetInterval;
      m_deviation = std::min(m_deviation, m_targetInterval / 16);
    }

    m_lastFrame = t1;
  }


  void FpsLimiter::delayTimeline() {
    TimePoint now = dxvk::high_resolution_clock::now();
    TimePoint target = this->nextTarget(now);

    NtTimerDuration remaining = std::chrono::duration_cast<NtTimerDuration>(target - now);

    m_nextFrame = remaining > NtTimerDuration::zero()
      ? this->sleep(now, remaining)
      : now;

    m_hasTarget = true;
    m_lastFrame = m_nextFrame;
  }


  FpsLimiter::TimePoint FpsLimiter::nextTarget(TimePoint now) {
    // A changed target interval restarts the cadence, since a deadline
    // derived from a different frame rate means nothing.
    bool restart = !m_hasTarget
                || m_targetOfLastFrame != m_targetInterval
                || m_method == FpsLimitMethod::Reactive;

    m_targetOfLastFrame = m_targetInterval;

    if (restart)
      return now + m_targetInterval;

    // Advance the cadence by exactly one interval. If the frame overran by
    // a whole interval or more the cadence is unrecoverable, so resynchronise
    // rather than issue a burst of zero-length waits trying to catch up.
    auto overshoot = std::chrono::duration_cast<NtTimerDuration>(now - m_nextFrame);

    return overshoot >= m_targetInterval
      ? now + m_targetInterval
      : m_nextFrame + m_targetInterval;
  }


  FpsLimiter::TimePoint FpsLimiter::sleep(TimePoint t0, NtTimerDuration duration) {
    if (duration <= NtTimerDuration::zero())
      return t0;

    switch (m_pacing) {
      case FpsLimitPacing::Sleep:   return this->sleepOnce(duration);
      case FpsLimitPacing::Sliced:  return this->sleepSliced(t0, duration);
      case FpsLimitPacing::Spin:    return this->spinUntil(t0, duration);
      case FpsLimitPacing::Precise: return this->sleepPrecise(t0, duration);
    }

    return this->sleepPrecise(t0, duration);
  }


  FpsLimiter::TimePoint FpsLimiter::sleepOnce(NtTimerDuration duration) {
    if (NtDelayExecution) {
      LARGE_INTEGER ticks;
      ticks.QuadPart = -duration.count();

      NtDelayExecution(FALSE, &ticks);
    } else {
      std::this_thread::sleep_for(duration);
    }

    return dxvk::high_resolution_clock::now();
  }


  FpsLimiter::TimePoint FpsLimiter::spinUntil(TimePoint t0, NtTimerDuration duration) {
    NtTimerDuration remaining = duration;
    TimePoint t1 = t0;

    while (remaining > NtTimerDuration::zero()) {
      t1 = dxvk::high_resolution_clock::now();
      remaining -= std::chrono::duration_cast<NtTimerDuration>(t1 - t0);
      t0 = t1;
    }

    return t1;
  }


  FpsLimiter::TimePoint FpsLimiter::sleepSliced(TimePoint t0, NtTimerDuration duration) {
    NtTimerDuration margin = NtTimerDuration(SliceMarginTicks);
    NtTimerDuration step   = NtTimerDuration(SliceStepTicks);

    NtTimerDuration remaining = duration;
    TimePoint t1 = t0;

    // Sleep in bounded steps and re-measure after each one, so an inaccurate
    // sleep is corrected within this frame rather than carried into the next.
    while (remaining > margin) {
      t1 = this->sleepOnce(std::min(remaining - margin, step));
      remaining -= std::chrono::duration_cast<NtTimerDuration>(t1 - t0);
      t0 = t1;
    }

    return this->spinUntil(t0, remaining);
  }


  FpsLimiter::TimePoint FpsLimiter::sleepPrecise(TimePoint t0, NtTimerDuration duration) {
    // On wine, we can rely on NtDelayExecution waiting for more or
    // less exactly the desired amount of time, and we want to avoid
    // spamming QueryPerformanceCounter for performance reasons.
    // On Windows, we busy-wait for the last couple of milliseconds
    // since sleeping is highly inaccurate and inconsistent.
    NtTimerDuration sleepThreshold = m_sleepThreshold;

    if (m_sleepGranularity != NtTimerDuration::zero())
      sleepThreshold += duration / 6;

    NtTimerDuration remaining = duration;
    TimePoint t1 = t0;

    while (remaining > sleepThreshold) {
      t1 = this->sleepOnce(remaining - sleepThreshold);
      remaining -= std::chrono::duration_cast<NtTimerDuration>(t1 - t0);
      t0 = t1;
    }

    // Busy-wait until we have slept long enough
    return this->spinUntil(t0, remaining);
  }


  void FpsLimiter::initialize() {
    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");

    if (ntdll) {
      NtDelayExecution = reinterpret_cast<NtDelayExecutionProc>(
        ::GetProcAddress(ntdll, "NtDelayExecution"));
      auto NtQueryTimerResolution = reinterpret_cast<NtQueryTimerResolutionProc>(
        ::GetProcAddress(ntdll, "NtQueryTimerResolution"));
      auto NtSetTimerResolution = reinterpret_cast<NtSetTimerResolutionProc>(
        ::GetProcAddress(ntdll, "NtSetTimerResolution"));

      ULONG min, max, cur;

      // Wine's implementation of these functions is a stub as of 6.10, which is fine
      // since it uses select() in NtDelayExecution. This is only relevant for Windows.
      if (NtQueryTimerResolution && !NtQueryTimerResolution(&min, &max, &cur)) {
        m_sleepGranularity = NtTimerDuration(cur);

        if (NtSetTimerResolution && !NtSetTimerResolution(max, TRUE, &cur)) {
          Logger::info(str::format("Setting timer interval to ", (double(max) / 10.0), " us"));
          m_sleepGranularity = NtTimerDuration(max);
        }
      }
    } else {
      // Assume 1ms sleep granularity by default
      m_sleepGranularity = NtTimerDuration(10000);
    }

    m_sleepThreshold = 4 * m_sleepGranularity;
    m_lastFrame = dxvk::high_resolution_clock::now();
    m_nextFrame = m_lastFrame;
    m_initialized = true;
  }

}
