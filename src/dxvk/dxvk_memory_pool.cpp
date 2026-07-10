#include <algorithm>
#include <cstring>
#include <numeric>

#include "dxvk_device.h"
#include "dxvk_memory.h"
#include "dxvk_memory_pool.h"

namespace dxvk {

  namespace {

    // 64 KiB granularity matches the free-list bucket size upstream
    // settled on in dxvk/dxvk#4280, chosen to satisfy most texture
    // alignment requirements without excessive free-list overhead.
    constexpr VkDeviceSize PAGE_SIZE            = 64ull  << 10;
    constexpr VkDeviceSize MIN_CHUNK_SIZE       = 4ull   << 20;
    constexpr VkDeviceSize MAX_CHUNK_SIZE       = 256ull << 20;
    // Same 32-bit address space concern as the reduced chunk size in
    // the legacy allocator's pickChunkSize (dxvk_memory.cpp).
    constexpr VkDeviceSize MAX_CHUNK_SIZE_32BIT = 64ull  << 20;
    constexpr VkDeviceSize CHUNK_GROWTH_DIVISOR = 4ull;
    constexpr VkDeviceSize MIN_CHUNKS_PER_HEAP  = 15ull;

    // Padded-power-of-two size classes for the slab tier, same
    // rationale upstream gives for its own pool ("the pool allocator
    // works on powers of two, so buffers should too", dxvk#4280).
    // See header comment for why 32 KiB (PAGE_SIZE / 2) is the ceiling.
    constexpr std::array<VkDeviceSize, DxvkMemorySmallClassCount> SMALL_ALLOC_CLASS_SIZES = {
      256ull, 512ull, 1024ull, 2048ull, 4096ull, 8192ull, 16384ull, 32768ull,
    };

    uint32_t pages_for_size(VkDeviceSize size) {
      return uint32_t((size + PAGE_SIZE - 1) / PAGE_SIZE);
    }

    // Scales with allocated_bytes rather than a fixed schedule, same
    // rationale as DXVK 2.4.1 removing dxvk.maxChunkSize in favor of
    // sizing chunks off how much the app has already allocated.
    VkDeviceSize next_chunk_size(VkDeviceSize allocated_bytes, VkDeviceSize heap_size, VkDeviceSize max_chunk_size) {
      VkDeviceSize target = std::clamp(allocated_bytes / CHUNK_GROWTH_DIVISOR, MIN_CHUNK_SIZE, max_chunk_size);
      return std::max(PAGE_SIZE, std::min(target, heap_size / MIN_CHUNKS_PER_HEAP));
    }

    bool is_poolable(VkDeviceSize size, VkDeviceSize align, const VkMemoryDedicatedAllocateInfo* ded_alloc_info, VkDeviceSize max_chunk_size) {
      return !ded_alloc_info && align <= PAGE_SIZE && size <= max_chunk_size;
    }

    bool is_chunk_free(const DxvkMemoryPoolType& pool, DxvkMemoryPageRange range) {
      return range.pageIndex == 0
          && range.pageCount == pages_for_size(pool.chunks[range.chunkIndex]->handle().memSize);
    }

    std::optional<size_t> find_fit(const std::vector<DxvkMemoryPageRange>& ranges, uint32_t pages_needed) {
      auto it = std::lower_bound(ranges.begin(), ranges.end(), pages_needed,
        [] (const DxvkMemoryPageRange& range, uint32_t pages) { return range.pageCount < pages; });

      return it != ranges.end() ? std::optional<size_t>(it - ranges.begin()) : std::nullopt;
    }

    // Smallest class covering both size and alignment: slot offsets
    // are k * slotSize on a page-aligned base, which only guarantees
    // the requested alignment when align <= slotSize.
    std::optional<uint32_t> find_small_class(VkDeviceSize size, VkDeviceSize align) {
      VkDeviceSize required = std::max(size, align);

      auto it = std::find_if(SMALL_ALLOC_CLASS_SIZES.begin(), SMALL_ALLOC_CLASS_SIZES.end(),
        [required] (VkDeviceSize classSize) { return classSize >= required; });

      return it != SMALL_ALLOC_CLASS_SIZES.end()
        ? std::optional<uint32_t>(uint32_t(it - SMALL_ALLOC_CLASS_SIZES.begin()))
        : std::nullopt;
    }

  }


