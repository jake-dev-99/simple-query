// Native tests for the Windows Runtime store helpers: waiting on a store call
// that can be stopped, and joining an apartment on each thread that reads a
// store.

#include <windows.h>
#include <objbase.h>

#include <gtest/gtest.h>

#include <optional>
#include <thread>
#include <string>

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

// Purpose: Pair a production store specification with its expected denial contract.
// @param spec is the production store identity used by ReadStore.
// @param capability is the independently expected permission identifier.
// @param denied_error is the independently expected denial message.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct DenialCase {
  winrt_store_detail::StoreSpec spec;
  const char* capability;
  const char* denied_error;
};

// Purpose: Exercise the real store-opening and reading boundary for both stores.
// @param none; GoogleTest supplies one DenialCase per test instance.
// @returns Nothing; this is a test fixture.
// @throws Nothing; test operations document their failures.
class ReadStoreDenialTest : public testing::TestWithParam<DenialCase> {};

using winrt_store_detail::ReadStore;
using winrt_store_detail::StoreRead;
// Purpose: Represent a nullable WinRT store handle without opening user data.
// @param none.
// @returns An alias for the base WinRT object interface.
// @throws Nothing.
using TestStore = winrt::Windows::Foundation::IInspectable;

// Purpose: A null opened store reports a denial without trying to read it.
TEST_P(ReadStoreDenialTest, NullStoreReturnsDenialAndCapability) {
  bool read_called = false;
  const auto result = ReadStore(StoreRead{
      .spec = GetParam().spec,
      .open = []() { return std::optional<TestStore>{TestStore{nullptr}}; },
      .read = [&](const TestStore&) {
        read_called = true;
        return RowsResult{};
      }});

  EXPECT_FALSE(read_called);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(*result.error, GetParam().denied_error);
  ASSERT_TRUE(result.denied_capability.has_value());
  EXPECT_EQ(*result.denied_capability, GetParam().capability);
  EXPECT_TRUE(result.rows.empty());
}

// Purpose: E_ACCESSDENIED from opening reports the store's denial contract.
TEST_P(ReadStoreDenialTest, AccessDeniedWhileOpeningReturnsDenialAndCapability) {
  bool read_called = false;
  const auto result = ReadStore(StoreRead{
      .spec = GetParam().spec,
      .open = []() -> std::optional<TestStore> {
        throw winrt::hresult_error(E_ACCESSDENIED);
      },
      .read = [&](const TestStore&) {
        read_called = true;
        return RowsResult{};
      }});

  EXPECT_FALSE(read_called);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(*result.error, GetParam().denied_error);
  ASSERT_TRUE(result.denied_capability.has_value());
  EXPECT_EQ(*result.denied_capability, GetParam().capability);
  EXPECT_TRUE(result.rows.empty());
}

// Purpose: E_ACCESSDENIED from reading reports the same denial contract.
TEST_P(ReadStoreDenialTest, AccessDeniedWhileReadingReturnsDenialAndCapability) {
  bool read_called = false;
  const auto result = ReadStore(StoreRead{
      .spec = GetParam().spec,
      .open = []() -> std::optional<TestStore> {
        return winrt::Windows::Foundation::PropertyValue::CreateInt32(1);
      },
      .read = [&](const TestStore&) -> RowsResult {
        read_called = true;
        throw winrt::hresult_error(E_ACCESSDENIED);
      }});

  EXPECT_TRUE(read_called);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(*result.error, GetParam().denied_error);
  ASSERT_TRUE(result.denied_capability.has_value());
  EXPECT_EQ(*result.denied_capability, GetParam().capability);
  EXPECT_TRUE(result.rows.empty());
}

// Purpose: Other HRESULT failures remain backend errors at either operation.
TEST_P(ReadStoreDenialTest, UnrelatedHresultIsNotClassifiedAsPermissionDenied) {
  for (const bool fail_while_opening : {true, false}) {
    bool read_called = false;
    const auto result = ReadStore(StoreRead{
        .spec = GetParam().spec,
        .open = [fail_while_opening]() -> std::optional<TestStore> {
          if (fail_while_opening) {
            throw winrt::hresult_error(E_FAIL, L"synthetic failure");
          }
          return winrt::Windows::Foundation::PropertyValue::CreateInt32(1);
        },
        .read = [&](const TestStore&) -> RowsResult {
          read_called = true;
          throw winrt::hresult_error(E_FAIL, L"synthetic failure");
        }});

    EXPECT_EQ(read_called, !fail_while_opening);
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(*result.error, std::string("simple_query: windows ") +
                                 GetParam().spec.backend +
                                 " backend unavailable - synthetic failure");
    EXPECT_FALSE(result.denied_capability.has_value());
    EXPECT_TRUE(result.rows.empty());
  }
}

INSTANTIATE_TEST_SUITE_P(
    ContactAndCalendarStores, ReadStoreDenialTest,
    testing::Values(
        DenialCase{winrt_store_detail::kContactsStore, "contacts",
                   "simple_query: windows contacts access is denied. Turn on "
                   "Contacts access for this app in Windows Settings > Privacy & security."},
        DenialCase{winrt_store_detail::kCalendarStore, "appointments",
                   "simple_query: windows calendar access is denied. Turn on "
                   "Calendar access for this app in Windows Settings > Privacy & security."}));

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
