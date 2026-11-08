#ifndef SIMPLE_QUERY_WINDOWS_WINRT_RECORDS_H_
#define SIMPLE_QUERY_WINDOWS_WINRT_RECORDS_H_

#include <flutter/encodable_value.h>

#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <winrt/Windows.Foundation.h>

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

// Purpose: Hold the rows a Windows Runtime store returned, or why it could
// not be read.
struct WinRtRowsResult {
  std::vector<flutter::EncodableMap> rows;
  // Set when the store could not be read; rows is then empty or partial.
  std::optional<std::string> error;
};

// Purpose: Give a store read's rows, or fail when the store could not be
// read, so a failed read is never mistaken for an empty store.
// @param result is the store read's outcome.
// @returns The rows read.
// @throws std::runtime_error carrying the read's error when it failed.
std::vector<flutter::EncodableMap> RowsOrThrow(WinRtRowsResult result);

// Purpose: List the user's contacts as query rows.
// @param stop_requested ends the read early, cancelling any store call in
// flight; empty means never.
// @returns The contact rows, the rows read before a stop, or an error.
// @throws Nothing; store failures are returned in error.
WinRtRowsResult ListContactRecords(const StopRequested& stop_requested);

// Purpose: List the appointments from a year back to a year ahead as rows.
// @param stop_requested ends the read early, cancelling any store call in
// flight; empty means never.
// @returns The appointment rows, the rows read before a stop, or an error.
// @throws Nothing; store failures are returned in error.
WinRtRowsResult ListCalendarRecords(const StopRequested& stop_requested);

// Purpose: List the user's contact lists.
// @returns One row per contact list, with its id and name.
// @throws winrt::hresult_error when the contact store cannot be read.
flutter::EncodableList ListContactStores();

// Purpose: List the user's appointment calendars.
// @returns One row per calendar, with its id and title.
// @throws winrt::hresult_error when the appointment store cannot be read.
flutter::EncodableList ListCalendarStores();

}  // namespace simple_query_windows

#endif  // SIMPLE_QUERY_WINDOWS_WINRT_RECORDS_H_
