#include <algorithm>
#include <cstring>
#include <iomanip>
#include <sstream>

#include "../util/util_bit.h"

#include "dxvk_device.h"
#include "dxvk_memory.h"

namespace dxvk {

  DxvkMemory::DxvkMemory() { }
  DxvkMemory::DxvkMemory(
          DxvkMemoryAllocator*  alloc,
          DxvkMemoryChunk*      chunk,
          DxvkMemoryType*       type,
          VkDeviceMemory        memory,
          VkDeviceSize          offset,
          VkDeviceSize          length,
          void*                 mapPtr)
  : m_alloc   (alloc),
    m_chunk   (chunk),
    m_type    (type),
    m_memory  (memory),
    m_offset  (offset),
    m_length  (length),
    m_mapPtr  (mapPtr) { }


  DxvkMemory::DxvkMemory(DxvkMemory&& other)
  : m_alloc   (std::exchange(other.m_alloc,  nullptr)),
    m_chunk   (std::exchange(other.m_chunk,  nullptr)),
    m_type    (std::exchange(other.m_type,   nullptr)),
    m_memory  (std::exchange(other.m_memory, VkDeviceMemory(VK_NULL_HANDLE))),
    m_offset  (std::exchange(other.m_offset, 0)),
    m_length  (std::exchange(other.m_length, 0)),
    m_mapPtr  (std::exchange(other.m_mapPtr, nullptr)) { }


  DxvkMemory& DxvkMemory::operator = (DxvkMemory&& other) {
    this->free();
    m_alloc   = std::exchange(other.m_alloc,  nullptr);
    m_chunk   = std::exchange(other.m_chunk,  nullptr);
    m_type    = std::exchange(other.m_type,   nullptr);
    m_memory  = std::exchange(other.m_memory, VkDeviceMemory(VK_NULL_HANDLE));
    m_offset  = std::exchange(other.m_offset, 0);
    m_length  = std::exchange(other.m_length, 0);
    m_mapPtr  = std::exchange(other.m_mapPtr, nullptr);
    return *this;
  }


  DxvkMemory::~DxvkMemory() {
    this->free();
  }


  void DxvkMemory::free() {
    if (m_alloc != nullptr)
      m_alloc->free(*this);
  }


  DxvkMemoryChunk::DxvkMemoryChunk(
          DxvkMemoryAllocator*  alloc,
          DxvkMemoryType*       type,
          DxvkDeviceMemory      memory,
          DxvkMemoryFlags       hints)
  : m_alloc(alloc), m_type(type), m_memory(memory), m_hints(hints) {
    // Mark the entire chunk as free
    m_freeList.push_back(FreeSlice { 0, memory.memSize });
  }


  DxvkMemoryChunk::~DxvkMemoryChunk() {
    // This call is technically not thread-safe, but it
    // doesn't need to be since we don't free chunks
    m_alloc->freeDeviceMemory(m_type, m_memory);
  }


