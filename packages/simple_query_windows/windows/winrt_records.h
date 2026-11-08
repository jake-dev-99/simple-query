#ifndef SIMPLE_QUERY_WINDOWS_WINRT_RECORDS_H_
#define SIMPLE_QUERY_WINDOWS_WINRT_RECORDS_H_

#include <flutter/encodable_value.h>

#include <optional>
#include <string>
#include <vector>

#include "stop_requested.h"

namespace simple_query_windows {

// Purpose: Hold the rows a Windows Runtime store returned, or why it could
// not be read.
struct WinRtRowsResult {
  std::vector<flutter::EncodableMap> rows;
  // Set when the store could not be read; rows is then empty or partial.
  std::optional<std::string> error;
};

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
