#include "platform_thread_dispatcher.h"

#include <mutex>
#include <utility>

namespace simple_query_windows {

namespace {

constexpr UINT kWakeMessage = WM_APP + 1;
constexpr const wchar_t* kWindowClassName =
    L"SimpleQueryWindowsPlatformThreadDispatcher";
// Its address lies in this module, which identifies the module.
const char kModuleAnchor = 0;

// Purpose: Guard the window class, so one dispatcher cannot unregister it
// between another's registration and window creation.
// @returns The process-wide window class mutex.
// @throws Nothing.
std::mutex& WindowClassMutex() {
  static std::mutex mutex;
  return mutex;
}

// Purpose: Find the module that holds this code. The plugin is a DLL, so the
// executable's handle would register the class for the app instead.
// @returns The module handle, or null when it cannot be found.
// @throws Nothing.
HMODULE CodeModule() {
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&kModuleAnchor), &module)) {
    return nullptr;
  }
  return module;
}

}  // namespace

// Purpose: Register the window class and create the message-only window that
// carries wake requests to the creating thread.
// @param on_wake runs on the creating thread once per delivered wake.
// @returns Nothing; IsReady() reports whether the window exists.
// @throws Nothing; a window that cannot be created leaves IsReady() false.
PlatformThreadDispatcher::PlatformThreadDispatcher(
    std::function<void()> on_wake)
    : on_wake_(std::move(on_wake)), module_(CodeModule()) {
  if (module_ == nullptr) {
    return;
  }
  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = &PlatformThreadDispatcher::WindowProc;
  window_class.hInstance = module_;
  window_class.lpszClassName = kWindowClassName;
  std::lock_guard<std::mutex> lock(WindowClassMutex());
  // While another dispatcher exists, the class is already registered; this
  // call then fails with ERROR_CLASS_ALREADY_EXISTS and the class stays usable.
  RegisterClassExW(&window_class);
  window_ = CreateWindowExW(0, kWindowClassName, L"", 0, 0, 0, 0, 0,
                            HWND_MESSAGE, nullptr, module_, nullptr);
  if (window_ != nullptr) {
    SetWindowLongPtrW(window_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  }
}

// Purpose: Detach and destroy the window, then unregister the class when no
// other dispatcher's window still uses it.
// @returns Nothing.
// @throws Nothing.
PlatformThreadDispatcher::~PlatformThreadDispatcher() {
  if (module_ == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(WindowClassMutex());
  if (window_ != nullptr) {
    // Detach first: the system discards messages queued for a destroyed
    // window, and a detached window ignores any it still receives.
    SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
    DestroyWindow(window_);
  }
  // Fails with ERROR_CLASS_HAS_WINDOWS while another dispatcher's window
  // exists, so only the last dispatcher destroyed removes the class.
  UnregisterClassW(kWindowClassName, module_);
}

// Purpose: Report whether wake requests can reach the creating thread.
// @returns true when the message window exists.
// @throws Nothing.
bool PlatformThreadDispatcher::IsReady() const { return window_ != nullptr; }

// Purpose: Queue one wake on the creating thread, merging requests made
// before the queued wake runs.
// @returns true when a wake is queued; false with GetLastError() set when
// nothing could be queued.
// @throws Nothing.
bool PlatformThreadDispatcher::RequestWake() {
  if (window_ == nullptr) {
    SetLastError(ERROR_INVALID_WINDOW_HANDLE);
    return false;
  }
  if (wake_queued_.exchange(true)) {
    return true;
  }
  if (PostMessageW(window_, kWakeMessage, 0, 0) != 0) {
    return true;
  }
  // Nothing was queued, so the next request must post again. The store does
  // not touch the thread's last-error value that PostMessageW set.
  wake_queued_.store(false);
  return false;
}

// Purpose: Run the dispatcher's wake for a wake message and pass every other
// message to the default window procedure.
// @param window is the dispatcher's message-only window.
// @param message is the message identifier.
// @param wparam is the message's first parameter.
// @param lparam is the message's second parameter.
// @returns 0 for a wake message, otherwise the default window result.
// @throws Nothing.
LRESULT CALLBACK PlatformThreadDispatcher::WindowProc(HWND window,
                                                      UINT message,
                                                      WPARAM wparam,
                                                      LPARAM lparam) {
  if (message != kWakeMessage) {
    return DefWindowProcW(window, message, wparam, lparam);
  }
  auto* dispatcher = reinterpret_cast<PlatformThreadDispatcher*>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (dispatcher != nullptr) {
    dispatcher->RunWake();
  }
  return 0;
}

// Purpose: Clear the queued-wake flag, then run on_wake.
// @returns Nothing.
// @throws Nothing beyond what on_wake throws.
void PlatformThreadDispatcher::RunWake() {
  // Clear before running, so a request made while on_wake runs queues a new
  // wake instead of being absorbed by this one.
  wake_queued_.store(false);
  on_wake_();
}

}  // namespace simple_query_windows