  DxvkMemory DxvkMemoryChunk::alloc(
          VkMemoryPropertyFlags flags,
          VkDeviceSize          size,
          VkDeviceSize          align,
          DxvkMemoryFlags       hints) {
    // Property flags must be compatible. This could
    // be refined a bit in the future if necessary.
    if (m_memory.memFlags != flags || !checkHints(hints))
      return DxvkMemory();

    // If the chunk is full, return
    if (m_freeList.size() == 0)
      return DxvkMemory();

    // Select the slice to allocate from in a worst-fit
    // manner. This may help keep fragmentation low.
    auto bestSlice = m_freeList.begin();

    for (auto slice = m_freeList.begin(); slice != m_freeList.end(); slice++) {
      if (slice->length == size) {
        bestSlice = slice;
        break;
      } else if (slice->length > bestSlice->length) {
        bestSlice = slice;
      }
    }

    // We need to align the allocation to the requested alignment
    const VkDeviceSize sliceStart = bestSlice->offset;
    const VkDeviceSize sliceEnd   = bestSlice->offset + bestSlice->length;

    const VkDeviceSize allocStart = dxvk::align(sliceStart,        align);
    const VkDeviceSize allocEnd   = dxvk::align(allocStart + size, align);

    if (allocEnd > sliceEnd)
      return DxvkMemory();

    // We can use this slice, but we'll have to add
    // the unused parts of it back to the free list.
    m_freeList.erase(bestSlice);

    if (allocStart != sliceStart)
      m_freeList.push_back({ sliceStart, allocStart - sliceStart });

    if (allocEnd != sliceEnd)
      m_freeList.push_back({ allocEnd, sliceEnd - allocEnd });

    // Create the memory object with the aligned slice
    void* mapPtr = m_memory.memPointer
      ? reinterpret_cast<char*>(m_memory.memPointer) + allocStart
      : nullptr;

    // Some games assume freshly mapped buffers are zero-initialized and
    // break on stale data. Clear the slice on hand-out if requested, which
    // also covers reused slices from recycled chunks.
    if (unlikely(mapPtr && m_alloc->zeroMappedMemory()))
      std::memset(mapPtr, 0, allocEnd - allocStart);

    return DxvkMemory(m_alloc, this, m_type,
      m_memory.memHandle, allocStart, allocEnd - allocStart, mapPtr);
  }


  void DxvkMemoryChunk::free(
          VkDeviceSize  offset,
          VkDeviceSize  length) {
    // Remove adjacent entries from the free list and then add
    // a new slice that covers all those entries. Without doing
    // so, the slice could not be reused for larger allocations.
    auto curr = m_freeList.begin();

    while (curr != m_freeList.end()) {
      if (curr->offset == offset + length) {
        length += curr->length;
        curr = m_freeList.erase(curr);
      } else if (curr->offset + curr->length == offset) {
        offset -= curr->length;
        length += curr->length;
        curr = m_freeList.erase(curr);
      } else {
        curr++;
      }
    }

    m_freeList.push_back({ offset, length });
  }


  bool DxvkMemoryChunk::isEmpty() const {
    return m_freeList.size() == 1
        && m_freeList[0].length == m_memory.memSize;
  }


  bool DxvkMemoryChunk::isCompatible(const Rc<DxvkMemoryChunk>& other) const {
    return other->m_memory.memFlags == m_memory.memFlags && other->m_hints == m_hints;
  }


  bool DxvkMemoryChunk::checkHints(DxvkMemoryFlags hints) const {
    DxvkMemoryFlags mask(
      DxvkMemoryFlag::Small,
      DxvkMemoryFlag::GpuReadable,
      DxvkMemoryFlag::GpuWritable,
      DxvkMemoryFlag::Transient);

    if (hints.test(DxvkMemoryFlag::IgnoreConstraints))
      mask = DxvkMemoryFlags();

    return (m_hints & mask) == (hints & mask);
  }


