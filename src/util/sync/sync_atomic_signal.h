#pragma once

#ifdef _WIN32

#include <synchapi.h>
#include <errhandlingapi.h>
#include <atomic>

#include "../util_string.h"
#include "../log/log.h"

//#define _atomic_signal_debug(x) Logger::debug(x)
#define _atomic_signal_debug(x) void()

namespace dxvk::sync {

  /**
   * \brief WaitOnAddress based sync implementation
   */
  class AtomicSignal {

  public:

    AtomicSignal(const char* name, bool initValue)
    : m_name(name) {
      m_flag.store(initValue, std::memory_order_release);
    }

    ~AtomicSignal() {}

    void wait() {
      _atomic_signal_debug( std::string("enter wait for ") + m_name );

      while (true) {

        bool _true = true;
        if (m_flag.load(std::memory_order_acquire)
        && m_flag.compare_exchange_strong(_true, false, std::memory_order_release, std::memory_order_acquire))
          break;

        // waits until m_flag becomes != false
        bool res = WaitOnAddress(&m_flag, (void*) &m_flagFalse, 1, m_infinite);

        if (unlikely(!res))
          Logger::err(str::format("WaitOnAddress failed. LastError: ", GetLastError()));

      }

      _atomic_signal_debug( std::string("finish wait for ") + m_name );
    }

    void signal_one() {
      _atomic_signal_debug( std::string("enter signal_one for ") + m_name );

      bool _false = 0;
      if (m_flag.compare_exchange_strong(_false, true, std::memory_order_release, std::memory_order_acquire))
        WakeByAddressSingle(&m_flag);

      _atomic_signal_debug( std::string("finish signal_one for ") + m_name );
    }

    void signal_all() {
      _atomic_signal_debug( std::string("enter signal_all for ") + m_name );

      m_flag.store(true, std::memory_order_release);
      WakeByAddressAll(&m_flag);

      _atomic_signal_debug( std::string("finish signal_all for ") + m_name );
    }

    void clear() {
      _atomic_signal_debug( std::string("clear flag for ") + m_name );
      m_flag.store(false, std::memory_order_release);
    }

  private:

    alignas(64) std::atomic<bool>       m_flag;
    const char*                         m_name;

    static constexpr std::atomic<bool>  m_flagFalse = { false };
    static constexpr uint32_t           m_infinite  = 0xffffffff; // matches INFINITE define in winbase.h
  };

}

#else

#include <atomic>
#include <stdint.h>
#include <climits>
#include <err.h>
#include <errno.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../util_string.h"
#include "../log/log.h"

static_assert( sizeof(std::atomic<uint32_t>) == sizeof(uint32_t) );


static int futex(uint32_t* uaddr, int futex_op, uint32_t val,
                 const struct timespec* timeout, uint32_t* uaddr2, uint32_t val3) {

  return syscall(SYS_futex, uaddr, futex_op, val, timeout, uaddr2, val3);
}


namespace dxvk::sync {

  /**
   * \brief futex based sync implementation
   */
  class AtomicSignal {

  public:

    AtomicSignal(const char* name, bool initValue)
    : m_name(name) {
      m_flag.store(initValue);
    }

    ~AtomicSignal() {}

    void wait() {

      while (true) {

        long s;
        uint32_t one = 1;
        if (m_flag.compare_exchange_strong(one, 0))
          return;

        // waits until m_flag becomes != 0
        s = futex((uint32_t*)&m_flag, FUTEX_WAIT_PRIVATE, 0, NULL, NULL, 0);

        if (unlikely(s == -1 && errno != EAGAIN))
          Logger::err("futex-FUTEX_WAIT");

      }
    }

    void signal_one() {

      long s;
      uint32_t zero = 0;
      if (m_flag.compare_exchange_strong(zero, 1)) {
        s = futex((uint32_t*)&m_flag, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);

        if (unlikely(s == -1))
          Logger::err("futex-FUTEX_WAKE");
      }

    }

    void signal_all() {

      long s;
      m_flag.store(1);
      s = futex((uint32_t*)&m_flag, FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0);

      if (unlikely(s == -1))
          Logger::err("futex-FUTEX_WAKE");

    }

    void clear() {
      m_flag.store(0);
    }

  private:

    alignas(64) std::atomic<uint32_t>    m_flag;
    const char*                          m_name;

  };

}



#endif
