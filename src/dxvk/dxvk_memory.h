#pragma once

#include "dxvk_adapter.h"
#include "dxvk_allocator.h"

namespace dxvk {

  class DxvkMemoryAllocator;

  /**
   * \brief Memory stats
   *
   * Reports the amount of device memory
   * allocated and used by the application.
   */
  struct DxvkMemoryStats {
    VkDeviceSize memoryAllocated = 0;
    VkDeviceSize memoryUsed      = 0;
    VkDeviceSize memoryBudget    = 0;
  };


  enum class DxvkSharedHandleMode {
      None,
      Import,
      Export,
  };

  /**
   * \brief Shared handle info
   *
   * The shared resource information for a given resource.
   */
  struct DxvkSharedHandleInfo {
    DxvkSharedHandleMode mode = DxvkSharedHandleMode::None;
    VkExternalMemoryHandleTypeFlagBits type   = VK_EXTERNAL_MEMORY_HANDLE_TYPE_FLAG_BITS_MAX_ENUM;
    union {
#ifdef _WIN32
      HANDLE                             handle = INVALID_HANDLE_VALUE;
#else
      // Placeholder for other handle types, such as FD
      void *dummy;
#endif
    };
  };


  /**
   * \brief Device memory object
   *
   * Stores a Vulkan memory object. If the object
   * was allocated on host-visible memory, it will
   * be persistently mapped.
   */
  struct DxvkDeviceMemory {
    VkDeviceMemory        memHandle  = VK_NULL_HANDLE;
    void*                 memPointer = nullptr;
    VkDeviceSize          memSize    = 0;
    VkMemoryPropertyFlags memFlags   = 0;
    float                 priority   = 0.0f;
    uint64_t              cookie     = 0;
  };


  /**
   * \brief Memory chunk
   *
   * Stores a device memory object with some metadata.
   * Chunks are addressed by index within their pool.
   */
  struct DxvkMemoryChunk {
    DxvkDeviceMemory memory;
  };


  /**
   * \brief Memory pool
   *
   * Stores a list of memory chunks, as well as an
   * allocator covering the entire pool. Small
   * allocations are served from size-class pools
   * so that they cannot fragment larger regions.
   */
  struct DxvkMemoryPool {
    constexpr static VkDeviceSize MaxChunkSize = DxvkPageAllocator::MaxChunkSize;
    constexpr static VkDeviceSize MinChunkSize = MaxChunkSize / 64u;

    /// Backing storage for allocated memory chunks
    std::vector<DxvkMemoryChunk> chunks;
    /// Memory allocator covering the entire memory pool
    DxvkPageAllocator pageAllocator;
    /// Pool allocator that sits on top of the page allocator
    DxvkPoolAllocator poolAllocator = { pageAllocator };
    /// Minimum desired allocation size for the next chunk.
    /// Always a power of two.
    VkDeviceSize nextChunkSize = MinChunkSize;
    /// Maximum chunk size for the memory pool. Hard limit.
    VkDeviceSize maxChunkSize = MaxChunkSize;

    int64_t alloc(uint64_t size, uint64_t align) {
      if (size <= DxvkPoolAllocator::MaxSize)
        return poolAllocator.alloc(size);
      else
        return pageAllocator.alloc(size, align);
    }

    bool free(uint64_t address, uint64_t size) {
      if (size <= DxvkPoolAllocator::MaxSize)
        return poolAllocator.free(address, size);
      else
        return pageAllocator.free(address, size);
    }
  };


  /**
   * \brief Memory heap
   *
   * Corresponds to a Vulkan memory heap and stores
   * its properties as well as the current budget.
   */
  struct DxvkMemoryHeap {
    uint32_t          index         = 0u;
    uint32_t          memoryTypes   = 0u;
    VkMemoryHeap      properties    = { };
    VkDeviceSize      memoryBudget  = 0u;
    VkBool32          enforceBudget = VK_FALSE;
  };


  /**
   * \brief Memory type
   *
   * Corresponds to a Vulkan memory type and stores
   * the memory pools used to sub-allocate memory on
   * this memory type. Mappable and non-mappable
   * allocations are kept in separate pools.
   */
  struct DxvkMemoryType {
    uint32_t          index         = 0u;
    VkMemoryType      memType       = { };

