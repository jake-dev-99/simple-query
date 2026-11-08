// The probe DLL's exports. The DLL is built from this file and its own copy
// of platform_thread_dispatcher.cpp, so the dispatcher's code lives in the
// DLL just as it does in the plugin.

#include <memory>

#include "platform_thread_dispatcher.h"

// Purpose: Create a dispatcher inside this DLL.
// @returns The dispatcher, or null when it is not ready.
// @throws Nothing; the export boundary cannot carry an exception.
extern "C" __declspec(dllexport) void* CreateProbeDispatcher() noexcept {
  auto dispatcher =
      std::make_unique<simple_query_windows::PlatformThreadDispatcher>([]() {});
  if (!dispatcher->IsReady()) {
    return nullptr;
  }
  return dispatcher.release();
}

// Purpose: Destroy a dispatcher CreateProbeDispatcher returned.
// @param dispatcher is the dispatcher to destroy.
// @returns Nothing.
// @throws Nothing.
extern "C" __declspec(dllexport) void DestroyProbeDispatcher(void* dispatcher) noexcept {
  delete static_cast<simple_query_windows::PlatformThreadDispatcher*>(dispatcher);
}
