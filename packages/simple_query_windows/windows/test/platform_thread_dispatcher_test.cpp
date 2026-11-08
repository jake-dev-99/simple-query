// Native tests for the platform-thread dispatcher.

#include "platform_thread_dispatcher.h"

#include <gtest/gtest.h>
#include <windows.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "test/dispatcher_probe.h"
#include "test/test_support.h"

namespace simple_query_windows {
namespace test {
namespace {

constexpr const wchar_t* kDispatcherWindowClass =
    L"SimpleQueryWindowsPlatformThreadDispatcher";
constexpr const wchar_t* kProbeLibrary = L"simple_query_windows_dispatcher_probe.dll";

// Purpose: Report whether a module has the dispatcher's window class.
// @param module is the module to look in.
// @returns true when the class is registered for that module.
// @throws Nothing.
bool HasDispatcherClass(HMODULE module) {
  WNDCLASSEXW info{};
  info.cbSize = sizeof(info);
  return GetClassInfoExW(module, kDispatcherWindowClass, &info) != FALSE;
}

// Purpose: Own the loaded probe library and free it afterwards.
// @param none; its constructor documents its own parameters.
// @returns Nothing; this is a class.
// @throws Nothing; its members document what they throw.
class ProbeLibrary {
 public:
  // Purpose: Load the probe library from the test runner's directory.
  // @returns Nothing; module() is null when it could not be loaded.
  // @throws Nothing.
  ProbeLibrary() : module_(LoadLibraryW(kProbeLibrary)) {}

  // Purpose: Unload the probe library.
  // @returns Nothing.
  // @throws Nothing.
  ~ProbeLibrary() {
    if (module_ != nullptr) {
      FreeLibrary(module_);
    }
  }

  ProbeLibrary(const ProbeLibrary&) = delete;
  ProbeLibrary& operator=(const ProbeLibrary&) = delete;

  // Purpose: Give the loaded module.
  // @returns The module, or null when it could not be loaded.
  // @throws Nothing.
  HMODULE module() const { return module_; }

  // Purpose: Find one of the probe's exported functions.
  // @param name is the export's name.
  // @returns The function, or null when it is missing.
  // @throws Nothing.
  template <typename Function>
  Function Export(const char* name) const {
    // GetProcAddress returns a generic function pointer; casting through
    // void (*)() is the documented way to give it the export's real type.
    using GenericFunction = void (*)();
    const auto generic = reinterpret_cast<GenericFunction>(GetProcAddress(module_, name));
    return reinterpret_cast<Function>(generic);
  }

