#ifndef SIMPLE_QUERY_WINDOWS_PLATFORM_THREAD_DISPATCHER_H_
#define SIMPLE_QUERY_WINDOWS_PLATFORM_THREAD_DISPATCHER_H_

#include <windows.h>

#include <functional>
#include <mutex>

namespace simple_query_windows {

// Purpose: Run one callback on the thread that created the dispatcher, when
// any thread asks for it. Flutter's Windows embedder accepts channel messages
// only on the platform thread, so observer workers wake this dispatcher
// instead of sending events themselves.
//
// Wake requests coalesce: while one wake is queued, further requests add
// nothing. The platform thread's message queue therefore holds at most one
// message from each dispatcher, however fast workers report changes.
//
// The dispatcher owns a message-only window. Create and destroy it on the
// platform thread, whose message loop delivers the wake. A wake still queued
// when the dispatcher is destroyed is dropped without running.
//
// The window class is registered for the module that holds this code, and
// the last dispatcher destroyed unregisters it, so an unloaded plugin leaves
// no class pointing at its code.
class PlatformThreadDispatcher {
 public:
  // Purpose: Create the message-only window that receives wake requests.
  // @param on_wake runs on the creating thread once per delivered wake.
  // @returns Nothing; check IsReady() before relying on wake delivery.
  // @throws Nothing; a window that cannot be created leaves IsReady() false.
  explicit PlatformThreadDispatcher(std::function<void()> on_wake);

  // Purpose: Destroy the window so that no queued wake runs afterwards.
  // @returns Nothing.
  // @throws Nothing.
  ~PlatformThreadDispatcher();

  PlatformThreadDispatcher(const PlatformThreadDispatcher&) = delete;
  PlatformThreadDispatcher& operator=(const PlatformThreadDispatcher&) = delete;

  // Purpose: Report whether wake requests can reach the creating thread.
  // @returns true when the message window exists.
  // @throws Nothing.
  bool IsReady() const;

  // Purpose: Ask the creating thread to run on_wake. Safe from any thread.
  // @returns true when a wake is queued, either by this call or by an earlier
  // one that has not run yet. false when the wake could not be queued, for
  // example because the thread's message queue is full; GetLastError() on the
  // calling thread then holds the reason. Nothing is queued after a false
  // return, so the caller must ask again later.
  // @throws Nothing.
  bool RequestWake();

 private:
  // Purpose: Receive the wake message on the creating thread.
  // @param window is the dispatcher's message-only window.
  // @param message is the message identifier.
  // @param wparam is the message's first parameter.
  // @param lparam is the message's second parameter.
  // @returns 0 for a wake message, otherwise the default window result.
  // @throws Nothing.
  static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam,
                                     LPARAM lparam);

  // Purpose: Clear the queued-wake flag, then run on_wake.
  // @returns Nothing.
  // @throws Nothing beyond what on_wake throws.
  void RunWake();

  std::function<void()> on_wake_;
  // Serializes the queued-wake flag with the post that sets it, so a request
  // reports a queued wake only after a post has actually succeeded.
  std::mutex wake_mutex_;
  // True from a successful post until the creating thread starts running it.
  // Guarded by wake_mutex_.
  bool wake_queued_ = false;
  // The module that holds this code and owns the window class; null when it
  // could not be found, which leaves IsReady() false.
  HMODULE module_ = nullptr;
  HWND window_ = nullptr;
};

}  // namespace simple_query_windows

#endif  // SIMPLE_QUERY_WINDOWS_PLATFORM_THREAD_DISPATCHER_H_
