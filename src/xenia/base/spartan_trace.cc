// RPCS3 Spartan Test trackers, ported to Xenia (PSYCHOsmurf / RPCS3 Spartan Test).
// Released under the BSD license, like Xenia - see LICENSE in the root.
#include "xenia/base/spartan_trace.h"

#include "xenia/base/platform.h"
#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#endif

namespace xe {
namespace spartan {

void* CurrentThreadQueryHandle() {
#if XE_PLATFORM_WIN32
  // Xenia's thread objects hold GetCurrentThread()'s pseudo handle, which means "the calling thread" wherever it is
  // used, so another thread reading it gets its own CPU time. This is a real handle to this thread, only for reading
  // its times. Never closed (one per thread start): the sampler can never read a closed or reused handle.
  HANDLE real = nullptr;
  if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &real,
                      THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0)) {
    return real;
  }
#endif
  return nullptr;
}

}  // namespace spartan
}  // namespace xe
