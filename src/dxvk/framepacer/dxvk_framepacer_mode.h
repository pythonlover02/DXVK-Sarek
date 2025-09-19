#pragma once

#include "dxvk_latency_markers.h"
#include "dxvk_frame_sync.h"
#include "../../util/sync/sync_signal.h"
#include "../../util/util_env.h"
#include "../../util/util_time.h"

namespace dxvk {

  /*
   * /brief Abstract frame pacer mode in order to support different strategies of synchronization.
   */

  class FramePacerMode {

    using time_point = high_resolution_clock::time_point;

  public:

    enum Mode {
      MAX_FRAME_LATENCY = 0,
      LOW_LATENCY,
      LOW_LATENCY_VRR,
      MIN_LATENCY
    };

    FramePacerMode( Mode mode, const char* name, LatencyMarkersStorage* markerStorage, FrameSync* frameSync, uint64_t firstFrameId )
    : m_mode( mode ),
      m_name( name ),
      m_firstFrameId( firstFrameId ),
      m_latencyMarkersStorage( markerStorage ),
      m_frameSync( frameSync ) {
      setFpsLimitFrametimeFromEnv();
    }

    virtual ~FramePacerMode() { }

    virtual void startFrame( uint64_t frameId ) { }
    virtual void endFrame( uint64_t frameId ) { }

    virtual void finishRender( uint64_t frameId ) { }

    virtual void notifyQueueSubmit( uint64_t frameId, time_point t ) { }
    virtual void notifyGpuReady( uint64_t frameId, time_point t ) { }

    virtual bool getDesiredPresentMode( uint32_t& presentMode ) const {
      return false; }

    void setPresentMode( uint32_t presentMode ) {
      m_presentMode = presentMode; }

    uint32_t getPresentMode() { return m_presentMode; }

    virtual void setTargetFrameRate( double frameRate ) {
      if (!m_fpsLimitEnvOverride && frameRate > 1.0)
        m_fpsLimitFrametime.store( 1'000'000/frameRate );
    }

    const Mode m_mode;

    const char* getName() const
      { return m_name; }

    static bool getDoubleFromEnv( const char* name, double* result );
    static bool getIntFromEnv( const char* name, int* result );

  protected:

    void setFpsLimitFrametimeFromEnv();

    const char*             m_name;
    const uint64_t          m_firstFrameId;
    LatencyMarkersStorage*  m_latencyMarkersStorage;
    FrameSync*              m_frameSync;
    std::atomic<uint32_t>   m_presentMode;
    std::atomic<int32_t>    m_fpsLimitFrametime = { 0 };
    bool                    m_fpsLimitEnvOverride = { false };

  };



  inline bool FramePacerMode::getDoubleFromEnv( const char* name, double* result ) {
    std::string env = env::getEnvVar(name);
    if (env.empty())
      return false;

    try {
      *result = std::stod(env);
      return true;
    } catch (const std::invalid_argument&) {
      return false;
    }
  }


  inline bool FramePacerMode::getIntFromEnv( const char* name, int* result ) {
    std::string env = env::getEnvVar(name);
    if (env.empty())
      return false;

    try {
      *result = std::stoi(env);
      return true;
    } catch (const std::invalid_argument&) {
      return false;
    }
  }


  inline void FramePacerMode::setFpsLimitFrametimeFromEnv() {
    double fpsLimit;
    if (!getDoubleFromEnv("DXVK_FRAME_RATE", &fpsLimit))
      return;

    m_fpsLimitEnvOverride = true;
    if (fpsLimit < 1.0)
      return;

    m_fpsLimitFrametime = 1'000'000/fpsLimit;
  }

}
