#ifndef SIMPLE_QUERY_WINDOWS_WINRT_RECORDS_H_
#define SIMPLE_QUERY_WINDOWS_WINRT_RECORDS_H_

#include <flutter/encodable_value.h>

#include <chrono>
#include <optional>
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
// apartment, such as the platform thread, keeps it.
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
