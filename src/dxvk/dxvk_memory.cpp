#include <algorithm>
#include <cstring>

#include "dxvk_device.h"
#include "dxvk_memory.h"

namespace dxvk {

  DxvkMemory::DxvkMemory() { }
  DxvkMemory::DxvkMemory(
          DxvkMemoryAllocator*  alloc,
          DxvkMemoryType*       type,
          VkDeviceMemory        memory,
          VkDeviceSize          address,
          VkDeviceSize          offset,
          VkDeviceSize          length,
          void*                 mapPtr)
  : m_alloc   (alloc),
    m_type    (type),
    m_memory  (memory),
    m_address (address),
    m_offset  (offset),
    m_length  (length),
    m_mapPtr  (mapPtr) { }


  DxvkMemory::DxvkMemory(DxvkMemory&& other)
  : m_alloc   (std::exchange(other.m_alloc,   nullptr)),
    m_type    (std::exchange(other.m_type,    nullptr)),
    m_memory  (std::exchange(other.m_memory,  VkDeviceMemory(VK_NULL_HANDLE))),
    m_address (std::exchange(other.m_address, 0)),
    m_offset  (std::exchange(other.m_offset,  0)),
    m_length  (std::exchange(other.m_length,  0)),
    m_mapPtr  (std::exchange(other.m_mapPtr,  nullptr)) { }


  DxvkMemory& DxvkMemory::operator = (DxvkMemory&& other) {
    this->free();
    m_alloc   = std::exchange(other.m_alloc,   nullptr);
    m_type    = std::exchange(other.m_type,    nullptr);
    m_memory  = std::exchange(other.m_memory,  VkDeviceMemory(VK_NULL_HANDLE));
    m_address = std::exchange(other.m_address, 0);
    m_offset  = std::exchange(other.m_offset,  0);
    m_length  = std::exchange(other.m_length,  0);
    m_mapPtr  = std::exchange(other.m_mapPtr,  nullptr);
    return *this;
  }


  DxvkMemory::~DxvkMemory() {
    this->free();
  }


  void DxvkMemory::free() {
    if (m_alloc != nullptr)
      m_alloc->free(*this);
  }


