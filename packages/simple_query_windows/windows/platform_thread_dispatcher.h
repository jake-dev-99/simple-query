#ifndef SIMPLE_QUERY_WINDOWS_PLATFORM_THREAD_DISPATCHER_H_
#define SIMPLE_QUERY_WINDOWS_PLATFORM_THREAD_DISPATCHER_H_

#include <windows.h>

#include <chrono>
#include <functional>
#include <mutex>

namespace simple_query_windows {

// Purpose: Name one platform-window timer installation.
// @param window receives the timer messages.
// @param id distinguishes deadline checks from other window messages.
// @param interval_ms is already bounded to Windows timer limits.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct DispatcherTimerRequest {
  HWND window;
  UINT_PTR id;
  UINT interval_ms;
};

// Purpose: Configure optional periodic wakes and their operating-system boundary.
// @param wake_interval is zero for posted wakes only.
// @param install_timer installs the timer; empty selects Windows SetTimer.
// It must not throw and reports false when installation fails.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct PlatformThreadDispatcherOptions {
  std::chrono::milliseconds wake_interval{0};
  std::function<bool(const DispatcherTimerRequest&)> install_timer;
};

// Purpose: Run one callback on the thread that created the dispatcher, when
// any thread asks for it. Flutter's Windows embedder accepts channel messages
// only on the platform thread, so observer workers wake this dispatcher
// instead of sending events themselves.
//
// Wake requests coalesce: while one wake is queued, further requests add
// nothing. The platform thread's message queue therefore holds at most one
// message from each dispatcher, however fast workers report changes.
//
// The dispatcher owns a message-only window. Create it and close it on the
// platform thread, whose message loop delivers the wake. A wake still queued
// when the dispatcher closes is dropped without running.
//
// Worker threads that outlive their owner can share the dispatcher. The
// owner closes it on the platform thread when it goes away; after that every
// wake request fails, so a late request never posts to a destroyed window or
// to another window that reused its handle. A closed dispatcher may then be
// destroyed on any thread.
//
// The window class is registered for the module that holds this code, and
// the last dispatcher destroyed unregisters it, so an unloaded plugin leaves
// no class pointing at its code.
// @param none; its constructor documents its own parameters.
// @returns Nothing; this is a class.
// @throws Nothing; its members document what they throw.
class PlatformThreadDispatcher {
 public:
  // Purpose: Create the message-only window that receives wake requests.
  // @param on_wake runs on the creating thread once per delivered wake.
  // @param options supplies periodic wakes and their timer installer. Setup
  // failure closes the dispatcher so no caller relies on a missing deadline.
  // @returns Nothing; check IsReady() before relying on wake delivery.
  // @throws Nothing; a window that cannot be created leaves IsReady() false.
  explicit PlatformThreadDispatcher(
      std::function<void()> on_wake,
      PlatformThreadDispatcherOptions options = {});

  // Purpose: Close the dispatcher if its owner has not.
  // Destroy an unclosed dispatcher on the creating thread.
  // @returns Nothing.
  // @throws Nothing.
  ~PlatformThreadDispatcher();

  PlatformThreadDispatcher(const PlatformThreadDispatcher&) = delete;
  PlatformThreadDispatcher& operator=(const PlatformThreadDispatcher&) = delete;

  // Purpose: Report whether wake requests can reach the creating thread.
  // @returns true when the message window exists.
  // @throws Nothing.
  bool IsReady() const;

  // Purpose: Destroy the window, so that no queued or later wake runs. Call
  // it on the creating thread. Closing twice does nothing.
  // @returns Nothing.
  // @throws Nothing.
  void Close();

  // Purpose: Ask the creating thread to run on_wake. Safe from any thread.
  // @returns true when a wake is queued, either by this call or by an earlier
  // one that has not run yet. false when the wake could not be queued, for
  // example because the thread's message queue is full or the dispatcher is
  // closed; GetLastError() on the calling thread then holds the reason.
  // Nothing is queued after a false return, so the caller must ask again
  // later.
  // @throws Nothing.
  bool RequestWake();

 private:
  // Purpose: Receive wake and timer messages on the creating thread.
  // @param window is the dispatcher's message-only window.
  // @param message is the message identifier.
  // @param wparam is the message's first parameter.
  // @param lparam is the message's second parameter.
  // @returns 0 for a wake message, otherwise the default window result.
  // @throws Nothing.
  // Windows fixes this signature for every window procedure.
  static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam,
                                     LPARAM lparam);

  // Purpose: Clear the queued-wake flag, then run on_wake.
  // @returns Nothing.
  // @throws Nothing beyond what on_wake throws.
  void RunWake();

  std::function<void()> on_wake_;
  // Serializes the queued-wake flag and the window handle with the post that
  // uses them, so a request reports a queued wake only after a post has
  // actually succeeded, and never posts once the window is closing.
  mutable std::mutex wake_mutex_;
  // True from a successful post until the creating thread starts running it.
  // Guarded by wake_mutex_.
  bool wake_queued_ = false;
  // The module that holds this code and owns the window class; null when it
  // could not be found, which leaves IsReady() false.
  HMODULE module_ = nullptr;
  // Null before creation succeeds and after Close. Guarded by wake_mutex_.
  HWND window_ = nullptr;
  // Set by the first Close, so the window class is released once.
  bool closed_ = false;
};

}  // namespace simple_query_windows

#endif  // SIMPLE_QUERY_WINDOWS_PLATFORM_THREAD_DISPATCHER_H_