  DxvkMemoryPool::DxvkMemoryPool(DxvkMemoryAllocator* alloc)
  : m_alloc        (alloc),
    m_maxChunkSize (env::is32BitHostPlatform() ? MAX_CHUNK_SIZE_32BIT : MAX_CHUNK_SIZE) { }


  DxvkMemoryPool::~DxvkMemoryPool() { }


  DxvkMemory DxvkMemoryPool::alloc(
          DxvkMemoryType*                   type,
          VkMemoryPropertyFlags             flags,
          VkDeviceSize                      size,
          VkDeviceSize                      align,
    const VkMemoryDedicatedAllocateInfo*    dedAllocInfo) {
    switch (is_poolable(size, align, dedAllocInfo, m_maxChunkSize)) {
      case true:  break;
      case false: return DxvkMemory();
    }

    std::optional<uint32_t> smallClass = find_small_class(size, align);

    return smallClass
      ? this->allocSmall(type, flags, *smallClass)
      : this->allocFromPool(type, flags, size, align);
  }


  DxvkMemory DxvkMemoryPool::allocFromPool(
          DxvkMemoryType*                   type,
          VkMemoryPropertyFlags             flags,
          VkDeviceSize                      size,
          VkDeviceSize                      align) {
    DxvkMemoryPoolType& pool = m_types[type->memTypeId];
    uint32_t pagesNeeded = pages_for_size(size);

    std::optional<size_t> slot = find_fit(pool.freeRanges, pagesNeeded);
    slot = slot ? slot : this->growPool(pool, type, flags, pagesNeeded);

    return slot
      ? this->takeRange(pool, type, *slot, pagesNeeded, size)
      : DxvkMemory();
  }


  DxvkMemory DxvkMemoryPool::allocSmall(
          DxvkMemoryType*                   type,
          VkMemoryPropertyFlags             flags,
          uint32_t                          classIndex) {
    DxvkMemoryPoolType& pool = m_types[type->memTypeId];
    std::vector<DxvkMemorySlab>& slabs = pool.slabs[classIndex];

    auto hasRoom = [] (const DxvkMemorySlab& slab) { return !slab.freeSlots.empty(); };

    bool ready = std::any_of(slabs.begin(), slabs.end(), hasRoom)
              || this->growSlab(pool, type, flags, classIndex);

    auto slabIt = std::find_if(slabs.begin(), slabs.end(), hasRoom);

    return ready && slabIt != slabs.end()
      ? this->takeSlot(pool, type, classIndex, uint32_t(slabIt - slabs.begin()))
      : DxvkMemory();
  }


  std::optional<DxvkMemoryPageRange> DxvkMemoryPool::acquirePage(
          DxvkMemoryPoolType&    pool,
          DxvkMemoryType*        type,
          VkMemoryPropertyFlags  flags) {
    std::optional<size_t> slot = find_fit(pool.freeRanges, 1);
    slot = slot ? slot : this->growPool(pool, type, flags, 1);

    return slot
      ? std::optional<DxvkMemoryPageRange>(this->takePages(pool, *slot, 1))
      : std::nullopt;
  }


  std::optional<size_t> DxvkMemoryPool::growPool(
          DxvkMemoryPoolType&    pool,
          DxvkMemoryType*        type,
          VkMemoryPropertyFlags  flags,
          uint32_t               pagesNeeded) {
    VkDeviceSize chunkSize = std::max(
      next_chunk_size(type->heap->stats.memoryAllocated, type->heap->properties.size, m_maxChunkSize),
      VkDeviceSize(pagesNeeded) * PAGE_SIZE);

    DxvkDeviceMemory devMem = m_alloc->tryAllocDeviceMemory(
      type, flags, chunkSize, DxvkMemoryFlags(), nullptr);

    switch (devMem.memHandle != VK_NULL_HANDLE) {
      case true:  break;
      case false: return std::nullopt;
    }

    pool.chunks.push_back(new DxvkMemoryChunk(m_alloc, type, devMem, DxvkMemoryFlags()));

    uint32_t chunkIndex = uint32_t(pool.chunks.size() - 1);
    uint32_t chunkPages = uint32_t(chunkSize / PAGE_SIZE);

    return std::optional<size_t>(this->insertRange(pool,
      DxvkMemoryPageRange { chunkIndex, 0, chunkPages }));
  }