  DxvkMemoryAllocator::DxvkMemoryAllocator(const DxvkDevice* device)
  : m_vkd             (device->vkd()),
    m_device          (device),
    m_devProps        (device->adapter()->deviceProperties()),
    m_memProps        (device->adapter()->memoryProperties()) {
    VkDeviceSize maxBudget = m_device->config().maxMemoryBudget;

    for (uint32_t i = 0; i < m_memProps.memoryHeapCount; i++) {
      m_memHeaps[i].properties = m_memProps.memoryHeaps[i];
      m_memHeaps[i].stats      = DxvkMemoryStats { 0, 0 };
      m_memHeaps[i].budget     = 0;

      /* Match upstream: only enforce a soft heap budget on discrete GPUs,
       * where VRAM is a hard ceiling. On UMA, the OS manages memory pressure
       * across CPU and GPU usage, and a fixed percentage cap causes spurious
       * allocation failures in games with large working sets (e.g. Cris Tales
       * on Intel HD Graphics). The driver reports real pressure via
       * VK_EXT_memory_budget when it matters. */
      if ((m_memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
       && !m_device->isUnifiedMemoryArchitecture()
       && m_device->properties().core.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        m_memHeaps[i].budget = (8 * m_memProps.memoryHeaps[i].size) / 10;

      if (maxBudget && (!m_memHeaps[i].budget || m_memHeaps[i].budget > maxBudget))
        m_memHeaps[i].budget = maxBudget;
    }

    for (uint32_t i = 0; i < m_memProps.memoryTypeCount; i++) {
      m_memTypes[i].heap       = &m_memHeaps[m_memProps.memoryTypes[i].heapIndex];
      m_memTypes[i].heapId     = m_memProps.memoryTypes[i].heapIndex;
      m_memTypes[i].memType    = m_memProps.memoryTypes[i];
      m_memTypes[i].memTypeId  = i;
      m_memTypes[i].chunkSize  = MinChunkSize;
    }

    /* Check what kind of heap the HVV memory type is on, if any. If the
     * HVV memory type is on the largest device-local heap, we either have
     * an UMA system or an RBAR-enabled system. Otherwise, there will likely
     * be a separate, smaller heap for it. */
    VkDeviceSize largestDeviceLocalHeap = 0;

    for (uint32_t i = 0; i < m_memProps.memoryTypeCount; i++) {
      if (m_memTypes[i].memType.propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
        largestDeviceLocalHeap = std::max(largestDeviceLocalHeap, m_memTypes[i].heap->properties.size);
    }

    /* Work around an issue on Nvidia drivers where using the entire
     * device_local | host_visible heap can cause crashes or slowdowns */
    if (m_device->properties().core.properties.vendorID == uint16_t(DxvkGpuVendor::Nvidia)) {
      bool shrinkNvidiaHvvHeap = device->adapter()->matchesDriver(DxvkGpuVendor::Nvidia,
        VK_DRIVER_ID_NVIDIA_PROPRIETARY_KHR, 0, VK_MAKE_VERSION(465, 0, 0));

      applyTristate(shrinkNvidiaHvvHeap, device->config().shrinkNvidiaHvvHeap);

      if (shrinkNvidiaHvvHeap) {
        for (uint32_t i = 0; i < m_memProps.memoryTypeCount; i++) {
          VkMemoryPropertyFlags hvvFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;

          if ((m_memTypes[i].memType.propertyFlags & hvvFlags) == hvvFlags
           && (m_memTypes[i].heap->properties.size < largestDeviceLocalHeap))
            m_memTypes[i].heap->budget = 32 << 20;
        }
      }
    }
  }


  DxvkMemoryAllocator::~DxvkMemoryAllocator() {

  }


  bool DxvkMemoryAllocator::zeroMappedMemory() const {
    return m_device->config().zeroMappedMemory;
  }


  DxvkMemory DxvkMemoryAllocator::alloc(
    const VkMemoryRequirements*             req,
    const VkMemoryDedicatedRequirements&    dedAllocReq,
    const VkMemoryDedicatedAllocateInfo&    dedAllocInfo,
          VkMemoryPropertyFlags             flags,
          DxvkMemoryFlags                   hints) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    // Keep small allocations together to avoid fragmenting
    // chunks for larger resources with lots of small gaps,
    // as well as resources with potentially weird lifetimes
    if (req->size <= SmallAllocationThreshold) {
      hints.set(DxvkMemoryFlag::Small);
      hints.clr(DxvkMemoryFlag::GpuWritable, DxvkMemoryFlag::GpuReadable);
    }

    // Ignore most hints for host-visible allocations since they
    // usually don't make much sense for those resources
    if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
      hints = hints & DxvkMemoryFlag::Transient;

    // On tiling GPUs, prefer cached memory for host-visible allocations
    // to speed up readbacks. This is a preference only: if no suitable
    // cached memory type exists, we fall back to uncached below.
    VkMemoryPropertyFlags cachedFlags = 0;

    if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
     && m_device->perfHints().preferCachedMemory)
      cachedFlags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;

    // Try to allocate from a memory type which supports the given flags exactly
    auto dedAllocPtr = dedAllocReq.prefersDedicatedAllocation ? &dedAllocInfo : nullptr;
    DxvkMemory result = this->tryAlloc(req, dedAllocPtr, flags | cachedFlags, hints);

    // If we asked for cached memory but none was available, retry uncached
    if (!result && cachedFlags) {
      cachedFlags = 0;
      result = this->tryAlloc(req, dedAllocPtr, flags, hints);
    }

    // If the first attempt failed, try ignoring the dedicated allocation
    if (!result && dedAllocPtr && !dedAllocReq.requiresDedicatedAllocation) {
      result = this->tryAlloc(req, nullptr, flags, hints);
      dedAllocPtr = nullptr;
    }

    // Retry without the hint constraints
    if (!result) {
      hints.set(DxvkMemoryFlag::IgnoreConstraints);
      result = this->tryAlloc(req, nullptr, flags, hints);
    }

    // If that still didn't work, probe slower memory types as well
    VkMemoryPropertyFlags optFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
                                   | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    VkMemoryPropertyFlags remFlags = 0;

    while (!result && (flags & optFlags)) {
      remFlags |= optFlags & -optFlags;
      optFlags &= ~remFlags;

      result = this->tryAlloc(req, dedAllocPtr, flags & ~remFlags, hints);
    }

    if (!result) {
      this->logMemoryError(*req);
      this->logMemoryStats();

      throw DxvkError("DxvkMemoryAllocator: Memory allocation failed");
    }

    return result;
  }


  DxvkMemory DxvkMemoryAllocator::tryAlloc(
    const VkMemoryRequirements*             req,
    const VkMemoryDedicatedAllocateInfo*    dedAllocInfo,
          VkMemoryPropertyFlags             flags,
          DxvkMemoryFlags                   hints) {
    DxvkMemory result;

    for (uint32_t i = 0; i < m_memProps.memoryTypeCount && !result; i++) {
      const bool supported = (req->memoryTypeBits & (1u << i)) != 0;
      const bool adequate  = (m_memTypes[i].memType.propertyFlags & flags) == flags;

      if (supported && adequate) {
        result = this->tryAllocFromType(&m_memTypes[i],
          flags, req->size, req->alignment, hints, dedAllocInfo);
      }
    }

    return result;
  }


  DxvkMemory DxvkMemoryAllocator::tryAllocFromType(
          DxvkMemoryType*                   type,
          VkMemoryPropertyFlags             flags,
          VkDeviceSize                      size,
          VkDeviceSize                      align,
          DxvkMemoryFlags                   hints,
    const VkMemoryDedicatedAllocateInfo*    dedAllocInfo) {
    constexpr VkDeviceSize DedicatedAllocationThreshold = 3;

    VkDeviceSize chunkSize = pickChunkSize(type->memTypeId,
      DedicatedAllocationThreshold * size, hints);

    DxvkMemory memory;

    // Require dedicated allocations for resources that use the Vulkan dedicated
    // allocation bits, or are too large to fit into a single full-sized chunk.
    bool needsDedicatedAllocation = size >= chunkSize || dedAllocInfo;

    // Prefer a dedicated allocation for very large resources in order to
    // reduce fragmentation if a large number of those resources are in use.
    bool wantsDedicatedAllocation = DedicatedAllocationThreshold * size > chunkSize;

    // Try to reuse existing memory as much as possible when the heap is nearly full.
    bool heapBudgetExceeded = 5 * type->heap->stats.memoryUsed + size > 4 * type->heap->properties.size;

    if (!needsDedicatedAllocation && (!wantsDedicatedAllocation || heapBudgetExceeded)) {
      // Attempt to suballocate from existing chunks first
      for (uint32_t i = 0; i < type->chunks.size() && !memory; i++)
        memory = type->chunks[i]->alloc(flags, size, align, hints);

      // If no existing chunk can accommodate the allocation, and if a dedicated
      // allocation is not preferred, create a new chunk and suballocate from it.
      if (!memory && !wantsDedicatedAllocation) {
        DxvkDeviceMemory devMem;

        if (this->shouldFreeEmptyChunks(type->heap, chunkSize))
          this->freeEmptyChunks(type->heap);

        for (uint32_t i = 0; i < 6 && (chunkSize >> i) >= size && !devMem.memHandle; i++)
          devMem = tryAllocDeviceMemory(type, flags, chunkSize >> i, hints, nullptr);

        if (devMem.memHandle) {
          Rc<DxvkMemoryChunk> chunk = new DxvkMemoryChunk(this, type, devMem, hints);
          memory = chunk->alloc(flags, size, align, hints);

          type->chunks.push_back(std::move(chunk));

          adjustChunkSize(type->memTypeId, devMem.memSize, hints);
        }
      }
    }

    // If a dedicated allocation is required or preferred and we haven't managed
    // to suballocate any memory before, try to create a dedicated allocation.
    if (!memory && (needsDedicatedAllocation || wantsDedicatedAllocation)) {
      if (this->shouldFreeEmptyChunks(type->heap, size))
        this->freeEmptyChunks(type->heap);

      DxvkDeviceMemory devMem = this->tryAllocDeviceMemory(
        type, flags, size, hints, dedAllocInfo);

      if (devMem.memHandle != VK_NULL_HANDLE) {
        if (unlikely(devMem.memPointer && this->zeroMappedMemory()))
          std::memset(devMem.memPointer, 0, size);

        memory = DxvkMemory(this, nullptr, type, devMem.memHandle, 0, size, devMem.memPointer);
      }
    }

    if (memory)
      type->heap->stats.memoryUsed += memory.m_length;

    return memory;
  }


  DxvkDeviceMemory DxvkMemoryAllocator::tryAllocDeviceMemory(
          DxvkMemoryType*                   type,
          VkMemoryPropertyFlags             flags,
          VkDeviceSize                      size,
          DxvkMemoryFlags                   hints,
    const VkMemoryDedicatedAllocateInfo*    dedAllocInfo) {
    bool useMemoryPriority = (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
                          && (m_device->features().extMemoryPriority.memoryPriority);

    if (type->heap->budget && type->heap->stats.memoryAllocated + size > type->heap->budget)
      return DxvkDeviceMemory();

    float priority = 0.0f;

    if (hints.test(DxvkMemoryFlag::GpuReadable))
      priority = 0.5f;
    if (hints.test(DxvkMemoryFlag::GpuWritable))
      priority = 1.0f;

    DxvkDeviceMemory result;
    result.memSize  = size;
    result.memFlags = flags;
    result.priority = priority;

    VkMemoryPriorityAllocateInfoEXT prio;
    prio.sType            = VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT;
    prio.pNext            = dedAllocInfo;
    prio.priority         = priority;

    VkMemoryAllocateInfo info;
    info.sType            = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    info.pNext            = useMemoryPriority ? &prio : prio.pNext;
    info.allocationSize   = size;
    info.memoryTypeIndex  = type->memTypeId;

    if (m_vkd->vkAllocateMemory(m_vkd->device(), &info, nullptr, &result.memHandle) != VK_SUCCESS)
      return DxvkDeviceMemory();

    if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
      VkResult status = m_vkd->vkMapMemory(m_vkd->device(), result.memHandle, 0, VK_WHOLE_SIZE, 0, &result.memPointer);

      if (status != VK_SUCCESS) {
        Logger::err(str::format("DxvkMemoryAllocator: Mapping memory failed with ", status));
        m_vkd->vkFreeMemory(m_vkd->device(), result.memHandle, nullptr);
        return DxvkDeviceMemory();
      }
    }

    type->heap->stats.memoryAllocated += size;
    m_device->adapter()->notifyHeapMemoryAlloc(type->heapId, size);
    return result;
  }


  void DxvkMemoryAllocator::free(
    const DxvkMemory&           memory) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);
    memory.m_type->heap->stats.memoryUsed -= memory.m_length;

