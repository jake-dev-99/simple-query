#ifndef SIMPLE_QUERY_WINDOWS_WINRT_RECORDS_H_
#define SIMPLE_QUERY_WINDOWS_WINRT_RECORDS_H_

#include <windows.h>

#include <flutter/encodable_value.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>

#include <winrt/Windows.Foundation.h>

#include "rows_result.h"
#include "stop_requested.h"

namespace simple_query_windows {

// How often a store call in flight checks whether its read should stop.
inline constexpr std::chrono::milliseconds kStopCheckInterval{50};

// Purpose: Keep the calling thread in a Windows Runtime apartment while the
// object lives. Apartments belong to threads, so every thread that reads a
// store needs its own. A thread that already joined a single-threaded
// apartment keeps it.
// @param none; its constructor documents its own parameters.
// @returns Nothing; this is a class.
// @throws Nothing; its members document what they throw.
class WinRtApartmentScope {
 public:
  // Purpose: Join the multithreaded apartment unless the thread is already in
  // a single-threaded one.
  // @returns Nothing.
  // @throws winrt::hresult_error when the apartment cannot be joined for any
  // other reason.
  WinRtApartmentScope();

  // Purpose: Leave the apartment this object joined, if it joined one.
  // @returns Nothing.
  // @throws Nothing.
  ~WinRtApartmentScope();

  WinRtApartmentScope(const WinRtApartmentScope&) = delete;
  WinRtApartmentScope& operator=(const WinRtApartmentScope&) = delete;

 private:
  bool joined_ = false;
};

// Purpose: Wait for a store call, cancelling it once the read should stop.
// The call's status is polled, because a Windows Runtime call accepts only
// one completion handler and repeated waits would each install another.
// @param operation is the store call in flight.
// @param stop_requested ends the wait early; empty means never.
// @returns The call's result, or nothing when the read stopped first.
// @throws winrt::hresult_error when the call failed or was cancelled
// elsewhere.
template <typename Operation>
auto AwaitUnlessStopped(const Operation& operation, const StopRequested& stop_requested)
    -> std::optional<decltype(operation.GetResults())> {
  while (operation.Status() == winrt::Windows::Foundation::AsyncStatus::Started) {
    if (IsStopRequested(stop_requested)) {
      operation.Cancel();
      return std::nullopt;
    }
    std::this_thread::sleep_for(kStopCheckInterval);
  }
  return operation.GetResults();
}

// Shared implementation detail: production and tests supply the store operations.
namespace winrt_store_detail {

// Purpose: Name one Windows Runtime store for its errors and denials.
// @param backend names the store in errors.
// @param capability identifies the packaged-app permission.
// @param settings_page names its Windows Settings privacy page.
// @returns Nothing; this is a data type.
// @throws Nothing.
struct StoreSpec {
  // The store's name in an error, such as "contacts".
  const char* backend;
  // The packaged-app capability that grants access, such as "contacts".
  const char* capability;
  // The Windows Settings privacy page that turns access on, such as "Contacts".
  const char* settings_page;
};

inline constexpr StoreSpec kContactsStore{
    .backend = "contacts", .capability = "contacts", .settings_page = "Contacts"};
inline constexpr StoreSpec kCalendarStore{
    .backend = "calendar", .capability = "appointments", .settings_page = "Calendar"};

// Purpose: Give the outcome of a store read the user has not allowed.
// @param spec names the store.
// @returns An error naming the capability and how to allow access.
// @throws Nothing.
RowsResult DeniedResult(const StoreSpec& spec);

// Purpose: Give the outcome of a read that stopped before it finished.
// @param none.
// @returns An outcome carrying kReadStoppedError.
// @throws Nothing.
RowsResult StoppedResult();

// Purpose: Name one read of a Windows Runtime store.
// @param spec names the store and its permission.
// @param open opens the store, or returns no value when stopped.
// @param read reads a non-null opened store.
// @returns Nothing; this is a data type.
// @throws Nothing; the operations document their failures.
template <typename Open, typename Read>
struct StoreRead {
  StoreSpec spec;
  // Opens the store; gives nothing when the read stopped, and a null store
  // when the user turned access off.
  Open open;
  // Reads the open store.
  Read read;
};

// Purpose: Open a store in a Windows Runtime apartment, read it, and turn
// a stop, a denial or a failure into the read's outcome.
// @param request names the store, how to open it and how to read it.
// @returns The outcome the read gives, kReadStoppedError, a denial naming
// the capability, or an error when the store fails.
// @throws Nothing; store failures are returned in error.
template <typename Open, typename Read>
RowsResult ReadStore(StoreRead<Open, Read> request) {
  const StoreSpec& spec = request.spec;
  try {
    // Joined first, so every store object below is released before the
    // thread leaves the apartment.
    const WinRtApartmentScope apartment;
    const auto store = request.open();
    if (!store.has_value()) {
      return StoppedResult();
    }
    if (!*store) {
      return DeniedResult(spec);
    }
    return request.read(*store);
  } catch (const winrt::hresult_error& error) {
    // A packaged app without the capability gets E_ACCESSDENIED rather
    // than a null store.
    if (static_cast<int32_t>(error.code()) == E_ACCESSDENIED) {
      return DeniedResult(spec);
    }
    RowsResult result;
    result.error = std::string("simple_query: windows ") + spec.backend +
                   " backend unavailable - " + winrt::to_string(error.message());
    return result;
  }
}

}  // namespace winrt_store_detail

// Purpose: List the user's contacts as query rows.
// @param stop_requested ends the read early, cancelling any store call in
// flight; empty means never.
// @returns The contact rows, or an error when the read failed, stopped or
// was denied. A denial also names the "contacts" capability.
// @throws Nothing; store failures are returned in error.
RowsResult ListContactRecords(const StopRequested& stop_requested);

// Purpose: List the appointments from a year back to a year ahead as rows.
// Times are ISO 8601 UTC, as on the other platforms.
// @param stop_requested ends the read early, cancelling any store call in
// flight; empty means never.
// @returns The appointment rows, or an error when the read failed, stopped
// or was denied. A denial also names the "appointments" capability.
// @throws Nothing; store failures are returned in error.
RowsResult ListCalendarRecords(const StopRequested& stop_requested);

// Purpose: List the user's contact lists.
// @param stop_requested ends the read early; empty means never.
// @returns One row per contact list, with its id and name, or an error as
// ListContactRecords gives one.
// @throws Nothing; store failures are returned in error.
RowsResult ListContactStores(const StopRequested& stop_requested);

// Purpose: List the user's appointment calendars.
// @param stop_requested ends the read early; empty means never.
// @returns One row per calendar, with its id and title, or an error as
// ListCalendarRecords gives one.
// @throws Nothing; store failures are returned in error.
RowsResult ListCalendarStores(const StopRequested& stop_requested);

}  // namespace simple_query_windows

#endif  // SIMPLE_QUERY_WINDOWS_WINRT_RECORDS_H_