  bool DxvkMemoryPool::growSlab(
          DxvkMemoryPoolType&    pool,
          DxvkMemoryType*        type,
          VkMemoryPropertyFlags  flags,
          uint32_t               classIndex) {
    std::optional<DxvkMemoryPageRange> page = this->acquirePage(pool, type, flags);

    switch (page.has_value()) {
      case true:  break;
      case false: return false;
    }

    VkDeviceSize slotSize  = SMALL_ALLOC_CLASS_SIZES[classIndex];
    uint32_t     slotCount = uint32_t(PAGE_SIZE / slotSize);

    std::vector<uint32_t> freeSlots(slotCount);
    std::iota(freeSlots.begin(), freeSlots.end(), 0);

    pool.slabs[classIndex].push_back(DxvkMemorySlab {
      page->chunkIndex, page->pageIndex, uint32_t(slotSize), std::move(freeSlots) });

    return true;
  }


  DxvkMemoryPageRange DxvkMemoryPool::takePages(
          DxvkMemoryPoolType&    pool,
          size_t                 slot,
          uint32_t               pagesNeeded) {
    DxvkMemoryPageRange range = pool.freeRanges[slot];
    pool.freeRanges.erase(pool.freeRanges.begin() + slot);

    uint32_t leftoverPages = range.pageCount - pagesNeeded;

    switch (leftoverPages > 0) {
      case true:
        this->insertRange(pool, DxvkMemoryPageRange {
          range.chunkIndex, range.pageIndex + pagesNeeded, leftoverPages });
        break;
      case false:
        break;
    }

    return DxvkMemoryPageRange { range.chunkIndex, range.pageIndex, pagesNeeded };
  }


  DxvkMemory DxvkMemoryPool::takeRange(
          DxvkMemoryPoolType&    pool,
          DxvkMemoryType*        type,
          size_t                 slot,
          uint32_t               pagesNeeded,
          VkDeviceSize           size) {
    DxvkMemoryPageRange range = this->takePages(pool, slot, pagesNeeded);

    const Rc<DxvkMemoryChunk>& chunk  = pool.chunks[range.chunkIndex];
    const DxvkDeviceMemory&    devMem = chunk->handle();
    VkDeviceSize                offset = VkDeviceSize(range.pageIndex) * PAGE_SIZE;
    void*                       mapPtr = devMem.memPointer
      ? reinterpret_cast<char*>(devMem.memPointer) + offset
      : nullptr;

    switch (mapPtr != nullptr && m_alloc->zeroMappedMemory()) {
      case true:  std::memset(mapPtr, 0, size); break;
      case false: break;
    }

    return DxvkMemory(m_alloc, chunk.ptr(), type,
      devMem.memHandle, offset, size, mapPtr, DxvkMemoryOwner::Pool);
  }


  DxvkMemory DxvkMemoryPool::takeSlot(
          DxvkMemoryPoolType&    pool,
          DxvkMemoryType*        type,
          uint32_t               classIndex,
          uint32_t               slabIndex) {
    DxvkMemorySlab& slab = pool.slabs[classIndex][slabIndex];

    uint32_t slot = slab.freeSlots.back();
    slab.freeSlots.pop_back();

    const Rc<DxvkMemoryChunk>& chunk  = pool.chunks[slab.chunkIndex];
    const DxvkDeviceMemory&    devMem = chunk->handle();

    VkDeviceSize offset = VkDeviceSize(slab.pageIndex) * PAGE_SIZE + VkDeviceSize(slot) * slab.slotSize;
    void*        mapPtr = devMem.memPointer
      ? reinterpret_cast<char*>(devMem.memPointer) + offset
      : nullptr;

    switch (mapPtr != nullptr && m_alloc->zeroMappedMemory()) {
      case true:  std::memset(mapPtr, 0, slab.slotSize); break;
      case false: break;
    }

    return DxvkMemory(m_alloc, chunk.ptr(), type,
      devMem.memHandle, offset, slab.slotSize, mapPtr, DxvkMemoryOwner::SmallPool);
  }


