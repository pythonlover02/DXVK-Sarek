#include "dxvk_calibrated_device_timestamps.h"
#include "../dxvk_device.h"

namespace dxvk {

  CalibratedDeviceTimestamps::CalibratedDeviceTimestamps( DxvkDevice* device )
    : m_device(device),
      m_timestampPeriod(device->adapter()->deviceProperties().core.properties.limits.timestampPeriod) { }


    void CalibratedDeviceTimestamps::calibrate() {

      Calibration nextCalibration;

      // we are only interested in the device "now" timestamp via Vulkan
      // because getting the host timestamp directly proved to be more reliable
      VkCalibratedTimestampInfoKHR calibratedTimestampInfo;
      calibratedTimestampInfo.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
      calibratedTimestampInfo.pNext = nullptr;
      calibratedTimestampInfo.timeDomain = VK_TIME_DOMAIN_DEVICE_KHR;

      VkResult res = m_device->vkd()->vkGetCalibratedTimestampsKHR(
        m_device->handle(), 1,
        &calibratedTimestampInfo,
        &nextCalibration.deviceTimestamp,
        &nextCalibration.maxDeviation
      );

      nextCalibration.hostTimestamp = high_resolution_clock::now();

      if (unlikely(res != VK_SUCCESS)) {
        Logger::err( "Failed to calibrate timestamp" );
        return;
      }

      m_calibration = nextCalibration;

    }


    CalibratedDeviceTimestamps::time_point CalibratedDeviceTimestamps::getHostTimestamp( uint64_t deviceTimestamp ) {

      int64_t deltaDeviceTicks = deviceTimestamp - m_calibration.deviceTimestamp;
      int64_t deltaDeviceNanoseconds = deltaDeviceTicks * m_timestampPeriod;

      return m_calibration.hostTimestamp + high_resolution_clock::nanoseconds( deltaDeviceNanoseconds );

    }


}