 private:
  HMODULE module_;
};

// Purpose: A wake requested from a worker runs on the creating thread.
TEST(PlatformThreadDispatcherTest, RunsTheWakeOnTheCreatingThread) {
  std::thread::id wake_thread;
  int wakes = 0;
  PlatformThreadDispatcher dispatcher([&]() {
    wake_thread = std::this_thread::get_id();
    ++wakes;
  });
  ASSERT_TRUE(dispatcher.IsReady());

  bool requested = false;
  std::thread worker([&]() { requested = dispatcher.RequestWake(); });
  worker.join();

  ASSERT_TRUE(requested);
  ASSERT_TRUE(PumpUntil([&]() { return wakes == 1; }, kDeliveryTimeout));
  EXPECT_EQ(wake_thread, std::this_thread::get_id());
}

// Purpose: A timer wakes the platform thread without a worker post, so a
// blocked worker cannot prevent its call deadline from being checked.
TEST(PlatformThreadDispatcherTest, TimerWakesWithoutAWorkerPostAndStopsWhenClosed) {
  int wakes = 0;
  PlatformThreadDispatcher dispatcher([&]() { ++wakes; }, std::chrono::milliseconds(10));
  ASSERT_TRUE(dispatcher.IsReady());
  ASSERT_TRUE(PumpUntil([&]() { return wakes >= 2; }, kDeliveryTimeout));

  dispatcher.Close();
  const int before = wakes;
  PumpQueuedMessages();
  EXPECT_EQ(wakes, before);
}

// Purpose: Requests made while a wake is queued add no further messages.
TEST(PlatformThreadDispatcherTest, CoalescesRequestsWhileAWakeIsQueued) {
  int wakes = 0;
  PlatformThreadDispatcher dispatcher([&]() { ++wakes; });

  EXPECT_TRUE(dispatcher.RequestWake());
  EXPECT_TRUE(dispatcher.RequestWake());
  EXPECT_TRUE(dispatcher.RequestWake());
  PumpQueuedMessages();
  EXPECT_EQ(wakes, 1);

  EXPECT_TRUE(dispatcher.RequestWake());
  PumpQueuedMessages();
  EXPECT_EQ(wakes, 2);
}

// Purpose: A full queue is reported, and the next request queues normally.
TEST(PlatformThreadDispatcherTest, ReportsAFullQueueAndRecovers) {
  int wakes = 0;
  PlatformThreadDispatcher dispatcher([&]() { ++wakes; });
  ASSERT_TRUE(FillMessageQueue());

  EXPECT_FALSE(dispatcher.RequestWake());
  PumpQueuedMessages();
  EXPECT_EQ(wakes, 0);

  EXPECT_TRUE(dispatcher.RequestWake());
  PumpQueuedMessages();
  EXPECT_EQ(wakes, 1);
}

// Purpose: While the queue is full, no concurrent request reports a wake as
// queued, so no caller skips a retry for a wake that was never posted.
TEST(PlatformThreadDispatcherTest, ReportsNoQueuedWakeToConcurrentRequestsOnAFullQueue) {
  constexpr int kRequesters = 8;
  constexpr int kRequestsEach = 200;
  int wakes = 0;
  PlatformThreadDispatcher dispatcher([&]() { ++wakes; });
  ASSERT_TRUE(FillMessageQueue());

  std::atomic<int> reported_queued{0};
  std::vector<std::thread> requesters;
  for (int requester = 0; requester < kRequesters; ++requester) {
    requesters.emplace_back([&]() {
      for (int request = 0; request < kRequestsEach; ++request) {
        if (dispatcher.RequestWake()) {
          ++reported_queued;
        }
      }
    });
  }
  for (auto& requester : requesters) {
    requester.join();
  }
  PumpQueuedMessages();

  EXPECT_EQ(reported_queued.load(), 0);
  EXPECT_EQ(wakes, 0);
  EXPECT_TRUE(dispatcher.RequestWake());
  EXPECT_TRUE(PumpUntil([&]() { return wakes == 1; }, kDeliveryTimeout));
}

// Purpose: A wake still queued when the dispatcher goes never runs.
TEST(PlatformThreadDispatcherTest, DropsAQueuedWakeWhenDestroyed) {
  int wakes = 0;
  auto dispatcher =
      std::make_unique<PlatformThreadDispatcher>([&]() { ++wakes; });
  ASSERT_TRUE(dispatcher->RequestWake());

  dispatcher.reset();
  PumpQueuedMessages();

  EXPECT_EQ(wakes, 0);
}

// Purpose: After Close, a wake request from a worker that still holds the
// dispatcher fails and runs nothing, including a wake queued before it.
TEST(PlatformThreadDispatcherTest, RefusesWakesOnceClosed) {
  int wakes = 0;
  auto dispatcher = std::make_shared<PlatformThreadDispatcher>([&]() { ++wakes; });
  ASSERT_TRUE(dispatcher->RequestWake());

  dispatcher->Close();
  bool requested = true;
  std::thread worker([dispatcher, &requested]() { requested = dispatcher->RequestWake(); });
  worker.join();
  PumpQueuedMessages();

  EXPECT_FALSE(requested);
  EXPECT_FALSE(dispatcher->IsReady());
  EXPECT_EQ(wakes, 0);
}

// Purpose: Destroying the last dispatcher removes its window class, and a
// later dispatcher registers it again.
TEST(PlatformThreadDispatcherTest, RemovesItsWindowClassWithTheLastDispatcher) {
  {
    PlatformThreadDispatcher first([]() {});
    PlatformThreadDispatcher second([]() {});
    ASSERT_TRUE(first.IsReady());
    ASSERT_TRUE(second.IsReady());
    EXPECT_TRUE(HasDispatcherClass(GetModuleHandleW(nullptr)));
  }
  EXPECT_FALSE(HasDispatcherClass(GetModuleHandleW(nullptr)));

  PlatformThreadDispatcher again([]() {});
  EXPECT_TRUE(again.IsReady());
}

// Purpose: Destroying one dispatcher leaves another one delivering wakes.
TEST(PlatformThreadDispatcherTest, KeepsDeliveringAfterAnotherIsDestroyed) {
  int wakes = 0;
  PlatformThreadDispatcher survivor([&]() { ++wakes; });
  { PlatformThreadDispatcher destroyed([]() {}); }

  ASSERT_TRUE(survivor.RequestWake());
  EXPECT_TRUE(PumpUntil([&]() { return wakes == 1; }, kDeliveryTimeout));
}

// Purpose: Inside a DLL, as in an app, the dispatcher registers its window
// class for the DLL rather than the executable, and removes it from the DLL
// when destroyed, so an unloaded plugin leaves no class behind.
TEST(PlatformThreadDispatcherTest, RegistersItsWindowClassForTheModuleHoldingIt) {
  ProbeLibrary probe;
  ASSERT_NE(probe.module(), nullptr);
  const auto create =
      probe.Export<CreateProbeDispatcherFunction>(kCreateProbeDispatcherExport);
  const auto destroy =
      probe.Export<DestroyProbeDispatcherFunction>(kDestroyProbeDispatcherExport);
  ASSERT_NE(create, nullptr);
  ASSERT_NE(destroy, nullptr);

  void* dispatcher = create();
  ASSERT_NE(dispatcher, nullptr);
  EXPECT_TRUE(HasDispatcherClass(probe.module()));
  EXPECT_FALSE(HasDispatcherClass(GetModuleHandleW(nullptr)));

  destroy(dispatcher);
  EXPECT_FALSE(HasDispatcherClass(probe.module()));
}

}  // namespace
}  // namespace test
}  // namespace simple_query_windows