  size_t DxvkMemoryPool::insertRange(
          DxvkMemoryPoolType&    pool,
          DxvkMemoryPageRange    range) {
    auto it = std::upper_bound(pool.freeRanges.begin(), pool.freeRanges.end(), range.pageCount,
      [] (uint32_t pages, const DxvkMemoryPageRange& r) { return pages < r.pageCount; });

    return pool.freeRanges.insert(it, range) - pool.freeRanges.begin();
  }


  void DxvkMemoryPool::free(
          DxvkMemoryType*   type,
          DxvkMemoryChunk*  chunk,
          VkDeviceSize      offset,
          VkDeviceSize      length) {
    DxvkMemoryPoolType& pool = m_types[type->memTypeId];

    DxvkMemoryPageRange merged = this->coalesce(pool, DxvkMemoryPageRange {
      this->indexOfChunk(pool, chunk),
      uint32_t(offset / PAGE_SIZE),
      pages_for_size(length),
    });

    switch (is_chunk_free(pool, merged) && this->shouldReleaseChunk(pool, type)) {
      case true:  this->releaseChunk(pool, merged.chunkIndex); break;
      case false: this->insertRange(pool, merged); break;
    }
  }


  void DxvkMemoryPool::freeSmall(
          DxvkMemoryType*   type,
          DxvkMemoryChunk*  chunk,
          VkDeviceSize      offset,
          VkDeviceSize      length) {
    DxvkMemoryPoolType& pool = m_types[type->memTypeId];

    // length is always a class's slotSize here (see takeSlot) - this
    // recovers the class index without threading extra state through
    // DxvkMemory just for this one path.
    auto classIt = std::find(SMALL_ALLOC_CLASS_SIZES.begin(), SMALL_ALLOC_CLASS_SIZES.end(), length);
    uint32_t classIndex = uint32_t(classIt - SMALL_ALLOC_CLASS_SIZES.begin());

    uint32_t chunkIndex = this->indexOfChunk(pool, chunk);
    uint32_t pageIndex  = uint32_t(offset / PAGE_SIZE);
    uint32_t slotIndex  = uint32_t((offset % PAGE_SIZE) / length);

    std::vector<DxvkMemorySlab>& slabs = pool.slabs[classIndex];

    auto slabIt = std::find_if(slabs.begin(), slabs.end(),
      [chunkIndex, pageIndex] (const DxvkMemorySlab& slab) {
        return slab.chunkIndex == chunkIndex && slab.pageIndex == pageIndex;
      });

    slabIt->freeSlots.push_back(slotIndex);

    uint32_t slabIndex = uint32_t(slabIt - slabs.begin());
    bool     slabEmpty = slabIt->freeSlots.size() == PAGE_SIZE / slabIt->slotSize;

    switch (slabEmpty && this->shouldReleaseChunk(pool, type)) {
      case true:  this->releaseSlab(pool, classIndex, slabIndex); break;
      case false: break;
    }
  }


  DxvkMemoryPageRange DxvkMemoryPool::coalesce(
          DxvkMemoryPoolType&    pool,
          DxvkMemoryPageRange    range) {
    auto adjacent = [range] (const DxvkMemoryPageRange& r) {
      return r.chunkIndex == range.chunkIndex
          && (r.pageIndex + r.pageCount == range.pageIndex
           || range.pageIndex + range.pageCount == r.pageIndex);
    };

    auto it = std::find_if(pool.freeRanges.begin(), pool.freeRanges.end(), adjacent);

    switch (it != pool.freeRanges.end()) {
      case true: {
        DxvkMemoryPageRange merged = {
          range.chunkIndex,
          std::min(range.pageIndex, it->pageIndex),
          range.pageCount + it->pageCount,
        };

        pool.freeRanges.erase(it);
        return this->coalesce(pool, merged);
      }
      case false:
        break;
    }

    return range;
  }


