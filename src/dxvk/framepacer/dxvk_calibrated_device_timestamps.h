#pragma once

#include "../../util/util_time.h"

namespace dxvk {

  class DxvkDevice;

  /*
   * todo: assumes 64 bit timestamps are supported for now
   * Intended to be used within a single thread, not thread-safe
   */

  class CalibratedDeviceTimestamps {
  public:
    using time_point = high_resolution_clock::time_point;

    CalibratedDeviceTimestamps( DxvkDevice* device );
    ~CalibratedDeviceTimestamps() { }

    void calibrate();
    time_point getHostTimestamp( uint64_t deviceTimestamp );

  private:

    struct Calibration {
      using time_point = high_resolution_clock::time_point;
      uint64_t deviceTimestamp;
      uint64_t maxDeviation;
      time_point hostTimestamp;
    };

    DxvkDevice* m_device;
    Calibration m_calibration;

    const float m_timestampPeriod;

  };


}