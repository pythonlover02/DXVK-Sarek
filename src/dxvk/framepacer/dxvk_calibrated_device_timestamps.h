#pragma once

#include "../../util/util_time.h"
#include "../vulkan/vulkan_loader.h"

namespace dxvk {

  class DxvkDevice;

  /*
   * Clock synchronization for GPU/CPU via the VK_KHR_calibrated_timestamps
   * (fallback VK_EXT_calibrated_timestamps) device extension. The clocks need
   * to get calibrated regularly, for example once every frame, to account for
   * clock drift.
   *
   * Assumes 64 bit timestamps are supported for now, as lower bit timestamps
   * would need overflow checks and are impractical since 32 bit timestamps
   * would wrap back to zero every 4 seconds at nanosecond precision.
   *
   * Intended to be used within a single thread, not thread-safe.
   */

  class CalibratedDeviceTimestamps {
  public:
    using time_point = high_resolution_clock::time_point;

    CalibratedDeviceTimestamps( DxvkDevice* device );
    ~CalibratedDeviceTimestamps() { }

    void enable() { m_enabled = m_canEnable; }
    bool isEnabled() const { return m_enabled; }

    void calibrate();
    time_point getHostTimestamp( uint64_t deviceTimestamp ) const;

  private:

    struct Calibration {
      using time_point = high_resolution_clock::time_point;
      uint64_t deviceTimestamp  = 0;
      uint64_t maxDeviation     = 0;
      time_point hostTimestamp  = time_point{};
    };

    DxvkDevice* m_device;
    Calibration m_calibration;
    bool        m_enabled = false;

    PFN_vkGetCalibratedTimestampsKHR m_fpGetCalibratedTimestamps = nullptr;

    const float    m_timestampPeriod;
    const uint32_t m_timestampValidBits;
    const bool     m_canEnable;

  };


}