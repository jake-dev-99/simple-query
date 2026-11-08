// Native tests for the Windows Runtime store helpers: waiting on a store call
// that can be stopped, and joining an apartment on each thread that reads a
// store.

#include <windows.h>
#include <objbase.h>

#include <gtest/gtest.h>

#include <optional>
#include <thread>

#include <winrt/Windows.Foundation.h>

#include "winrt_records.h"

namespace simple_query_windows {
namespace test {
namespace {

using winrt::Windows::Foundation::AsyncStatus;

constexpr int kFakeResult = 42;

// Purpose: Stand in for a Windows Runtime call that finishes after a set
// number of status checks, recording whether it was cancelled.
// @param none; its constructor documents its own parameters.
// @returns Nothing; this is a class.
// @throws Nothing; its members document what they throw.
class FakeOperation {
 public:
  // Purpose: Describe a call that finishes on a given status check.
  // @param checks_until_done is how many checks report it still running;
  // a negative count means it never finishes.
  // @returns Nothing.
  // @throws Nothing.
  explicit FakeOperation(int checks_until_done)
      : checks_until_done_(checks_until_done) {}

  // Purpose: Report the call's status, counting the check.
  // @returns Started until the set number of checks has passed, then
  // Completed.
  // @throws Nothing.
  AsyncStatus Status() const {
    ++checks_;
    if (checks_until_done_ < 0 || checks_ <= checks_until_done_) {
      return AsyncStatus::Started;
    }
    return AsyncStatus::Completed;
  }

  // Purpose: Record that the call was cancelled.
  // @returns Nothing.
  // @throws Nothing.
  void Cancel() const { cancelled_ = true; }

  // Purpose: Give the call's result.
  // @returns The fixed fake result.
  // @throws Nothing.
  int GetResults() const { return kFakeResult; }

  // Purpose: Report whether the call was cancelled.
  // @returns true once Cancel ran.
  // @throws Nothing.
  bool cancelled() const { return cancelled_; }

 private:
  int checks_until_done_;
  // AwaitUnlessStopped takes the call as const, as C++/WinRT calls are.
  mutable int checks_ = 0;
  mutable bool cancelled_ = false;
};

// Purpose: Hold the apartment a thread is in, as COM reports it.
// @param result is E_FAIL until CoGetApartmentType runs, and S_OK when
// type and qualifier are valid.
// @param type is APTTYPE_CURRENT until CoGetApartmentType sets it.
// @param qualifier tells an implicit multithreaded apartment from an
// explicit one.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct ApartmentState {
  HRESULT result = E_FAIL;
  APTTYPE type = APTTYPE_CURRENT;
  APTTYPEQUALIFIER qualifier = APTTYPEQUALIFIER_NONE;
};

// Purpose: Ask COM which apartment the calling thread is in.
// @returns The answer, with the call's result.
// @throws Nothing.
ApartmentState CurrentApartment() {
  ApartmentState state;
  state.result = CoGetApartmentType(&state.type, &state.qualifier);
  return state;
}

// Purpose: Report whether an apartment type is single-threaded.
// @param type is APTTYPE_CURRENT until CoGetApartmentType sets it.
// @returns true for an ordinary or main single-threaded apartment.
// @throws Nothing.
bool IsSingleThreaded(APTTYPE type) {
  return type == APTTYPE_STA || type == APTTYPE_MAINSTA;
}

// Purpose: A call that finishes is waited out and its result returned.
TEST(AwaitUnlessStoppedTest, ReturnsTheResultOnceTheCallFinishes) {
  const FakeOperation operation(3);

  const std::optional<int> result = AwaitUnlessStopped(operation, StopRequested{});

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, kFakeResult);
  EXPECT_FALSE(operation.cancelled());
}

// Purpose: A stop cancels the call in flight and returns nothing.
TEST(AwaitUnlessStoppedTest, CancelsTheCallWhenAskedToStop) {
  const FakeOperation operation(-1);
  int stop_checks = 0;

  const std::optional<int> result =
      AwaitUnlessStopped(operation, [&stop_checks]() { return ++stop_checks > 2; });

  EXPECT_FALSE(result.has_value());
  EXPECT_TRUE(operation.cancelled());
  EXPECT_EQ(stop_checks, 3);
}

// Purpose: A worker thread joins the multithreaded apartment for the scope,
// and leaves it afterwards.
TEST(WinRtApartmentScopeTest, JoinsAndLeavesTheMultithreadedApartment) {
  ApartmentState inside;
  HRESULT after = E_FAIL;
  bool threw = false;
  std::thread worker([&]() {
    try {
      const WinRtApartmentScope apartment;
      inside = CurrentApartment();
    } catch (const winrt::hresult_error&) {
      threw = true;
    }
    // S_OK means the thread was in no apartment, so the scope left it.
    after = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(after)) {
      CoUninitialize();
    }
  });
  worker.join();

  ASSERT_FALSE(threw);
  EXPECT_EQ(inside.result, S_OK);
  EXPECT_EQ(inside.type, APTTYPE_MTA);
  EXPECT_EQ(inside.qualifier, APTTYPEQUALIFIER_NONE);
  EXPECT_EQ(after, S_OK);
}

// Purpose: A thread already in a single-threaded apartment, such as the
// platform thread, keeps it during and after the scope.
TEST(WinRtApartmentScopeTest, LeavesASingleThreadedApartmentAlone) {
  HRESULT joined = E_FAIL;
  ApartmentState inside;
  ApartmentState after;
  bool threw = false;
  std::thread worker([&]() {
    joined = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    try {
      const WinRtApartmentScope apartment;
      inside = CurrentApartment();
    } catch (const winrt::hresult_error&) {
      threw = true;
    }
    after = CurrentApartment();
    if (SUCCEEDED(joined)) {
      CoUninitialize();
    }
  });
  worker.join();

  ASSERT_EQ(joined, S_OK);
  ASSERT_FALSE(threw);
  EXPECT_EQ(inside.result, S_OK);
  EXPECT_TRUE(IsSingleThreaded(inside.type));
  EXPECT_EQ(after.result, S_OK);
  EXPECT_TRUE(IsSingleThreaded(after.type));
}

}  // namespace
}  // namespace test
}  // namespace simple_query_windows
