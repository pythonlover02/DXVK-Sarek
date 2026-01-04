#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>
#include "../log/log.h"


namespace dxvk::sync {

  /*
   * Simple and efficient arena allocator, where the first objects that are allocated
   * are expected to be freed first too. This condition is checked and the user has to care
   * for correct usage. This allocator is thread-safe.
   */

  template<typename T, int size>
  class RingbufferAllocator {
  public:

    RingbufferAllocator() { }
    ~RingbufferAllocator() { }

    T* alloc() {
      Indices expected = m_indices.load();
      Indices desired;

      high_resolution_clock::time_point allocTime {};

      do {
        desired = expected;
        desired.producer = (expected.producer + 1) % size;
        desired.allocCount++;

        // safe guard, but this allocator is not meant to hit this condition
        if (unlikely(desired.allocCount > size)) {
          checkThrowError(allocTime);
          continue;
        }
      } while (!m_indices.compare_exchange_weak( expected, desired ));

      return &m_data[desired.producer];
    }

    void free(T* data) {
      if (data == nullptr)
        return;

      int index = (uintptr_t(data)-uintptr_t(m_data))/sizeof(T);

      Indices expected = m_indices.load();
      Indices desired;

      if (unlikely(expected.consumer != index)) {
        Logger::err( str::format( "RingbufferAllocator expected release ",
          expected.consumer, ", got ", index ) );
      }

      do {
        desired = expected;
        desired.consumer = (index + 1) % size;
        desired.allocCount--;
      } while (!m_indices.compare_exchange_weak( expected, desired ));

      if (unlikely(desired.allocCount < 0)) {
        Logger::err( "RingbufferAllocator allocCount < 0" );
      }
    }

    T* getDataUnsafe()
      { return m_data; }

  private:

    void checkThrowError( high_resolution_clock::time_point& allocTime ) {
      // adding this check here in case allocating would cause a freeze,
      // which should never happen, but in case it would, this helps to
      // quickly identify missing calling free() as the root cause
      if (allocTime == high_resolution_clock::time_point{} ) {
        allocTime = high_resolution_clock::now();
      }
      auto t2 = high_resolution_clock::now();
      if (std::chrono::duration_cast<std::chrono::milliseconds>(
        t2 - allocTime).count() > 1000) {
        throw DxvkError("RingbufferAllocator could not allocate");
      }
    }

    struct Indices {
      uint16_t producer = 0;
      uint16_t consumer = 1;
      int32_t allocCount = 0;
    };

    std::atomic<Indices> m_indices;
    T m_data[size];
  };

}