    if (memory.m_chunk != nullptr) {
      this->freeChunkMemory(
        memory.m_type,
        memory.m_chunk,
        memory.m_offset,
        memory.m_length);
    } else {
      DxvkDeviceMemory devMem;
      devMem.memHandle  = memory.m_memory;
      devMem.memPointer = nullptr;
      devMem.memSize    = memory.m_length;
      this->freeDeviceMemory(memory.m_type, devMem);
    }
  }


  void DxvkMemoryAllocator::freeChunkMemory(
          DxvkMemoryType*       type,
          DxvkMemoryChunk*      chunk,
          VkDeviceSize          offset,
          VkDeviceSize          length) {
    chunk->free(offset, length);

    if (chunk->isEmpty()) {
      Rc<DxvkMemoryChunk> chunkRef = chunk;

      // Free the chunk if we have to, or at least put it at the end of
      // the list so that chunks that are already in use and cannot be
      // freed are prioritized for allocations to reduce memory pressure.
      type->chunks.erase(std::remove(type->chunks.begin(), type->chunks.end(), chunkRef));

      if (!this->shouldFreeChunk(type, chunkRef))
        type->chunks.push_back(std::move(chunkRef));
    }
  }


  void DxvkMemoryAllocator::freeDeviceMemory(
          DxvkMemoryType*       type,
          DxvkDeviceMemory      memory) {
    m_vkd->vkFreeMemory(m_vkd->device(), memory.memHandle, nullptr);
    type->heap->stats.memoryAllocated -= memory.memSize;
    m_device->adapter()->notifyHeapMemoryFree(type->heapId, memory.memSize);
  }


  VkDeviceSize DxvkMemoryAllocator::pickChunkSize(uint32_t memTypeId, VkDeviceSize requiredSize, DxvkMemoryFlags hints) const {
    VkMemoryType type = m_memProps.memoryTypes[memTypeId];
    VkMemoryHeap heap = m_memProps.memoryHeaps[type.heapIndex];

    // Start from the current per-type chunk size and grow it
    // up to the maximum until it can serve the request.
    VkDeviceSize chunkSize = m_memTypes[memTypeId].chunkSize;

    while (chunkSize < requiredSize && chunkSize < MaxChunkSize)
      chunkSize <<= 1u;

    if (hints.test(DxvkMemoryFlag::Small))
      chunkSize = std::min<VkDeviceSize>(chunkSize, 16 << 20);

    // Try to waste a bit less system memory especially in
    // 32-bit applications due to address space constraints
    if (type.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
      chunkSize = std::min<VkDeviceSize>((env::is32BitHostPlatform() ? 16 : 64) << 20, chunkSize);

    // Reduce the chunk size on small heaps so
    // we can at least fit in 15 allocations
    while (chunkSize * 15 > heap.size)
      chunkSize >>= 1;

    return chunkSize;
  }


  void DxvkMemoryAllocator::adjustChunkSize(
          uint32_t              memTypeId,
          VkDeviceSize          allocatedSize,
          DxvkMemoryFlags       hints) {
    VkDeviceSize chunkSize = m_memTypes[memTypeId].chunkSize;

    // Don't bump chunk size if we reached the maximum or if
    // we already were unable to allocate a full chunk.
    if (chunkSize <= allocatedSize && chunkSize <= m_memTypes[memTypeId].heap->stats.memoryAllocated)
      m_memTypes[memTypeId].chunkSize = pickChunkSize(memTypeId, chunkSize * 2, DxvkMemoryFlags());
  }


  bool DxvkMemoryAllocator::shouldFreeChunk(
    const DxvkMemoryType*       type,
    const Rc<DxvkMemoryChunk>&  chunk) const {
    // Under memory pressure, we should start freeing everything.
    if (this->shouldFreeEmptyChunks(type->heap, 0))
      return true;

    // Free chunks that are below the current chunk size since it probably
    // not going to be able to serve enough allocations to be useful.
    if (chunk->size() < type->chunkSize)
      return true;

    // Only keep a small number of chunks of each type around to save memory.
    uint32_t numEmptyChunks = 0;

    for (const auto& c : type->chunks) {
      if (c != chunk && c->isEmpty() && c->isCompatible(chunk))
        numEmptyChunks += 1;
    }

    // Be a bit more lenient on system memory since data uploads may otherwise
    // lead to a large number of allocations and deallocations at runtime.
    uint32_t maxEmptyChunks = env::is32BitHostPlatform() ? 2 : 4;

    if ((type->memType.propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
     || !(type->memType.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
      maxEmptyChunks = 1;

    return numEmptyChunks >= maxEmptyChunks;
  }


  bool DxvkMemoryAllocator::shouldFreeEmptyChunks(
    const DxvkMemoryHeap*       heap,
          VkDeviceSize          allocationSize) const {
    VkDeviceSize budget = heap->budget;

    if (!budget)
      budget = (heap->properties.size * 4) / 5;

    return heap->stats.memoryAllocated + allocationSize > budget;
  }


  void DxvkMemoryAllocator::freeEmptyChunks(
    const DxvkMemoryHeap*       heap) {
    for (uint32_t i = 0; i < m_memProps.memoryTypeCount; i++) {
      DxvkMemoryType* type = &m_memTypes[i];

      if (type->heap != heap)
        continue;

      type->chunks.erase(
        std::remove_if(type->chunks.begin(), type->chunks.end(),
          [] (const Rc<DxvkMemoryChunk>& chunk) { return chunk->isEmpty(); }),
        type->chunks.end());
    }
  }


  void DxvkMemoryAllocator::logMemoryError(const VkMemoryRequirements& req) const {
    std::stringstream sstr;
    sstr << "DxvkMemoryAllocator: Memory allocation failed" << std::endl
         << "  Size:      " << req.size << std::endl
         << "  Alignment: " << req.alignment << std::endl
         << "  Mem types: ";

    uint32_t memTypes = req.memoryTypeBits;

    while (memTypes) {
      uint32_t index = bit::tzcnt(memTypes);
      sstr << index;

      if ((memTypes &= memTypes - 1))
        sstr << ",";
      else
        sstr << std::endl;
    }

    Logger::err(sstr.str());
  }


  void DxvkMemoryAllocator::logMemoryStats() const {
    DxvkAdapterMemoryInfo memHeapInfo = m_device->adapter()->getMemoryHeapInfo();

    std::stringstream sstr;
    sstr << "Heap  Size (MiB)  Allocated   Used        Reserved    Budget" << std::endl;

    for (uint32_t i = 0; i < m_memProps.memoryHeapCount; i++) {
      sstr << std::setw(2) << i << ":   "
           << std::setw(6) << (m_memHeaps[i].properties.size >> 20) << "      "
           << std::setw(6) << (m_memHeaps[i].stats.memoryAllocated >> 20) << "      "
           << std::setw(6) << (m_memHeaps[i].stats.memoryUsed >> 20) << "      ";

      if (m_device->extensions().extMemoryBudget) {
        sstr << std::setw(6) << (memHeapInfo.heaps[i].memoryAllocated >> 20) << "      "
             << std::setw(6) << (memHeapInfo.heaps[i].memoryBudget >> 20) << "      " << std::endl;
      } else {
        sstr << " n/a         n/a" << std::endl;
      }
    }

    Logger::err(sstr.str());
  }

}