  DxvkMemoryAllocator::DxvkMemoryAllocator(const DxvkDevice* device)
  : m_vkd             (device->vkd()),
    m_device          (device),
    m_devProps        (device->adapter()->deviceProperties()),
    m_memProps        (device->adapter()->memoryProperties()) {
    m_memTypeCount = m_memProps.memoryTypeCount;
    m_memHeapCount = m_memProps.memoryHeapCount;

    for (uint32_t i = 0; i < m_memHeapCount; i++) {
      auto& heap = m_memHeaps[i];

      heap.index        = i;
      heap.properties   = m_memProps.memoryHeaps[i];
      heap.memoryBudget = m_memProps.memoryHeaps[i].size;

      /* Only enforce a hard heap budget on discrete GPUs, where VRAM is a
       * hard ceiling. On UMA, the OS manages memory pressure across CPU and
       * GPU usage, and a fixed cap causes spurious allocation failures in
       * games with large working sets (e.g. Cris Tales on Intel HD Graphics).
       * The driver reports real pressure via VK_EXT_memory_budget instead,
       * which feeds updateMemoryHeapBudgets below. */
      heap.enforceBudget = (m_memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
        && !m_device->isUnifiedMemoryArchitecture()
        && m_device->properties().core.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    }

    for (uint32_t i = 0; i < m_memTypeCount; i++) {
      auto& type = m_memTypes[i];

      type.index    = i;
      type.memType  = m_memProps.memoryTypes[i];
      type.heap     = &m_memHeaps[m_memProps.memoryTypes[i].heapIndex];

      type.heap->memoryTypes |= 1u << i;

      type.devicePool.maxChunkSize = determineMaxChunkSize(type, false);
      type.mappedPool.maxChunkSize = determineMaxChunkSize(type, true);

      // Uncached system memory is going to be used for large temporary
      // allocations during resource creation. Account for that by always
      // using full-sized chunks.
      if ((type.memType.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
       && !(type.memType.propertyFlags & (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)))
        type.mappedPool.nextChunkSize = type.mappedPool.maxChunkSize;
    }

    determineMemoryTypesWithPropertyFlags();

    updateMemoryHeapBudgets();
  }


  DxvkMemoryAllocator::~DxvkMemoryAllocator() {
    for (uint32_t i = 0; i < m_memTypeCount; i++) {
      auto& type = m_memTypes[i];

      freeAllChunksInPool(type, type.devicePool);
      freeAllChunksInPool(type, type.mappedPool);
    }
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

    // If a dedicated allocation is required or preferred, try that first.
    // Shared resources always end up here because the caller sets both
    // bits, and the export or import info chained onto dedAllocInfo has
    // to reach vkAllocateMemory intact.
    if (dedAllocReq.prefersDedicatedAllocation || dedAllocReq.requiresDedicatedAllocation) {
      DxvkMemory result = this->allocateDedicatedMemory(*req, flags, &dedAllocInfo);

      if (result)
        return result;

      if (dedAllocReq.requiresDedicatedAllocation) {
        this->logMemoryError(*req, flags);
        this->logMemoryStats();

        throw DxvkError("DxvkMemoryAllocator: Memory allocation failed");
      }
    }

    // Suballocate from a memory pool of a suitable memory type
    DxvkMemory result = this->allocateMemory(*req, flags);

    if (result)
      return result;

    // If that failed, probe slower memory types as well
    VkMemoryPropertyFlags optionalFlags = flags & (
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
      VK_MEMORY_PROPERTY_HOST_CACHED_BIT);

    if (optionalFlags) {
      result = this->allocateMemory(*req, flags & ~optionalFlags);

      if (result)
        return result;
    }

    this->logMemoryError(*req, flags);
    this->logMemoryStats();

    throw DxvkError("DxvkMemoryAllocator: Memory allocation failed");
  }


  DxvkMemory DxvkMemoryAllocator::allocateMemory(
    const VkMemoryRequirements& req,
          VkMemoryPropertyFlags properties) {
    // Ensure the allocation size is also aligned
    VkDeviceSize size = align(req.size, req.alignment);

    uint32_t memoryTypeMask = req.memoryTypeBits & getMemoryTypeMask(properties);

    for (uint32_t i = 0; i < m_memTypeCount; i++) {
      if (!(memoryTypeMask & (1u << i)))
        continue;

      auto& type = m_memTypes[i];

      // Use the correct memory pool depending on property flags. This way
      // we avoid wasting address space on unmapped allocations, and mapped
      // allocations cannot fragment device-local memory.
      auto& selectedPool = (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        ? type.mappedPool
        : type.devicePool;

      // Always try to suballocate first, even if the allocation is
      // very large. We will decide what to do if this fails.
      int64_t address = selectedPool.alloc(size, req.alignment);

      if (likely(address >= 0))
        return createMemory(type, selectedPool, address, size);

      // If the allocation is very large, use a dedicated allocation instead
      // of creating a new chunk. This way we avoid excessive fragmentation,
      // especially when multiple such resources are created at once.
      VkDeviceSize maxChunkSize = selectedPool.maxChunkSize;
      uint32_t minResourcesPerChunk = 4u;

      if (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        if (properties & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
          // For HVV allocations, it may be beneficial to ignore the chunk
          // size limit if the resource is large. HVV is usually slow to
          // allocate from and may be very limited in size, so we should
          // avoid dedicated allocations as well as fragmentation.
          maxChunkSize = DxvkPageAllocator::MaxChunkSize / (env::is32BitHostPlatform() ? 4u : 1u);
          maxChunkSize = std::min(maxChunkSize, type.heap->properties.size / MinAllocationsPerHeap);
          maxChunkSize = std::max(maxChunkSize, selectedPool.maxChunkSize);

          minResourcesPerChunk = std::clamp(uint32_t(maxChunkSize / size), 1u, 3u);
        } else {
          // System memory allocations tend to be more volatile, so be
          // lenient and allow a single resource to fill an entire chunk.
          minResourcesPerChunk = 1u;
        }
      }

      if (size * minResourcesPerChunk > maxChunkSize) {
        DxvkDeviceMemory memory = allocateDeviceMemory(type, req.size, properties, nullptr);

        if (!memory.memHandle)
          continue;

        return createMemory(type, memory);
      }

      // Try to allocate a new chunk that is large enough to hold
      // multiple resources of the type we are trying to allocate.
      VkDeviceSize desiredSize = selectedPool.nextChunkSize;

      while (desiredSize < size * minResourcesPerChunk)
        desiredSize *= 2u;

      if (allocateChunkInPool(type, selectedPool, properties, size, desiredSize)) {
        address = selectedPool.alloc(size, req.alignment);

        if (likely(address >= 0))
          return createMemory(type, selectedPool, address, size);
      }
    }

    return DxvkMemory();
  }


  DxvkMemory DxvkMemoryAllocator::allocateDedicatedMemory(
    const VkMemoryRequirements& req,
          VkMemoryPropertyFlags properties,
    const void*                 next) {
    uint32_t memoryTypeMask = req.memoryTypeBits & getMemoryTypeMask(properties);

    for (uint32_t i = 0; i < m_memTypeCount; i++) {
      if (!(memoryTypeMask & (1u << i)))
        continue;

      auto& type = m_memTypes[i];
      DxvkDeviceMemory memory = allocateDeviceMemory(type, req.size, properties, next);

      if (memory.memHandle)
        return createMemory(type, memory);
    }

    return DxvkMemory();
  }


  DxvkDeviceMemory DxvkMemoryAllocator::allocateDeviceMemory(
          DxvkMemoryType&       type,
          VkDeviceSize          size,
          VkMemoryPropertyFlags properties,
    const void*                 next) {
    // Re-query the driver budget before deciding anything, since chunk
    // allocations are rare enough for this not to matter for performance.
    updateMemoryHeapBudgets();

    // Preemptively free some unused allocations to reduce memory waste
    freeEmptyChunksInHeap(*type.heap, size);

    // If we would exceed the vram budget on a dedicated GPU, give up so
    // that the caller can fall back to a system memory type.
    if (!next && type.heap->enforceBudget
     && getMemoryStats(type.heap->index).memoryAllocated + size > type.heap->memoryBudget)
      return DxvkDeviceMemory();

    VkMemoryAllocateInfo memoryInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, next };
    memoryInfo.allocationSize  = size;
    memoryInfo.memoryTypeIndex = type.index;

    // Decide on a memory priority based on the memory type. Dedicated
    // allocations are expected to be high-bandwidth resources such as
    // render targets, while BAR memory is most useful in system memory.
    VkMemoryPriorityAllocateInfoEXT priorityInfo = { VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT };

    if (type.memType.propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
      if (type.memType.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        priorityInfo.priority = 0.0f;
      else if (next)
        priorityInfo.priority = 1.0f;
      else
        priorityInfo.priority = 0.5f;

      if (m_device->features().extMemoryPriority.memoryPriority)
        priorityInfo.pNext = std::exchange(memoryInfo.pNext, &priorityInfo);
    }

    DxvkDeviceMemory result;
    result.memSize  = size;
    result.memFlags = properties;
    result.priority = priorityInfo.priority;

    if (m_vkd->vkAllocateMemory(m_vkd->device(), &memoryInfo, nullptr, &result.memHandle) != VK_SUCCESS)
      return DxvkDeviceMemory();

    if (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
      VkResult status = m_vkd->vkMapMemory(m_vkd->device(), result.memHandle, 0, VK_WHOLE_SIZE, 0, &result.memPointer);

      if (status != VK_SUCCESS) {
        Logger::err(str::format("DxvkMemoryAllocator: Mapping memory failed with ", status));
        m_vkd->vkFreeMemory(m_vkd->device(), result.memHandle, nullptr);
        return DxvkDeviceMemory();
      }
    }

    result.cookie = ++m_nextCookie;

    type.stats.memoryAllocated += size;
    m_device->adapter()->notifyHeapMemoryAlloc(type.heap->index, size);
    return result;
  }


  bool DxvkMemoryAllocator::allocateChunkInPool(
          DxvkMemoryType&       type,
          DxvkMemoryPool&       pool,
          VkMemoryPropertyFlags properties,
          VkDeviceSize          requiredSize,
          VkDeviceSize          desiredSize) {
    // Try to allocate device memory. If the allocation fails, retry with a
    // smaller size until we reach a point where we cannot service it at all.
    DxvkDeviceMemory chunk = { };

    while (!chunk.memHandle && desiredSize >= std::max(requiredSize, DxvkMemoryPool::MinChunkSize)) {
      chunk = allocateDeviceMemory(type, desiredSize, properties, nullptr);
      desiredSize /= 2u;
    }

    if (!chunk.memHandle)
      return false;

    // If we expect the application to require more memory in the
    // future, increase the chunk size for subsequent allocations.
    if (pool.nextChunkSize < pool.maxChunkSize
     && pool.nextChunkSize <= type.stats.memoryAllocated / 2u)
      pool.nextChunkSize *= 2u;

    // Add the newly created chunk to the pool
    uint32_t chunkIndex = pool.pageAllocator.addChunk(chunk.memSize);

    pool.chunks.resize(std::max<size_t>(pool.chunks.size(), chunkIndex + 1u));
    pool.chunks[chunkIndex].memory = chunk;
    return true;
  }


  DxvkMemory DxvkMemoryAllocator::createMemory(
          DxvkMemoryType&       type,
          DxvkMemoryPool&       pool,
          VkDeviceSize          address,
          VkDeviceSize          size) {
    type.stats.memoryUsed += size;

    uint32_t chunkIndex = address >> DxvkPageAllocator::ChunkAddressBits;
    VkDeviceSize offset = address & DxvkPageAllocator::ChunkAddressMask;

    auto& chunk = pool.chunks[chunkIndex];

    void* mapPtr = chunk.memory.memPointer
      ? reinterpret_cast<char*>(chunk.memory.memPointer) + offset
      : nullptr;

    // Some games assume freshly mapped buffers are zero-initialized and
    // break on stale data. Clear the slice on hand-out if requested, which
    // also covers reused slices from recycled chunks.
    if (unlikely(mapPtr && this->zeroMappedMemory()))
      std::memset(mapPtr, 0, size);

    return DxvkMemory(this, &type, chunk.memory.memHandle,
      address, offset, size, mapPtr);
  }


  DxvkMemory DxvkMemoryAllocator::createMemory(
          DxvkMemoryType&       type,
    const DxvkDeviceMemory&     memory) {
    type.stats.memoryUsed += memory.memSize;

    if (unlikely(memory.memPointer && this->zeroMappedMemory()))
      std::memset(memory.memPointer, 0, memory.memSize);

    return DxvkMemory(this, &type, memory.memHandle,
      DedicatedChunkAddress, 0, memory.memSize, memory.memPointer);
  }


  void DxvkMemoryAllocator::free(
    const DxvkMemory&           memory) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    auto& type = *memory.m_type;
    type.stats.memoryUsed -= memory.m_length;

    if (memory.m_address & DedicatedChunkAddress) {
      DxvkDeviceMemory devMem;
      devMem.memHandle  = memory.m_memory;
      devMem.memPointer = nullptr;
      devMem.memSize    = memory.m_length;

      this->freeDeviceMemory(type, devMem);
    } else {
      // Mapped chunks only ever come from the mapped pool, so the map
      // pointer tells us which pool the address belongs to.
      auto& pool = memory.m_mapPtr ? type.mappedPool : type.devicePool;

      if (unlikely(pool.free(memory.m_address, memory.m_length)))
        this->freeEmptyChunksInPool(type, pool, 0);
    }
  }


  void DxvkMemoryAllocator::freeDeviceMemory(
          DxvkMemoryType&       type,
          DxvkDeviceMemory      memory) {
    m_vkd->vkFreeMemory(m_vkd->device(), memory.memHandle, nullptr);

    type.stats.memoryAllocated -= memory.memSize;
    m_device->adapter()->notifyHeapMemoryFree(type.heap->index, memory.memSize);
  }


  bool DxvkMemoryAllocator::freeEmptyChunksInPool(
          DxvkMemoryType&       type,
          DxvkMemoryPool&       pool,
          VkDeviceSize          allocationSize) {
    // Allow for one unused max-size chunk on device-local memory types.
    // For mapped memory allocations we need to be more lenient, since
    // applications frequently allocate staging buffers.
    VkDeviceSize maxUnusedMemory = pool.maxChunkSize;

    if (&pool == &type.mappedPool)
      maxUnusedMemory *= 4u;

    // Factor the current memory allocation into the decision to free chunks
    VkDeviceSize heapBudget = type.heap->memoryBudget;
    VkDeviceSize heapAllocated = getMemoryStats(type.heap->index).memoryAllocated;

    VkDeviceSize unusedMemory = 0u;

    bool chunkFreed = false;

    for (uint32_t i = 0; i < pool.chunks.size(); i++) {
      DxvkMemoryChunk& chunk = pool.chunks[i];

      if (!chunk.memory.memHandle || pool.pageAllocator.pagesUsed(i))
        continue;

      // Free the chunk if it is smaller than the current chunk size of the
      // pool, since it is unlikely to be useful for future allocations.
      // Also free if the pending allocation would exceed the heap budget.
      bool shouldFree = chunk.memory.memSize < pool.nextChunkSize
        || allocationSize + heapAllocated > heapBudget
        || allocationSize > heapBudget;

      // If we still don't free the chunk under these conditions, count it
      // towards unused memory in the current memory pool. Once we exceed
      // the limit, free any empty chunk we encounter.
      if (!shouldFree) {
        unusedMemory += chunk.memory.memSize;
        shouldFree = unusedMemory > maxUnusedMemory;
      }

      if (shouldFree) {
        freeDeviceMemory(type, chunk.memory);
        heapAllocated -= chunk.memory.memSize;

        chunk = DxvkMemoryChunk();
        pool.pageAllocator.removeChunk(i);

        chunkFreed = true;
      }
    }

    return chunkFreed;
  }


  void DxvkMemoryAllocator::freeEmptyChunksInHeap(
    const DxvkMemoryHeap&       heap,
          VkDeviceSize          allocationSize) {
    for (uint32_t i = 0; i < m_memTypeCount; i++) {
      if (!(heap.memoryTypes & (1u << i)))
        continue;

      auto& type = m_memTypes[i];

      freeEmptyChunksInPool(type, type.devicePool, allocationSize);
      freeEmptyChunksInPool(type, type.mappedPool, allocationSize);
    }
  }


  void DxvkMemoryAllocator::freeAllChunksInPool(
          DxvkMemoryType&       type,
          DxvkMemoryPool&       pool) {
    for (uint32_t i = 0; i < pool.chunks.size(); i++) {
      if (pool.chunks[i].memory.memHandle) {
        freeDeviceMemory(type, pool.chunks[i].memory);
        pool.chunks[i] = DxvkMemoryChunk();
      }
    }
  }


  VkDeviceSize DxvkMemoryAllocator::determineMaxChunkSize(
    const DxvkMemoryType&       type,
          bool                  mappable) const {
    VkDeviceSize size = DxvkMemoryPool::MaxChunkSize;

    // Prefer smaller chunks for host-visible allocations in order to
    // reduce the amount of address space required. We compensate for
    // the smaller size by allowing more unused memory on these heaps.
    if (mappable)
      size /= env::is32BitHostPlatform() ? 16u : 4u;

    // Ensure that we can at least do a handful of allocations to fill
    // the heap. Important on systems with a small BAR or a small heap.
    while (MinAllocationsPerHeap * size > type.heap->properties.size)
      size /= 2u;

    // Always use at least the minimum chunk size
    return std::max(size, DxvkMemoryPool::MinChunkSize);
  }


  void DxvkMemoryAllocator::determineMemoryTypesWithPropertyFlags() {
    // Initialize look-up table for memory type masks based on required
    // property flags. This lets us avoid iterating over unsuitable types.
    for (uint32_t i = 0; i < m_memTypesByPropertyFlags.size(); i++) {
      VkMemoryPropertyFlags flags = VkMemoryPropertyFlags(i);

      uint32_t vidmemMask = 0u;
      uint32_t sysmemMask = 0u;

      for (uint32_t j = 0; j < m_memTypeCount; j++) {
        VkMemoryPropertyFlags typeFlags = m_memTypes[j].memType.propertyFlags;

        if ((typeFlags & flags) != flags)
          continue;

        if (typeFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
          vidmemMask |= 1u << j;
        else
          sysmemMask |= 1u << j;
      }

      // If a system memory type exists with the given properties, do not
      // include any device-local memory types. This way we won't ever pick
      // host-visible vram when explicitly trying to allocate system memory.
      m_memTypesByPropertyFlags[i] = sysmemMask ? sysmemMask : vidmemMask;
    }

    uint32_t hostCachedIndex = uint32_t(
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
      VK_MEMORY_PROPERTY_HOST_CACHED_BIT);

    uint32_t hostCoherentIndex = uint32_t(
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    uint32_t hostVisibleVramIndex = uint32_t(
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    // If there is no cached coherent memory type, reuse the uncached
    // one. This is likely slow, but API front-ends rely on it.
    if (!m_memTypesByPropertyFlags[hostCachedIndex])
      m_memTypesByPropertyFlags[hostCachedIndex] = m_memTypesByPropertyFlags[hostCoherentIndex];

    // If we zero mapped memory, we need good CPU memory bandwidth.
    // Prefer uncached system memory over HVV in that case.
    if (m_device->config().zeroMappedMemory)
      m_memTypesByPropertyFlags[hostVisibleVramIndex] = m_memTypesByPropertyFlags[hostCoherentIndex];

    // On tilers, uncached stores are expected to be very slow. Always
    // use cached memory for mapped allocations there. This replaces the
    // cached-first, uncached-on-failure retry the allocator used to do.
    if (m_device->perfHints().preferCachedMemory) {
      m_memTypesByPropertyFlags[hostCoherentIndex] = m_memTypesByPropertyFlags[hostCachedIndex];
      m_memTypesByPropertyFlags[hostVisibleVramIndex] = m_memTypesByPropertyFlags[hostCachedIndex];
    }
  }


  uint32_t DxvkMemoryAllocator::getMemoryTypeMask(
          VkMemoryPropertyFlags properties) const {
    return m_memTypesByPropertyFlags[uint32_t(properties) % uint32_t(m_memTypesByPropertyFlags.size())];
  }


  DxvkMemoryStats DxvkMemoryAllocator::getMemoryStats(uint32_t heap) const {
    DxvkMemoryStats result = { };

    for (uint32_t i = 0; i < m_memTypeCount; i++) {
      if (!(m_memHeaps[heap].memoryTypes & (1u << i)))
        continue;

      result.memoryAllocated += m_memTypes[i].stats.memoryAllocated;
      result.memoryUsed      += m_memTypes[i].stats.memoryUsed;
    }

    result.memoryBudget = m_memHeaps[heap].memoryBudget;
    return result;
  }


  void DxvkMemoryAllocator::updateMemoryHeapBudgets() {
    if (!m_device->extensions().extMemoryBudget)
      return;

    VkDeviceSize maxBudget = m_device->config().maxMemoryBudget;

    DxvkAdapterMemoryInfo memHeapInfo = m_device->adapter()->getMemoryHeapInfo();

    for (uint32_t i = 0; i < m_memHeapCount; i++) {
      if (!memHeapInfo.heaps[i].memoryBudget)
        continue;

      // Deduct driver-internal allocations from the resource budget
      VkDeviceSize allocated = getMemoryStats(i).memoryAllocated;

      VkDeviceSize internal = std::max(memHeapInfo.heaps[i].memoryAllocated, allocated) - allocated;
                   internal = std::min(memHeapInfo.heaps[i].memoryBudget, internal);

      m_memHeaps[i].memoryBudget = std::min(
        memHeapInfo.heaps[i].memoryBudget - internal,
        m_memHeaps[i].properties.size);

      if (maxBudget)
        m_memHeaps[i].memoryBudget = std::min(m_memHeaps[i].memoryBudget, maxBudget);
    }
  }


  void DxvkMemoryAllocator::logMemoryError(
    const VkMemoryRequirements& req,
          VkMemoryPropertyFlags flags) const {
    Logger::err(str::format(
      "DxvkMemoryAllocator: Memory allocation failed",
      "\n  Size:      ", req.size,
      "\n  Alignment: ", req.alignment,
      "\n  Mem flags: ", "0x", std::hex, flags,
      "\n  Mem types: ", "0x", std::hex, req.memoryTypeBits));
  }


  void DxvkMemoryAllocator::logMemoryStats() const {
    DxvkAdapterMemoryInfo memHeapInfo = m_device->adapter()->getMemoryHeapInfo();

    for (uint32_t i = 0; i < m_memHeapCount; i++) {
      DxvkMemoryStats stats = getMemoryStats(i);

      Logger::err(str::format("Heap ", i, ": ",
        (stats.memoryAllocated >> 20), " MB allocated, ",
        (stats.memoryUsed      >> 20), " MB used, ",
        m_device->extensions().extMemoryBudget
          ? str::format(
              (memHeapInfo.heaps[i].memoryAllocated >> 20), " MB allocated (driver), ",
              (memHeapInfo.heaps[i].memoryBudget    >> 20), " MB budget (driver), ",
              (m_memHeaps[i].properties.size        >> 20), " MB total")
          : str::format(
              (m_memHeaps[i].properties.size        >> 20), " MB total")));
    }
  }

}