    DxvkMemoryHeap*   heap          = nullptr;

    DxvkMemoryStats   stats         = { };

    DxvkMemoryPool    devicePool;
    DxvkMemoryPool    mappedPool;
  };


  /**
   * \brief Memory slice
   *
   * Represents a slice of memory that has
   * been sub-allocated from a bigger chunk.
   */
  class DxvkMemory {
    friend class DxvkMemoryAllocator;
  public:

    DxvkMemory();
    DxvkMemory             (DxvkMemory&& other);
    DxvkMemory& operator = (DxvkMemory&& other);
    ~DxvkMemory();

    /**
     * \brief Memory object
     *
     * This information is required when
     * binding memory to Vulkan objects.
     * \returns Memory object
     */
    VkDeviceMemory memory() const {
      return m_memory;
    }

    /**
     * \brief Offset into device memory
     *
     * This information is required when
     * binding memory to Vulkan objects.
     * \returns Offset into device memory
     */
    VkDeviceSize offset() const {
      return m_offset;
    }

    /**
     * \brief Pointer to mapped data
     *
     * \param [in] offset Byte offset
     * \returns Pointer to mapped data
     */
    void* mapPtr(VkDeviceSize offset) const {
      return reinterpret_cast<char*>(m_mapPtr) + offset;
    }

    /**
     * \brief Returns length of memory allocated
     *
     * \returns Memory size
     */
    VkDeviceSize length() const {
      return m_length;
    }

    /**
     * \brief Checks whether the memory slice is defined
     *
     * \returns \c true if this slice points to actual device
     *          memory, and \c false if it is undefined.
     */
    operator bool () const {
      return m_memory != VK_NULL_HANDLE;
    }

  private:

    DxvkMemory(
      DxvkMemoryAllocator*  alloc,
      DxvkMemoryType*       type,
      VkDeviceMemory        memory,
      VkDeviceSize          address,
      VkDeviceSize          offset,
      VkDeviceSize          length,
      void*                 mapPtr);

    DxvkMemoryAllocator*  m_alloc   = nullptr;
    DxvkMemoryType*       m_type    = nullptr;
    VkDeviceMemory        m_memory  = VK_NULL_HANDLE;
    VkDeviceSize          m_address = 0;
    VkDeviceSize          m_offset  = 0;
    VkDeviceSize          m_length  = 0;
    void*                 m_mapPtr  = nullptr;

    void free();

  };


  /**
   * \brief Memory allocation flags
   *
   * Retained for the benefit of callers. Chunk grouping is
   * derived from the memory type and from the allocation
   * size, so these no longer influence placement.
   */
  enum class DxvkMemoryFlag : uint32_t {
    Small             = 0,  ///< Small allocation
    GpuReadable       = 1,  ///< Medium-priority resource
    GpuWritable       = 2,  ///< High-priority resource
    Transient         = 3,  ///< Resource is short-lived
    IgnoreConstraints = 4,  ///< Ignore most allocation flags
  };

  using DxvkMemoryFlags = Flags<DxvkMemoryFlag>;


  /**
   * \brief Memory allocator
   *
   * Allocates device memory for Vulkan resources.
   * Memory objects will be destroyed automatically.
   */
  class DxvkMemoryAllocator {
    friend class DxvkMemory;

    /// Address bit that marks a dedicated allocation. Dedicated
    /// allocations do not belong to any chunk in any pool.
    constexpr static VkDeviceSize DedicatedChunkAddress = VkDeviceSize(1) << 63;

    constexpr static VkDeviceSize MinChunkSize = DxvkMemoryPool::MinChunkSize;
    constexpr static VkDeviceSize MaxChunkSize = DxvkMemoryPool::MaxChunkSize;

    /// Minimum number of allocations we want to be able to fit into a heap
    constexpr static uint32_t MinAllocationsPerHeap = 7u;
  public:

    DxvkMemoryAllocator(const DxvkDevice* device);
    ~DxvkMemoryAllocator();

