#include "dxvk_hud_item.h"
#include "../framepacer/dxvk_framepacer.h"

namespace dxvk::hud {

  HudRenderLatencyItem::HudRenderLatencyItem() { }
  HudRenderLatencyItem::~HudRenderLatencyItem() { }

  void HudRenderLatencyItem::update(dxvk::high_resolution_clock::time_point time) {
    const Rc<DxvkLatencyTracker> tracker = m_tracker;
    const FramePacer* framePacer = dynamic_cast<FramePacer*>( tracker.ptr() );
    if (!framePacer)
      return;

    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(time - m_lastUpdate);

    if (elapsed.count() >= UpdateInterval) {
      m_lastUpdate = time;

      LatencyMarkersReader reader = framePacer->m_latencyMarkersStorage.getReader(100);
      const LatencyMarkers* markers;
      uint32_t count = 0;
      int64_t totalLatency = 0;
      while (reader.getNext(markers)) {
        totalLatency += markers->gpuFinished;
        ++count;
      }

      if (!count)
        return;

      int64_t latency = totalLatency / count;
      m_latency = str::format(latency / 1000, ".", (latency/100) % 10, " ms");
    }
  }


  HudPos HudRenderLatencyItem::render(
    const Rc<DxvkCommandList>&ctx,
    const HudPipelineKey&     key,
    const HudOptions&         options,
          HudRenderer&        renderer,
          HudPos              position) {

    position.y += 12;
    renderer.drawText(16, position, 0xff4040ffu, "Render latency:");
    renderer.drawText(16, { position.x + 195, position.y },
      0xffffffffu, m_latency);

    position.y += 8;
    return position;
  }


  HudLatencyDetailsItem::HudLatencyDetailsItem() { }
  HudLatencyDetailsItem::~HudLatencyDetailsItem() { }

  void HudLatencyDetailsItem::update(dxvk::high_resolution_clock::time_point time) {

    const Rc<DxvkLatencyTracker> tracker = m_tracker;
    FramePacer* framePacer = dynamic_cast<FramePacer*>( tracker.ptr() );
    if (!framePacer)
      return;

    if (!framePacer->m_enableGpuBufferTracking)
      framePacer->m_enableGpuBufferTracking.store(true);
    if (!framePacer->m_enableVSyncBufferTracking && framePacer->getFramePacerMode()->getPresentMode() == VK_PRESENT_MODE_FIFO_KHR)
      framePacer->m_enableVSyncBufferTracking.store(true);

    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(time - m_lastUpdate);

    if (elapsed.count() >= UpdateInterval) {
      m_lastUpdate = time;

      const LatencyStats* gpuBufferStats = framePacer->getGpuBufferStats();
      if (gpuBufferStats) {
        int32_t p50 = gpuBufferStats->getPercentile(0.5);
        int32_t p75 = gpuBufferStats->getPercentile(0.75);
        int32_t p95 = gpuBufferStats->getPercentile(0.95);
        int32_t p99 = gpuBufferStats->getPercentile(0.99);
        m_gpuP50 = str::format(p50);
        m_gpuP75 = str::format(p75);
        m_gpuP95 = str::format(p95);
        m_gpuP99 = str::format(p99);
      }

      if (framePacer->getFramePacerMode()->getPresentMode() == VK_PRESENT_MODE_FIFO_KHR
        && (framePacer->getMode() || std::chrono::duration_cast<std::chrono::milliseconds>(
          high_resolution_clock::now() - FpsLimiter::m_lastActive.load()).count() > 3000) ) {
        const LatencyStats* presentStats = framePacer->getPresentStats();
        if (presentStats) {
          int32_t p50 = presentStats->getPercentile(0.5);
          int32_t p75 = presentStats->getPercentile(0.75);
          int32_t p95 = presentStats->getPercentile(0.95);
          int32_t p99 = presentStats->getPercentile(0.99);
          m_presentP50 = str::format(p50);
          m_presentP75 = str::format(p75);
          m_presentP95 = str::format(p95);
          m_presentP99 = str::format(p99);
        }
      } else {
        m_presentP50 = "";
      }

    }
  }


  HudPos HudLatencyDetailsItem::render(
    const Rc<DxvkCommandList>&ctx,
    const HudPipelineKey&     key,
    const HudOptions&         options,
          HudRenderer&        renderer,
          HudPos              position) {

    constexpr int w = 12;
    position.y += 12;

    renderer.drawText(16, position, 0xff40ffffu, "GPU Buffer (us):");
    position.y += 16;
    int x = 2 * w;

    renderer.drawText(16, { position.x + x, position.y }, 0xff40ffffu, "p50"); x += 4*w;
    renderer.drawText(16, { position.x + x, position.y }, 0xffffffffu, m_gpuP50); x += (m_gpuP50.size()+1)*w;
    renderer.drawText(16, { position.x + x, position.y }, 0xff40ffffu, "p75"); x += 4*w;
    renderer.drawText(16, { position.x + x, position.y }, 0xffffffffu, m_gpuP75); x += (m_gpuP75.size()+1)*w;
    renderer.drawText(16, { position.x + x, position.y }, 0xff40ffffu, "p95"); x += 4*w;
    renderer.drawText(16, { position.x + x, position.y }, 0xffffffffu, m_gpuP95); x += (m_gpuP95.size()+1)*w;
    renderer.drawText(16, { position.x + x, position.y }, 0xff40ffffu, "p99"); x += 4*w;
    renderer.drawText(16, { position.x + x, position.y }, 0xffffffffu, m_gpuP99);

    if (!m_presentP50.empty()) {
      position.y += 18;
      renderer.drawText(16, position, 0xff40ffffu, "V-Sync Buffer (us):");
      position.y += 16;
      x = 2 * w;

      renderer.drawText(16, { position.x + x, position.y }, 0xff40ffffu, "p50"); x += 4*w;
      renderer.drawText(16, { position.x + x, position.y }, 0xffffffffu, m_presentP50); x += (m_presentP50.size()+1)*w;
      renderer.drawText(16, { position.x + x, position.y }, 0xff40ffffu, "p75"); x += 4*w;
      renderer.drawText(16, { position.x + x, position.y }, 0xffffffffu, m_presentP75); x += (m_presentP75.size()+1)*w;
      renderer.drawText(16, { position.x + x, position.y }, 0xff40ffffu, "p95"); x += 4*w;
      renderer.drawText(16, { position.x + x, position.y }, 0xffffffffu, m_presentP95); x += (m_presentP95.size()+1)*w;
      renderer.drawText(16, { position.x + x, position.y }, 0xff40ffffu, "p99"); x += 4*w;
      renderer.drawText(16, { position.x + x, position.y }, 0xffffffffu, m_presentP99);
    }

    position.y += 8;
    return position;
  }


}
