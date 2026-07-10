#pragma once

#include <array>
#include <optional>
#include <vector>

#include "dxvk_adapter.h"

namespace dxvk {

  class DxvkMemory;
  class DxvkMemoryAllocator;
  class DxvkMemoryChunk;
  struct DxvkMemoryType;

  /**
   * \brief Memory allocation strategy
   *
   * Selects which allocator services requests that
   * the legacy chunk allocator would otherwise handle.
   */
  enum class DxvkMemoryStrategy : uint32_t {
    Legacy = 0,
    Paged  = 1,
  };

  /**
   * \brief Memory allocation owner
   *
   * Identifies which allocator produced a given \c DxvkMemory
   * slice, so freeing it can be routed back to the right place.
   */
  enum class DxvkMemoryOwner : uint32_t {
    Dedicated = 0,
    Chunk     = 1,
    Pool      = 2,
    SmallPool = 3,
  };

  // Ceiling is PAGE_SIZE / 2: beyond that a slab would only ever
  // hold one slot per page, which is just the page-tier allocator
  // with extra indirection and no pooling benefit. Matches upstream's
  // 32 KiB pool ceiling (dxvk/dxvk#4280) as a consequence of this,
  // not by copying their number - their page size differs from ours.
  constexpr uint32_t DxvkMemorySmallClassCount = 8;

  struct DxvkMemoryPageRange {
    uint32_t chunkIndex;
    uint32_t pageIndex;
    uint32_t pageCount;
  };

  /**
   * \brief Small allocation slab
   *
   * One 64 KiB page carved into fixed-size slots
   * for a single size class.
   */
  struct DxvkMemorySlab {
    uint32_t              chunkIndex;
    uint32_t              pageIndex;
    uint32_t              slotSize;
    std::vector<uint32_t> freeSlots;
  };

  struct DxvkMemoryPoolType {
    std::vector<Rc<DxvkMemoryChunk>>                                   chunks;
    std::vector<DxvkMemoryPageRange>                                   freeRanges;
    std::array<std::vector<DxvkMemorySlab>, DxvkMemorySmallClassCount> slabs;
  };

  struct DxvkMemoryPoolTypeStats {
    uint32_t chunkCount = 0;
    uint32_t pagesTotal = 0;
    uint32_t pagesFree  = 0;
  };

  using DxvkMemoryPoolStats = std::array<DxvkMemoryPoolTypeStats, VK_MAX_MEMORY_TYPES>;

  /**
   * \brief Paged memory allocator
   *
   * Services allocations that fit within a bounded chunk size using
   * a page-granular best-fit free list, with a size-classed slab
   * tier in front of it for small, frequently churned allocations
   * such as constant buffers. Anything it can't service is expected
   * to fall back to the legacy chunk allocator at the call site.
   *
   * Not thread-safe on its own - every entry point relies on the
   * caller already holding DxvkMemoryAllocator's allocation lock.
   */
  class DxvkMemoryPool {

  public:

    explicit DxvkMemoryPool(DxvkMemoryAllocator* alloc);
    ~DxvkMemoryPool();

    DxvkMemory alloc(
            DxvkMemoryType*                   type,
            VkMemoryPropertyFlags             flags,
            VkDeviceSize                      size,
            VkDeviceSize                      align,
      const VkMemoryDedicatedAllocateInfo*    dedAllocInfo);

    void free(
            DxvkMemoryType*   type,
            DxvkMemoryChunk*  chunk,
            VkDeviceSize      offset,
            VkDeviceSize      length);

    void freeSmall(
            DxvkMemoryType*   type,
            DxvkMemoryChunk*  chunk,
            VkDeviceSize      offset,
            VkDeviceSize      length);

    DxvkMemoryPoolStats getStats() const;

  private:

    DxvkMemoryAllocator*                                m_alloc;
    VkDeviceSize                                         m_maxChunkSize;
    std::array<DxvkMemoryPoolType, VK_MAX_MEMORY_TYPES>  m_types;

    DxvkMemory allocFromPool(
            DxvkMemoryType*                   type,
            VkMemoryPropertyFlags             flags,
            VkDeviceSize                      size,
            VkDeviceSize                      align);

    DxvkMemory allocSmall(
            DxvkMemoryType*                   type,
            VkMemoryPropertyFlags             flags,
            uint32_t                          classIndex);

    std::optional<DxvkMemoryPageRange> acquirePage(
            DxvkMemoryPoolType&    pool,
            DxvkMemoryType*        type,
            VkMemoryPropertyFlags  flags);

    std::optional<size_t> growPool(
            DxvkMemoryPoolType&    pool,
            DxvkMemoryType*        type,
            VkMemoryPropertyFlags  flags,
            uint32_t               pagesNeeded);

    bool growSlab(
            DxvkMemoryPoolType&    pool,
            DxvkMemoryType*        type,
            VkMemoryPropertyFlags  flags,
            uint32_t               classIndex);

    DxvkMemoryPageRange takePages(
            DxvkMemoryPoolType&    pool,
            size_t                 slot,
            uint32_t               pagesNeeded);

    DxvkMemory takeRange(
            DxvkMemoryPoolType&    pool,
            DxvkMemoryType*        type,
            size_t                 slot,
            uint32_t               pagesNeeded,
            VkDeviceSize           size);

    DxvkMemory takeSlot(
            DxvkMemoryPoolType&    pool,
            DxvkMemoryType*        type,
            uint32_t               classIndex,
            uint32_t               slabIndex);

    size_t insertRange(
            DxvkMemoryPoolType&    pool,
            DxvkMemoryPageRange    range);

    DxvkMemoryPageRange coalesce(
            DxvkMemoryPoolType&    pool,
            DxvkMemoryPageRange    range);

    uint32_t indexOfChunk(
      const DxvkMemoryPoolType&    pool,
      const DxvkMemoryChunk*       chunk) const;

    void releaseChunk(
            DxvkMemoryPoolType&    pool,
            uint32_t               chunkIndex);

    void releaseSlab(
            DxvkMemoryPoolType&    pool,
            uint32_t               classIndex,
            uint32_t               slabIndex);

    bool shouldReleaseChunk(
      const DxvkMemoryPoolType&    pool,
            DxvkMemoryType*        type) const;

  };

}
