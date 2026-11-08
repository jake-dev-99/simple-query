#include "winrt_records.h"

// Declares RPC_E_CHANGED_MODE; C++/WinRT includes no Windows SDK headers.
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <mutex>
#include <utility>

#include <winrt/Windows.ApplicationModel.Appointments.h>
#include <winrt/Windows.ApplicationModel.Contacts.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/base.h>

namespace simple_query_windows {

namespace {

using flutter::EncodableList;
using flutter::EncodableMap;
using flutter::EncodableValue;

// How often a store call in flight checks whether its read should stop.
constexpr std::chrono::milliseconds kStopCheckInterval{50};

// Purpose: Join the process's multithreaded Windows Runtime apartment once.
// @returns Nothing.
// @throws winrt::hresult_error when the apartment cannot be initialized for a
// reason other than the calling thread already being in another apartment.
void EnsureWinRtApartment() {
  static std::once_flag flag;
  std::call_once(flag, []() {
    try {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (const winrt::hresult_error& error) {
      // RPC_E_CHANGED_MODE means the thread already joined a single-threaded
      // apartment, such as the platform thread; store calls still work there.
      if (static_cast<int32_t>(error.code()) != RPC_E_CHANGED_MODE) {
        throw;
      }
    }
  });
}

// Purpose: Convert a Windows Runtime time to milliseconds since the Unix epoch.
// @param value is the Windows Runtime time.
// @returns Milliseconds since 1970-01-01 UTC.
// @throws Nothing.
int64_t ToUnixEpochMs(winrt::Windows::Foundation::DateTime value) {
  constexpr int64_t kWinRtToUnixEpochTicks = 116444736000000000LL;
  const int64_t ticks = value.time_since_epoch().count();
  return (ticks - kWinRtToUnixEpochTicks) / 10000;
}

// Purpose: Wait for a store call, cancelling it once the read should stop.
// @param operation is the store call in flight.
// @param stop_requested ends the wait early; empty means never.
// @returns The call's result, or nothing when the read stopped first.
// @throws winrt::hresult_error when the call failed.
template <typename Operation>
auto AwaitUnlessStopped(const Operation& operation, const StopRequested& stop_requested)
    -> std::optional<decltype(operation.GetResults())> {
  while (operation.wait_for(kStopCheckInterval) ==
         winrt::Windows::Foundation::AsyncStatus::Started) {
    if (IsStopRequested(stop_requested)) {
      operation.Cancel();
      return std::nullopt;
    }
  }
  return operation.GetResults();
}

// Purpose: Build the query row for one contact.
// @param contact is the contact.
// @returns The row, or nothing for a contact without an id.
// @throws winrt::hresult_error when a contact field cannot be read.
std::optional<EncodableMap> ContactRow(
    const winrt::Windows::ApplicationModel::Contacts::Contact& contact) {
  std::string id = winrt::to_string(contact.Id());
  if (id.empty()) {
    return std::nullopt;
  }

  std::string display_name = winrt::to_string(contact.DisplayName());
  if (display_name.empty()) {
    display_name = winrt::to_string(contact.FirstName());
    const auto last_name = winrt::to_string(contact.LastName());
    if (!last_name.empty()) {
      if (!display_name.empty()) {
        display_name += " ";
      }
      display_name += last_name;
    }
  }

  EncodableList phones;
  for (const auto& phone : contact.Phones()) {
    phones.push_back(EncodableValue(winrt::to_string(phone.Number())));
  }

  EncodableList emails;
  for (const auto& email : contact.Emails()) {
    emails.push_back(EncodableValue(winrt::to_string(email.Address())));
  }

  EncodableMap row;
  row[EncodableValue("id")] = EncodableValue(id);
  row[EncodableValue("displayName")] = EncodableValue(display_name);
  row[EncodableValue("phones")] = EncodableValue(phones);
  row[EncodableValue("emails")] = EncodableValue(emails);

  if (contact.JobInfo().Size() > 0) {
    const auto company = winrt::to_string(contact.JobInfo().GetAt(0).CompanyName());
    if (!company.empty()) {
      row[EncodableValue("organization")] = EncodableValue(company);
    }
  }
  return row;
}

// Purpose: Build the query row for one appointment.
// @param appointment is the appointment.
// @param calendar_id is the id of the calendar that holds it.
// @returns The row.
// @throws winrt::hresult_error when an appointment field cannot be read.
EncodableMap AppointmentRow(
    const winrt::Windows::ApplicationModel::Appointments::Appointment& appointment,
    const std::string& calendar_id) {
  const auto start_time = appointment.StartTime();
  const auto end_time = start_time + appointment.Duration();
  const int64_t start_ms = ToUnixEpochMs(start_time);
  const int64_t end_ms = ToUnixEpochMs(end_time);

  std::string appointment_id = winrt::to_string(appointment.LocalId());
  if (appointment_id.empty()) {
    appointment_id = calendar_id + ":" + std::to_string(start_ms);
  }

  EncodableMap row;
  row[EncodableValue("id")] = EncodableValue(appointment_id);
  row[EncodableValue("title")] = EncodableValue(winrt::to_string(appointment.Subject()));
  row[EncodableValue("startAt")] = EncodableValue(std::to_string(start_ms));
  row[EncodableValue("endAt")] = EncodableValue(std::to_string(end_ms));
  row[EncodableValue("isAllDay")] = EncodableValue(appointment.AllDay());
  row[EncodableValue("calendarId")] = EncodableValue(calendar_id);
  row[EncodableValue("updatedAt")] = EncodableValue(std::to_string(start_ms));
  return row;
}

}  // namespace

WinRtRowsResult ListContactRecords(const StopRequested& stop_requested) {
  using namespace winrt::Windows::ApplicationModel::Contacts;
  WinRtRowsResult result;
  try {
    EnsureWinRtApartment();

    const auto store = AwaitUnlessStopped(
        ContactManager::RequestStoreAsync(ContactStoreAccessType::AllContactsReadOnly),
        stop_requested);
    if (!store.has_value()) {
      return result;
    }
    const auto contacts = AwaitUnlessStopped(store->FindContactsAsync(), stop_requested);
    if (!contacts.has_value()) {
      return result;
    }

    for (const auto& contact : *contacts) {
      if (IsStopRequested(stop_requested)) {
        break;
      }
      auto row = ContactRow(contact);
      if (row.has_value()) {
        result.rows.push_back(std::move(*row));
      }
    }
  } catch (const winrt::hresult_error& error) {
    result.error = "simple_query: windows contacts backend unavailable - " +
                   winrt::to_string(error.message());
  } catch (const std::exception& error) {
    result.error = "simple_query: windows contacts backend unavailable - " +
                   std::string(error.what());
  }
  return result;
}

WinRtRowsResult ListCalendarRecords(const StopRequested& stop_requested) {
  using namespace winrt::Windows::ApplicationModel::Appointments;
  WinRtRowsResult result;
  try {
    EnsureWinRtApartment();

    const auto store = AwaitUnlessStopped(
        AppointmentManager::RequestStoreAsync(AppointmentStoreAccessType::AllCalendarsReadOnly),
        stop_requested);
    if (!store.has_value()) {
      return result;
    }
    const auto calendars =
        AwaitUnlessStopped(store->FindAppointmentCalendarsAsync(), stop_requested);
    if (!calendars.has_value()) {
      return result;
    }

    const auto now = winrt::clock::now();
    // C++/WinRT projects TimeSpan as a std::chrono::duration, so it is
    // converted directly rather than through the ABI Duration field.
    const auto lookback =
        std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(
            std::chrono::hours(24 * 365));
    const auto start = now - lookback;
    const auto range =
        std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(
            std::chrono::hours(24 * 365 * 2));

    for (const auto& calendar : *calendars) {
      const auto appointments =
          AwaitUnlessStopped(calendar.FindAppointmentsAsync(start, range), stop_requested);
      if (!appointments.has_value()) {
        return result;
      }
      const std::string calendar_id = winrt::to_string(calendar.LocalId());
      for (const auto& appointment : *appointments) {
        if (IsStopRequested(stop_requested)) {
          return result;
        }
        result.rows.push_back(AppointmentRow(appointment, calendar_id));
      }
    }
  } catch (const winrt::hresult_error& error) {
    result.error = "simple_query: windows calendar backend unavailable - " +
                   winrt::to_string(error.message());
  } catch (const std::exception& error) {
    result.error = "simple_query: windows calendar backend unavailable - " +
                   std::string(error.what());
  }
  return result;
}

EncodableList ListContactStores() {
  using namespace winrt::Windows::ApplicationModel::Contacts;
  EnsureWinRtApartment();

  const auto store =
      ContactManager::RequestStoreAsync(ContactStoreAccessType::AllContactsReadOnly).get();
  const auto lists = store.FindContactListsAsync().get();

  EncodableList stores;
  for (const auto& list : lists) {
    EncodableMap row;
    row[EncodableValue("id")] = EncodableValue(winrt::to_string(list.Id()));
    row[EncodableValue("name")] = EncodableValue(winrt::to_string(list.DisplayName()));
    stores.push_back(EncodableValue(row));
  }
  return stores;
}

EncodableList ListCalendarStores() {
  using namespace winrt::Windows::ApplicationModel::Appointments;
  EnsureWinRtApartment();

  const auto store =
      AppointmentManager::RequestStoreAsync(AppointmentStoreAccessType::AllCalendarsReadOnly)
          .get();
  const auto calendars = store.FindAppointmentCalendarsAsync().get();

  EncodableList rows;
  for (const auto& calendar : calendars) {
    EncodableMap row;
    row[EncodableValue("id")] = EncodableValue(winrt::to_string(calendar.LocalId()));
    row[EncodableValue("title")] = EncodableValue(winrt::to_string(calendar.DisplayName()));
    rows.push_back(EncodableValue(row));
  }
  return rows;
}

}  // namespace simple_query_windows