    /**
     * \brief Buffer-image granularity
     *
     * The granularity between linear and non-linear
     * resources in adjacent memory locations. See
     * section 11.6 of the Vulkan spec for details.
     * \returns Buffer-image granularity
     */
    VkDeviceSize bufferImageGranularity() const {
      return m_devProps.limits.bufferImageGranularity;
    }

    /**
     * \brief Allocates device memory
     *
     * \param [in] req Memory requirements
     * \param [in] dedAllocReq Dedicated allocation requirements
     * \param [in] dedAllocInfo Dedicated allocation info
     * \param [in] flags Memory type flags
     * \param [in] hints Memory hints
     * \returns Allocated memory slice
     */
    DxvkMemory alloc(
      const VkMemoryRequirements*             req,
      const VkMemoryDedicatedRequirements&    dedAllocReq,
      const VkMemoryDedicatedAllocateInfo&    dedAllocInfo,
            VkMemoryPropertyFlags             flags,
            DxvkMemoryFlags                   hints);

    /**
     * \brief Queries memory stats
     *
     * Returns the total amount of memory
     * allocated and used for a given heap.
     * \param [in] heap Heap index
     * \returns Memory stats for this heap
     */
    DxvkMemoryStats getMemoryStats(uint32_t heap) const;

    /**
     * \brief Whether mapped memory should be zero-initialized
     * \returns \c true if zeroMappedMemory is enabled
     */
    bool zeroMappedMemory() const;

  private:

    const Rc<vk::DeviceFn>                 m_vkd;
    const DxvkDevice*                      m_device;
    const VkPhysicalDeviceProperties       m_devProps;
    const VkPhysicalDeviceMemoryProperties m_memProps;

    dxvk::mutex                                     m_mutex;

    uint32_t m_memTypeCount = 0u;
    uint32_t m_memHeapCount = 0u;

    std::array<DxvkMemoryHeap, VK_MAX_MEMORY_HEAPS> m_memHeaps = { };
    std::array<DxvkMemoryType, VK_MAX_MEMORY_TYPES> m_memTypes = { };

    std::array<uint32_t, 16> m_memTypesByPropertyFlags = { };

    uint64_t m_nextCookie = 0u;

    DxvkMemory allocateMemory(
      const VkMemoryRequirements& req,
            VkMemoryPropertyFlags properties);

    DxvkMemory allocateDedicatedMemory(
      const VkMemoryRequirements& req,
            VkMemoryPropertyFlags properties,
      const void*                 next);

    DxvkDeviceMemory allocateDeviceMemory(
            DxvkMemoryType&       type,
            VkDeviceSize          size,
            VkMemoryPropertyFlags properties,
      const void*                 next);

    bool allocateChunkInPool(
            DxvkMemoryType&       type,
            DxvkMemoryPool&       pool,
            VkMemoryPropertyFlags properties,
            VkDeviceSize          requiredSize,
            VkDeviceSize          desiredSize);

    DxvkMemory createMemory(
            DxvkMemoryType&       type,
            DxvkMemoryPool&       pool,
            VkDeviceSize          address,
            VkDeviceSize          size);

    DxvkMemory createMemory(
            DxvkMemoryType&       type,
      const DxvkDeviceMemory&     memory);

    void free(
      const DxvkMemory&           memory);

    void freeDeviceMemory(
            DxvkMemoryType&       type,
            DxvkDeviceMemory      memory);

    bool freeEmptyChunksInPool(
            DxvkMemoryType&       type,
            DxvkMemoryPool&       pool,
            VkDeviceSize          allocationSize);

    void freeEmptyChunksInHeap(
      const DxvkMemoryHeap&       heap,
            VkDeviceSize          allocationSize);

    void freeAllChunksInPool(
            DxvkMemoryType&       type,
            DxvkMemoryPool&       pool);

    VkDeviceSize determineMaxChunkSize(
      const DxvkMemoryType&       type,
            bool                  mappable) const;

    void determineMemoryTypesWithPropertyFlags();

    uint32_t getMemoryTypeMask(
            VkMemoryPropertyFlags properties) const;

    void updateMemoryHeapBudgets();

    void logMemoryError(
      const VkMemoryRequirements& req,
            VkMemoryPropertyFlags flags) const;

    void logMemoryStats() const;

  };

}
