#include "dxvk_calibrated_device_timestamps.h"
#include "../dxvk_device.h"
#include <vector>

namespace dxvk {

  CalibratedDeviceTimestamps::CalibratedDeviceTimestamps( DxvkDevice* device )
  : m_device(device),
    m_timestampPeriod(device->adapter()->deviceProperties().core.properties.limits.timestampPeriod),
    m_timestampValidBits(device->adapter()->getTimestampValidBits()),
    m_enabled( m_device->vki()->vkGetPhysicalDeviceCalibrateableTimeDomainsKHR != nullptr &&
               m_device->vkd()->vkGetCalibratedTimestampsKHR != nullptr &&
               m_timestampValidBits == 64 ) {

    if (m_device->vki()->vkGetPhysicalDeviceCalibrateableTimeDomainsKHR == nullptr ||
        m_device->vkd()->vkGetCalibratedTimestampsKHR == nullptr) {
      Logger::warn( "VK_KHR_calibrated_timestamps is not enabled. Frame pacing will be suboptimal." );
      return;
    }

    if (m_timestampValidBits != 64) {
      Logger::warn( str::format("VK_KHR_calibrated_timestamps is not enabled due to the device queue reporting ",
        m_timestampValidBits, " bit timestamps. Currently only implemented support for 64 bit timestamps. ",
        "Frame pacing will be suboptimal."));
      return;
    }

    uint32_t count;
    m_device->vki()->vkGetPhysicalDeviceCalibrateableTimeDomainsKHR(m_device->adapter()->handle(), &count, nullptr);
    std::vector<VkTimeDomainKHR> timeDomains( count );
    m_device->vki()->vkGetPhysicalDeviceCalibrateableTimeDomainsKHR(m_device->adapter()->handle(), &count, timeDomains.data());
    bool foundDeviceTimeDomain = false;
    for (uint32_t i=0; i<count; ++i ) {
      foundDeviceTimeDomain |= timeDomains[i] == VK_TIME_DOMAIN_DEVICE_KHR;
    }

    if (!foundDeviceTimeDomain)
      Logger::err( str::format(
        "VK_TIME_DOMAIN_DEVICE_KHR is not reported by vkGetPhysicalDeviceCalibrateableTimeDomainsKHR(), ",
        "possibly a Vulkan driver bug" ) );

    calibrate();

  }


  void CalibratedDeviceTimestamps::calibrate() {

    Calibration nextCalibration;

    // we are only interested in the device "now" timestamp via Vulkan
    // since getting the host timestamp directly proved to be more reliable
    // possibly because of how the GPU driver interacts with Wine
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


  CalibratedDeviceTimestamps::time_point CalibratedDeviceTimestamps::getHostTimestamp( uint64_t deviceTimestamp ) const {

    if (unlikely(m_calibration.deviceTimestamp == 0))
      return time_point{};

    int64_t deltaDeviceTicks = deviceTimestamp - m_calibration.deviceTimestamp;
    int64_t deltaDeviceNanoseconds = deltaDeviceTicks * m_timestampPeriod;

    return m_calibration.hostTimestamp + high_resolution_clock::nanoseconds( deltaDeviceNanoseconds );

  }


}