  void DxvkMemoryPool::releaseChunk(
          DxvkMemoryPoolType&    pool,
          uint32_t               chunkIndex) {
    uint32_t lastIndex = uint32_t(pool.chunks.size() - 1);

    switch (chunkIndex != lastIndex) {
      case true:  pool.chunks[chunkIndex] = std::move(pool.chunks[lastIndex]); break;
      case false: break;
    }

    // Chunk indices are positional, so the swap-remove above can
    // invalidate any range or slab that pointed at the old last
    // slot - both need the same fix-up, not just freeRanges.
    auto remapChunk = [chunkIndex, lastIndex] (uint32_t& idx) {
      idx = idx == lastIndex ? chunkIndex : idx;
    };

    std::for_each(pool.freeRanges.begin(), pool.freeRanges.end(),
      [&remapChunk] (DxvkMemoryPageRange& range) { remapChunk(range.chunkIndex); });

    std::for_each(pool.slabs.begin(), pool.slabs.end(),
      [&remapChunk] (std::vector<DxvkMemorySlab>& slabsOfClass) {
        std::for_each(slabsOfClass.begin(), slabsOfClass.end(),
          [&remapChunk] (DxvkMemorySlab& slab) { remapChunk(slab.chunkIndex); });
      });

    pool.chunks.pop_back();
  }


  void DxvkMemoryPool::releaseSlab(
          DxvkMemoryPoolType&    pool,
          uint32_t               classIndex,
          uint32_t               slabIndex) {
    DxvkMemorySlab slab = pool.slabs[classIndex][slabIndex];
    uint32_t lastIndex = uint32_t(pool.slabs[classIndex].size() - 1);

    switch (slabIndex != lastIndex) {
      case true:  pool.slabs[classIndex][slabIndex] = std::move(pool.slabs[classIndex][lastIndex]); break;
      case false: break;
    }

    pool.slabs[classIndex].pop_back();

    // Hand the page back to the page-level free list; if that makes
    // the whole chunk free, this composes with the same release path
    // free() uses instead of duplicating chunk teardown here.
    DxvkMemoryPageRange merged = this->coalesce(pool,
      DxvkMemoryPageRange { slab.chunkIndex, slab.pageIndex, 1 });

    switch (is_chunk_free(pool, merged)) {
      case true:  this->releaseChunk(pool, merged.chunkIndex); break;
      case false: this->insertRange(pool, merged); break;
    }
  }


  bool DxvkMemoryPool::shouldReleaseChunk(
    const DxvkMemoryPoolType&    pool,
          DxvkMemoryType*        type) const {
    // Mirrors the legacy allocator's shouldFreeChunk cushion
    // (dxvk_memory.cpp): keep a handful of already-empty chunks
    // around instead of freeing immediately, so alloc/free churn
    // right at a chunk boundary doesn't bounce vkAllocateMemory /
    // vkFreeMemory back to back.
    uint32_t emptyChunks = uint32_t(std::count_if(pool.freeRanges.begin(), pool.freeRanges.end(),
      [&pool] (const DxvkMemoryPageRange& range) { return is_chunk_free(pool, range); }));

    uint32_t maxEmptyChunks = env::is32BitHostPlatform() ? 2u : 4u;

    return emptyChunks >= maxEmptyChunks
        || m_alloc->shouldFreeEmptyChunks(type->heap, 0);
  }


  uint32_t DxvkMemoryPool::indexOfChunk(
    const DxvkMemoryPoolType&    pool,
    const DxvkMemoryChunk*       chunk) const {
    auto it = std::find_if(pool.chunks.begin(), pool.chunks.end(),
      [chunk] (const Rc<DxvkMemoryChunk>& c) { return c.ptr() == chunk; });

    return uint32_t(it - pool.chunks.begin());
  }


  DxvkMemoryPoolStats DxvkMemoryPool::getStats() const {
    std::lock_guard<dxvk::mutex> lock(m_alloc->m_mutex);

    DxvkMemoryPoolStats stats = {};

    for (uint32_t i = 0; i < VK_MAX_MEMORY_TYPES; i++) {
      const DxvkMemoryPoolType& pool = m_types[i];

      stats[i].chunkCount = uint32_t(pool.chunks.size());

      stats[i].pagesTotal = std::accumulate(pool.chunks.begin(), pool.chunks.end(), uint32_t(0),
        [] (uint32_t sum, const Rc<DxvkMemoryChunk>& chunk) { return sum + pages_for_size(chunk->handle().memSize); });

      stats[i].pagesFree = std::accumulate(pool.freeRanges.begin(), pool.freeRanges.end(), uint32_t(0),
        [] (uint32_t sum, const DxvkMemoryPageRange& range) { return sum + range.pageCount; });
    }

    return stats;
  }

}
