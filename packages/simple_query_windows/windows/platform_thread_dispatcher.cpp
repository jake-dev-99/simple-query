#include "platform_thread_dispatcher.h"

#include <utility>

namespace simple_query_windows {

namespace {

constexpr UINT kWakeMessage = WM_APP + 1;
constexpr const wchar_t* kWindowClassName =
    L"SimpleQueryWindowsPlatformThreadDispatcher";

}  // namespace

PlatformThreadDispatcher::PlatformThreadDispatcher(
    std::function<void()> on_wake)
    : on_wake_(std::move(on_wake)) {
  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = &PlatformThreadDispatcher::WindowProc;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.lpszClassName = kWindowClassName;
  // Every dispatcher after the first registers the same class again; that
  // call fails with ERROR_CLASS_ALREADY_EXISTS and the class stays usable.
  RegisterClassExW(&window_class);
  window_ = CreateWindowExW(0, kWindowClassName, L"", 0, 0, 0, 0, 0,
                            HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr),
                            nullptr);
  if (window_ != nullptr) {
    SetWindowLongPtrW(window_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  }
}

PlatformThreadDispatcher::~PlatformThreadDispatcher() {
  if (window_ != nullptr) {
    // Detach first: the system discards messages queued for a destroyed
    // window, and a detached window ignores any it still receives.
    SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
    DestroyWindow(window_);
  }
}

bool PlatformThreadDispatcher::IsReady() const { return window_ != nullptr; }

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

void PlatformThreadDispatcher::RunWake() {
  // Clear before running, so a request made while on_wake runs queues a new
  // wake instead of being absorbed by this one.
  wake_queued_.store(false);
  on_wake_();
}

}  // namespace simple_query_windows
