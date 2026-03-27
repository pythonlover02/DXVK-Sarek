#pragma once

#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <vector>
#include <array>

#include "../util/thread.h"
#include "dxvk_include.h"

namespace dxvk {

  class DxvkDevice;
  class DxvkGraphicsPipeline;
  class DxvkGraphicsPipelineStateInfo;

  enum class DxvkDyasyncPriority : uint32_t {
    Background = 0,
    Live       = 1,
  };

  struct DxvkDyasyncEntry {
    DxvkGraphicsPipeline*         pipeline  = nullptr;
    DxvkGraphicsPipelineStateInfo state;
    DxvkDyasyncPriority           priority  = DxvkDyasyncPriority::Live;
  };

  /**
   * \brief Dyasync pipeline compiler
   *
   * Compiles graphics pipelines asynchronously using
   * a separate thread pool and MPMC ring buffers.
   * Complements GPL by handling states that cannot
   * be fast-linked from pipeline libraries.
   */
  class DxvkPipelineCompiler : public RcObject {

  public:

    explicit DxvkPipelineCompiler(const DxvkDevice* device);
    ~DxvkPipelineCompiler();

    /**
     * \brief Queues a pipeline for async compilation
     *
     * \param [in] pipeline Graphics pipeline object
     * \param [in] state Pipeline state to compile
     * \param [in] priority Compilation priority
     * \returns \c true if successfully queued
     */
    bool queueCompilation(
      DxvkGraphicsPipeline*                pipeline,
      const DxvkGraphicsPipelineStateInfo& state,
      DxvkDyasyncPriority                  priority);

  private:

    static constexpr uint32_t RingCapacity = 4096;
    static constexpr uint32_t RingMask     = RingCapacity - 1;

    struct RingCell {
      std::atomic<uint32_t> sequence;
      DxvkDyasyncEntry      data;
    };

    struct MpmcRing {
      alignas(CACHE_LINE_SIZE) std::atomic<uint32_t> head{0};
      alignas(CACHE_LINE_SIZE) std::atomic<uint32_t> tail{0};
      std::array<RingCell, RingCapacity> cells;

      MpmcRing() {
        for (uint32_t i = 0; i < RingCapacity; i++)
          cells[i].sequence.store(i, std::memory_order_relaxed);
      }

      bool tryPush(DxvkDyasyncEntry&& entry) {
        uint32_t pos = tail.load(std::memory_order_relaxed);
        for (;;) {
          auto& cell = cells[pos & RingMask];
          uint32_t seq = cell.sequence.load(std::memory_order_acquire);
          int32_t diff = int32_t(seq) - int32_t(pos);
          if (diff == 0) {
            if (tail.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
              cell.data = std::move(entry);
              cell.sequence.store(pos + 1, std::memory_order_release);
              return true;
            }
          } else if (diff < 0) {
            return false;
          } else {
            pos = tail.load(std::memory_order_relaxed);
          }
        }
      }

      bool tryPop(DxvkDyasyncEntry& entry) {
        uint32_t pos = head.load(std::memory_order_relaxed);
        for (;;) {
          auto& cell = cells[pos & RingMask];
          uint32_t seq = cell.sequence.load(std::memory_order_acquire);
          int32_t diff = int32_t(seq) - int32_t(pos + 1);
          if (diff == 0) {
            if (head.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
              entry = std::move(cell.data);
              cell.sequence.store(pos + RingCapacity, std::memory_order_release);
              return true;
            }
          } else if (diff < 0) {
            return false;
          } else {
            pos = head.load(std::memory_order_relaxed);
          }
        }
      }

      bool empty() const {
        return head.load(std::memory_order_relaxed)
            >= tail.load(std::memory_order_relaxed);
      }
    };

    std::atomic<bool>           m_compilerStop{false};
    MpmcRing                    m_liveRing;
    MpmcRing                    m_backgroundRing;

    std::mutex                  m_sleepMutex;
    std::condition_variable     m_sleepCond;

    std::vector<dxvk::thread>   m_compilerThreads;
    std::atomic<uint32_t>       m_pickCounter{0};

    void runCompilerThread();

  };

}
